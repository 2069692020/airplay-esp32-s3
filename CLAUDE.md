# CLAUDE.md

This file provides guidance to AI coding agents (and humans) when working with code
in this repository.

## Project Overview

ESP32 AirPlay 2 Receiver — firmware that turns ESP32/ESP32-S2/ESP32-S3/ESP32-P4/
ESP32-C5 boards into AirPlay 2 speakers. Supports ALAC and AAC decoding, Bluetooth
A2DP (classic-BT ESP32 boards only), W5500 Ethernet, OLED/TFT displays, hardware
buttons, network radio, local playback from an SD card, and a browser UI.

**This tree is a working fork of [`rbouteiller/airplay-esp32`](https://github.com/rbouteiller/airplay-esp32),
built for one specific machine**: an ESP32-S3 N16R8 driving a PCM5102A DAC and a
128×64 SSD1306 OLED, plus a SPI SD-card reader and an AHT20 temperature/humidity
sensor. Everything downstream of the AirPlay pipeline (SD playback, web radio, m3u
playlists, GBK↔UTF-8 filenames, CJK font, Chinese web UI, AHT20, SNTP wall clock,
always-on hotspot) is local work on top of upstream.

Audio chain: `RTSP session → decoder → jitter buffer → I2S → PCM5102A → TDA1308 → headphone out`

源码在 git 里，但"改坏了"这件事通常是在设备上才发现 —— 所以每轮动手前要给自己留
一份可回退的基线，见下面「硬件改动纪律」。

## 硬件改动纪律（动手前先留退路）

仓库本身是 git，但 **git 回退不了一台设备**：源码能 `checkout`，已经烧进 flash 的
镜像和 NVS 不能。所以改这套固件时的规矩是：

1. **每轮动手前留一份基线**：当前源码状态 + **实际烧录并测过的那一组镜像**
   （bootloader / partition-table / ota_data_initial / app / storage）+ 一句"怎么退回去"。
   提交者本地放在被 gitignore 的 `firmware_backup/<日期>_<主题>/`，仓库里不需要。
2. **每批改动重新留基线，不是一轮只做一次。** 只留了前两批就停下来，第三批就在
   没有对照的情况下改共享文件。
3. **烧录范围跟着改动范围走**：只动 `data/www/` → 单重烧 `storage.bin`
   （`0x820000`）；动了 C 代码 → 烧 app（`0x20000`）；改了分区表/`sdkconfig` 布局 →
   **五个镜像一组烧**（含 `0x19000 ota_data_initial.bin`），否则页面会去请求
   固件里根本不存在的字段。
4. 批量文本替换（`sed` 之类）之后**必须先 diff 再信任**——本项目出现过一次把某个
   helper 的调用点改成它自己、变成无限递归。

## Build & Flash

**This board (daily driver)** — a hand-assembled ESP-IDF 5.5.5 environment that
dodges Git-Bash/MSYS interference:

```bash
python build_fw.py build                    # idf.py build with the right env
python build_fw.py flash --port COM3        # NEVER trust a remembered COM number
python build_fw.py monitor --port COM3
```

Do **not** `source export.sh` / `export.bat` under Git Bash, and do not switch to
IDF 6.x (see Code Quality). Always capture the real exit code
(`... > log 2>&1; echo "EXIT=$?"`) — piping to `tail` reports the pipe's status and
once turned a failed flash into a false success. List ports first:

```bash
python -c "import serial.tools.list_ports as lp; [print(p.device, hex(p.vid or 0)+':'+hex(p.pid or 0)) for p in lp.comports()]"
```

The S3 enumerates as `303A:1001` (USB-Serial-JTAG), so **RTS does reset it** —
opening the port with pyserial also resets the chip (`ESP_RST_SW`). A serial probe
destroys the scene you were trying to observe.

**PlatformIO** (upstream flow, other boards):
```bash
pio run -e <env> -t build          # Build firmware
pio run -e <env> -t upload         # Build + flash via USB
pio run -e <env> -t monitor        # Serial monitor (115200 baud)
pio run -e <env> -t uploadfs       # Flash SPIFFS from data/
```

**ESP-IDF** (native): `idf.py set-target esp32s3 && idf.py build && idf.py -p PORT flash`

## Build Environments

| Environment | Board | Notes |
|---|---|---|
| `esp32s3` | ESP32-S3 + external DAC (e.g. PCM5102A) | **This tree's target** |
| `esp32s3-jtag` | ESP32-S3 with JTAG | Extends esp32s3 |
| `esp32c5-xiao` | Seeed XIAO ESP32-C5 (RISC-V, 8MB) | Needs the pioarduino platform fork |
| `esp32wrover-dev` | Freenove ESP32 WROVER dev board (4MB) | BT defaults layered in |
| `smartamp` | ESP32 (esp-wrover-kit board def, 4MB) | BT + I2S codec defaults |
| `waveshare-esp32s3` | Waveshare ESP32-S3 (16MB flash) | Extends esp32s3 |
| `squeezeamp` / `-bt` / `-4m` | ESP32 + TAS5756 DAC/amp | `-bt` adds A2DP |
| `esparagus-audio-brick` / `-bt` / `-s3` | ESP32(+TAS5825M) | `-bt` adds A2DP + Ethernet |
| `esparagus-audio-brick-dual-dac` | ESP32-S3 + 2× TAS5825M (rev D) | Stereo @0x4C + PBTL mono sub @0x4D |
| `esparagus-louder` / `-bt` / `-s3` | Esparagus + extra gain | |

Board configs live in `config/` (the generated `sdkconfig` stays at the project
root). Sdkconfig defaults are layered via `cmake_extra_args` (left-to-right
override); `build_fw.py` passes `config/sdkconfig.defaults;config/sdkconfig.defaults.esp32s3`.
Custom board config: create `config/sdkconfig.user.<name>` + `user_platformio.ini`
to extend any environment without modifying the main config.

⚠️ Editing anything under `config/sdkconfig.defaults*` has **no effect until you
`rm -f sdkconfig` and rebuild** — defaults only seed a config that does not exist yet.

## Architecture

```
main/
├── main.c                  # Entry — NVS, WiFi, service start, I2S handover hooks
├── settings.c              # NVS persistence for device name, WiFi credentials, volume
├── aht20.c                 # AHT20 temp/humidity on its own I2C bus (GPIO9/15, 0x38)
├── time_sync.c             # SNTP wall clock + timezone (independent of ntp_clock.c)
├── bt_coex.c               # BT coexistence worker — queues BT events so the
│                           #   AirPlay / local-player handover is serialised
├── alac_magic_cookie.c     # Build the ALAC magic cookie (ALACSpecificConfig)
│                           #   from stream format parameters
├── spiram_task.h           # Deliberate pass-through over xTaskCreate(): task stacks
│                           #   must stay in internal RAM (SPI ops disable the cache),
│                           #   so this is the single rewrite point, not a feature
├── buttons.c / led.c       # GPIO input / status LED (Kconfig-gated, unwired here)
├── dacp_client.c           # DACP — remote commands to the AirPlay sender
├── playback_control.c      # Unified play/pause/volume/skip abstraction
├── audio/                  # Audio pipeline
│   ├── audio_receiver.c    # RTSP session manager — orchestrates streams
│   ├── audio_stream.c      # Base stream abstraction
│   ├── audio_stream_buffered.c   # AirPlay 2 AAC (deep jitter buffer)
│   ├── audio_stream_realtime.c   # AirPlay 1/RAOP ALAC (low-latency UDP)
│   ├── audio_decoder.c     # ALAC and AAC decoders
│   ├── audio_buffer.c      # Frame buffering between receiver and output
│   ├── audio_timing.c      # PTP-based timing — early/late frame handling
│   ├── audio_resample.c    # Sample rate conversion (44.1→48kHz)
│   ├── audio_output.c      # I2S output (+ the external-writer guard)
│   ├── audio_output_common.c / _spdif.c / _usb.c  # Shared + alternate backends
│   ├── audio_crypto.c      # AirPlay stream encryption
│   ├── a2dp_sink.c         # Bluetooth A2DP sink (ESP32 only, Kconfig-gated)
│   └── eq_events.c         # EQ parameter changes (TAS58xx)
├── rtsp/                   # RTSP protocol server (rtsp_server/conn/handlers,
│   │                       #   events, crypto, fairplay, rsa, message)
├── hap/                    # HomeKit Accessory Protocol (pair setup/verify, crypto,
│   │                       #   srp, tlv8)
├── plist/                  # Apple property-list parsing (binary + XML)
├── network/                # Network stack
│   ├── wifi.c              # WiFi AP+STA (AP stays open), captive portal, reconnect
│   ├── ethernet.c          # W5500 SPI Ethernet driver
│   ├── mdns_airplay.c      # mDNS _airplay._tcp / _raop._tcp advertisement
│   ├── ptp_clock.c         # Precision Time Protocol clock (multi-room)
│   ├── ntp_clock.c         # RAOP sender clock-offset sync — NOT the wall clock
│   ├── web_server.c        # HTTP config/control server (see API surface below)
│   ├── ota.c               # 收包→校验→写下一个 app 槽（不重启、不鉴权：
│   │                       #   重启与口令校验都在 web_server.c）
│   ├── socket_utils.c      # Shared socket helpers
│   ├── dns_server.c        # Captive-portal DNS
│   └── log_stream.c        # /ws/logs WebSocket log streaming
└── ...
components/
├── dac/                    # Abstract DAC API (Kconfig-selected implementation)
├── dac_tas57xx/            # TI TAS57xx driver with hybrid-flow DSP
├── dac_tas58xx/            # TI TAS58xx driver with on-chip DSP + 15-band EQ
├── dac_es8311/             # ES8311 codec backend (third DAC path)
├── display/                # display.c API + display_st7789.c (LVGL) + display_stub.c,
│                           #   u8g2 path for SSD1306/sh1106 over I2C/SPI
├── cjk_font/               # One font pulled out of u8g2 on purpose:
│   └── ...                 #   u8g2_font_wqy12_t_gb2312 (7539 glyphs) — full 12px CJK
├── boards/                 # Board support (HAL): board_common.c + esp32-generic,
│   │                       #   esp32c5-xiao, esp32s2-generic, esp32s3-generic,
│   │                       #   squeezeamp, esparagus-audio-brick,
│   │                       #   waveshare-esp32p4, waveshare-esp32s3,
│   └── partitions{,-4m,-16m,-16m-ota,-no-ota}.csv
│                           #   本项目用 partitions-16m-ota.csv（双 4MB 槽 + otadata
│                           #   + storage 7.875MB）；partitions-no-ota.csv 是单槽退路
├── sd_card/                # SD over SPI: FATFS mount, dir ops, GBK<->UTF-8
│   ├── filename_enc.c      #   filename conversion (FATFS has no UTF-8 mode)
│   └── gbk_table.c         #   generated CP936 table — do not hand-edit
├── sd_player/              # Local playback from SD: file → ring buffer → decode → I2S
├── web_radio/              # Network radio: HTTP stream / m3u → ring buffer → decode → I2S
├── audio-resampler/        # sinc resampler (44.1→48kHz)
├── spiffs_storage/         # SPIFFS mount (web pages + DSP configs)
└── board_utils/ , u8g2/ , u8g2-hal-esp-idf/
```

## API surface (`main/network/web_server.c`)

All endpoints are same-origin-guarded (below). The web UI in `data/www/` is the
reference client.

| Group | Endpoints |
|---|---|
| Status | `GET /api/system/info` (ip/mac/rssi/channel/ap_open/ap_stations/free_heap/firmware_version/**fw_slot**（当前运行的 OTA 槽，升级后就该从 ota_0 变 ota_1）/uptime_s/reset_reason/playback_source/**airplay_state/airplay_title/airplay_artist/airplay_album/airplay_position_s/airplay_duration_s**/**ota_enabled**/temperature_c/humidity_pct/sensor_age_s/time_synced/time_local/eq_supported/sub_supported/dual_supported) |
| AirPlay control | `GET|POST /api/volume`, `POST /api/device/name` |
| Network | `GET /api/wifi/scan`, `POST /api/wifi/config`, `POST /api/system/restart` |
| **Firmware update** | `POST /api/ota/update`（原始 `.bin` 作为请求体 + `X-OTA-Password` 头；无口令=403，口令错=401，本地在播=409，成功即重启）、`POST /api/ota/password` `{password,current}`（口令存在 NVS；改口令必须带旧口令，第一次不用） |
| Network music | `/api/music`, `/api/music/play`, `/api/music/stop`, `/api/music/next`, `/api/music/prev`, `/api/music` state |
| SD card | `/api/sd/list`, `/play`, `/stop`, `/state`, `/next`, `/prev`, `/upload`, `/mkdir`, `/delete`, `/mount` |
| SPIFFS files | `/api/fs/list`, `/api/fs/upload`, `/api/fs/delete` |
| Audio output | `/api/audio/channel`, `/api/audio/dual`, `/api/audio/biamp`, `/api/audio/sub`, `/api/eq` |
| Diagnostics | `/api/speedtest/ping`, `/download`, `/upload`, `WS /ws/logs`, `/api/led/brightness` |
| Pages | `/` (index), `/eq`, `/logs`, `/speedtest`, `/base.css`, `/app.js`, captive-portal `/connecttest.txt` + `/generate_204` |

## Key Conventions

- **CMake/Kconfig**: board selection is via `CONFIG_` options. The DAC driver is
  auto-selected (TAS57xx / TAS58xx / ES8311). Display, buttons, BT, Ethernet are
  Kconfig-gated.
- **Component structure**: each component has its own `CMakeLists.txt` with
  `idf_component_register()`.
- **Git submodules**: `components/u8g2` and `components/u8g2-hal-esp-idf` are
  submodules — always clone with `--recursive`.
- **SPIFFS**: `data/` is flashed to SPIFFS as `storage.bin` at `0x420000`.
  `data/www/` = web UI, `data/bg/` = ST7789 boot background. The TAS57xx
  hybrid-flow binaries under `/spiffs/hf/` are a runtime convention only — this
  tree ships no `data/hf/` (that DAC is not on this board).
- **SD card**: `components/sd_card` mounts a FAT card over **SPI3** at `/sdcard`
  (CS/SCLK/MISO/MOSI from `CONFIG_SD_*_GPIO`). Paths passed around the codebase are
  card-relative (`/Music/x.flac`), never `/sdcard/...`. FATFS has no UTF-8 mode, so
  `CONFIG_FATFS_CODEPAGE_936` makes `readdir()` return **GBK bytes** which
  `filename_enc.c` converts — the conversion is central in `sd_card_full_path()`, so
  callers must not hand-convert (that double-encodes). Regenerate the table with
  `python scripts/gen_gbk_table.py`.
- **I2S ownership (the invariant that keeps biting)**: AirPlay, Bluetooth,
  `web_radio` and `sd_player` share **one** I2S. An external source takes it by
  calling `audio_output_stop()` and then only ever `audio_output_write()` —
  **never** `audio_output_start()`, which starts AirPlay's playback task and makes
  it a second writer on the same DMA (symptoms: stutter + noise, then silence until
  reboot). Whoever stops must hand it back with `resume_airplay()` (restore sample
  rate, then restart the task). `audio_output_write()` now **rejects** external
  writes while `playback_task_handle` is alive, so violating it is one visible error
  return instead of hours of degraded audio. When touching handover logic, enumerate
  **every** source and **every** protocol version that can reach it — this bug class
  has been fixed for one path and missed for the others three separate times.
- **Audio pipeline**: AudioReceiver (rtsp) → decoder → AudioBuffer → AudioOutput
  (I2S/SPDIF/USB). Buffered streams (AAC) use the deep jitter buffer; realtime
  streams (ALAC) use low-latency UDP with early/late timing thresholds.
- **AirPlay v1 vs v2 is compile-time**, not negotiated: `CONFIG_AIRPLAY_FORCE_V1`
  swaps the mDNS feature bits and whether `_airplay._tcp` is advertised at all.
- **AirPlay/Bluetooth coexistence**: mutually exclusive at runtime (BT connection
  suspends AirPlay, disconnect resumes it). Only on classic-BT-capable ESP32 boards;
  **ESP32-S3 has no classic BT**, so this tree builds with no A2DP.
- **HTTP control plane**: one `gate_dispatch`/`register_uri` choke point in
  `web_server.c` enforces a Host allowlist (IPv4 literal / `*.local` / localhost)
  plus same-origin `Origin`/`Referer` on state-changing requests. That closes DNS
  rebinding and cross-site POSTs **without a token**, so `curl` and local scripts
  keep working. New endpoints must be registered through it — do not add a raw
  `httpd_register_uri_handler()`.
  ⚠️ The gate needs an explicit exemption for the captive-portal probes
  (`gate_is_captive_probe()`: GET on `/hotspot-detect.html`,
  `/library/test/success.html`, `/generate_204`, `/connecttest.txt`, `/redirect`):
  those requests carry `Host: captive.apple.com` etc., so without the exemption the
  setup hotspot answers 403 and the config page never auto-opens. Exempt GET only —
  the handlers return a fixed 302 and touch no state.
- **Eth/WiFi failover**: Ethernet preferred at boot, WiFi fallback if no cable,
  hot-swap at runtime.

## Landmines already paid for

- **Changing the partition table needs two edits, not one.** Editing
  `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` in `config/sdkconfig.defaults*` does
  nothing while `sdkconfig` exists, so the build keeps emitting the **old** layout —
  the tell is `build/flash_args` still showing `storage` at `0x420000` and no
  `ota_data_initial.bin`. Either regenerate `sdkconfig` (loses the hand-tweaked
  LVGL values) or edit those two lines **inside `sdkconfig`** (then restore CRLF,
  or every later diff shows the whole file as changed). Also: after `erase_flash`
  the chip is not in the bootloader, so esptool needs `--before default_reset` —
  with `no_reset` you get a `Write timeout`.

- `CONFIG_AUDIO_OUTPUT_SPDIF` and `CONFIG_AUDIO_OUTPUT_USB` **do not link** in this
  tree: `main.c`'s local-source hooks call `audio_output_stop/write/is_active/
  set_sample_rate`, which those two backends never implement. CI's
  `output-backends` job builds exactly those two configs, so it is red until they are
  implemented. Do not "fix" it by deleting the job.
- `/ws/logs` gets evicted by the HTTP server, not by a UI bug: `max_open_sockets = 3`
  + `lru_purge_enable = true` means any new inbound connection evicts the
  least-recently-active socket, which an idle long-lived WebSocket always is. It also
  has **no origin check**, because in esp_http_server 5.5 the WS handshake completes
  before handler dispatch, so the gate cannot reach it.
- The WiFi scan must not disconnect STA first (`ESP_ERR_WIFI_STATE`); see the
  pitfall log §14.
- `CONFIG_BAT_CHANNEL` / `CONFIG_LED_RGB_GPIO` upstream defaults (7 / 48) collide
  with I2S MCLK and the flash pins; both are `-1` here.
- Display I2C and the AHT20 bus are separate; AHT20 SDA on GPIO45 steals the SD
  card's SCLK (there is now a startup conflict check).
- `reset_reason` reads back as `unknown` on this build — do not chase it as a bug.

## Code Quality

**Requirements**: ESP-IDF **5.5.5** (verified; `dependencies.lock` pins `idf 5.5.5`).
Do **not** use 6.x — `rtsp_rsa.c` relies on `mbedtls_pk_rsa()`, which 6.x removed.

**Formatting**: LLVM-style, 2-space indent, 80-column limit. See `.clang-format`.
There is no host compiler and no `clang-format` on this machine (only
`idf_clang_tidy.exe`), so format by eye and check `awk 'length($0)>80'` yourself —
note it counts *bytes*, so CJK comments read longer than they look.

**Linting**: clang-tidy with bugprone, performance, portability and readability
checks over `main/` **and** `components/`. See `.clang-tidy`.

**Pre-commit hook**: auto-formats staged C/H files and runs clang-tidy (requires
`build/compile_commands.json`). Install via `git config core.hooksPath .githooks`.

**CI** (real files, in `.github/workflows/`):
- `ci.yml` — `changes`, `format-check`, `lint-check`, `output-backends`, `build`,
  and `ci-gate` (the required aggregate check)
- `build.yml` — build matrix
- `release.yml` — `validate-release-version`, `build`, `release`
- `docs.yml` — `docs-build`, `deploy`

**Local tooling**:
```bash
scripts/format.sh          # Format all C/H files (excludes the u8g2 submodule)
scripts/lint.sh            # Run clang-tidy on all C/H files
scripts/lint.sh --fix      # Attempt to auto-fix clang-tidy issues
```

**Verifying web UI changes without a desktop viewport**: the in-app browser is about
531×586, so nothing at `@media (min-width:900px)` can be observed geometrically, and
screenshots may return no surface. Verify by walking `document.styleSheets` →
`cssRules` and asserting each `CSSMediaRule` actually parsed
(`cssRules.length`, `style.order`, `gridTemplateColumns`) — a stray character makes
the browser drop a whole declaration block **silently**, so "file written" proves
nothing. Re-fetch the page after a flash (`/?v=n`) or you read the previous build.

**No unit tests**: this is embedded firmware — no test framework is in place.
Manual testing on hardware is required.

## 发布规范（GitHub / 许可证 / 隐私红线）

**许可证**：`LICENSE` 是上游的 **Non-Commercial License**（Copyright Remi
Bouteiller），明确授予"使用、复制、**修改**、分发"，**但仅限非商业目的**，且要求
版权声明与许可声明出现在所有副本或实质部分中。所以：

- 公开发布本修改版是**允许的**，但**不能替换 LICENSE**，也不能只署自己的名字；
  自己改动的部分可以另加声明，整体仍是 Non-Commercial。
- 任何商业化（卖硬件、接单、赞助/捐赠按钮、广告）都要先拿到授权邮件里那个联系人的
  书面许可。
- README/docs 里指向上游仓库与官方文档的链接**必须保留**，否则会被当成上游官方仓库。

**隐私红线**（提交前逐条 grep，公开仓库里一条都不能留）：

| 项目 | 现在的状态 |
|---|---|
| 热点口令 | ✅ **已挪出已跟踪文件**：`config/sdkconfig.defaults` 里是空占位符，真值在 `config/sdkconfig.user.esp32s3`（被 `config/sdkconfig.user.*` 忽略）。`build_fw.py` 在该文件存在时把它追加到 defaults 链**最后**，所以它盖住基座值 —— 已实测：删掉 `sdkconfig` 重新 reconfigure，口令仍被正确写出。没有这个文件的克隆会编出**开放热点** |
| 作者的工程日志 | 那份中文日志（含 SSID/IP/MAC/个人路径）**不随本仓库发布**，仓库里也不该出现指向它的引用；要沉淀的结论都写进本文件与注释 |
| `firmware_backup/` | ✅ 已进 `.gitignore`（整项目快照 + 镜像 + 真凭据，绝不入库） |
| LAN IP / 内网直链示例 | ✅ 网页 placeholder 已换成 `192.168.1.x` / `example.com` 段 |
| 个人绝对路径 | ✅ 已从 `build_fw.py` 去掉（`PROJECT` 取脚本自身目录，`IDF_TOOLS_PATH` 走环境变量或 `~/.espressif`）；`config/sdkconfig.defaults.esp32s3` 的还原指引改为指向 `firmware_backup/` 的日期目录约定 |
| 🔴 **本机编出的固件镜像** | ⚠️ **源码干净，但镜像里有口令**：本机 `build_fw.py` 会把 `sdkconfig.user.esp32s3` 追加进 defaults 链，所以 `build/airplay2-receiver.bin` 里**实测嵌着真实热点口令**（1 处字面量）。**别把自己在本机编的 `.bin` 传到 GitHub Releases / 发给别人** —— 那等于把口令公开，而且别人刷上去之后热点口令和你家一样。要发预编译镜像只能用 CI 产物（runner 上没有本地覆盖文件 → 空口令） |

> ⚠️ **验证镜像时别用 `strings`** —— 这台 Windows/Git-Bash **没有 `strings`**，
> `strings -a x.bin | grep ...` 会静默返回 0，看起来像"没有泄露"。用 python 直查
> （把下面几个 `<...>` 换成自己配置里的实际值；**真值不要写进这个文件**）：
> ```bash
> python -c "d=open('build/airplay2-receiver.bin','rb').read(); \
> [print(p, d.count(p)) for p in [b'<口令片段>',b'<SSID>',b'<用户名>',b'192.168.5']]"
> ```

⚠️ **重建 `sdkconfig` 会丢东西**：`rm -f sdkconfig` 之后从 defaults 链重建，实测有
**21 行 LVGL 配置和当前不一样**（`LV_MEM_SIZE_KILOBYTES` 64→0、`LV_COLOR_DEPTH_16` 丢失、
断言开关整套变化）—— 因为那些是手工 menuconfig 调出来的，**只存在于 `sdkconfig`，
不存在于任何 defaults 文件**。所以：重建前先 `cp sdkconfig` 存一份，或者先把要保留的值
补进 `config/sdkconfig.user.esp32s3`。

```bash
# 发布前自检（在仓库根目录）。命中任何一条都别提交。
grep -rn -E "192\.168\.[0-9]+\.[0-9]+|C:/Users/|E:(/|\\\\)ESP32|[0-9A-F]{2}(:[0-9A-F]{2}){5}" \
  --exclude-dir=build --exclude-dir=managed_components --exclude-dir=u8g2 \
  --exclude-dir=firmware_backup . | grep -v "192\.168\.4\.1"
grep -rn "CONFIG_DEFAULT_AP_PASSWORD" config/sdkconfig.defaults   # 必须是空串
```

**上游文档与实情的一致性**（2026-09-19 处理过，改功能时记得同步回来）：
`docs/reference/ota.md` 顶部标注"本 fork 的 OTA 默认关闭、需升级口令"，并已放回
`mkdocs.yml` 导航；`docs/features/sendspin.md` 标注"本树 0 处实现"——那是唯一仍然
没有代码支撑的页面；`README.md` 与 `docs/index.md` 的特性清单（OTA / ESP32-S3 蓝牙）
都按实情写，README 顶部有 fork 横幅。

## 注释里的 `§N` 是什么意思

代码和文档注释里大量出现的 `§26`、`§31.8`、"批次 4" 这类编号，指的是作者自己那份
**中文工程日志**（约 3800 行，逐次会话记录现象 → 根因 → 修法 → 实测证据，另有
「避坑记录」一节专门收集花掉几小时的硬件/工具链陷阱，带 ⭐ 的是硬啃出来的 bug，
"已验证 / 未验证" 两栏是可信度边界，"本轮未做，留待观察" 是**已知但故意没修**）。

⚠️ **那份日志不在本仓库里**（里面有家里 SSID、局域网 IP、MAC、个人路径，而且它本质
是私人笔记）。所以克隆这份代码的人看不到它 —— 因此本文件承担"公开可读的那一层沉淀"：

- **架构与接口** → 上面的 Architecture / API surface
- **已经付过学费的坑** → Landmines already paid for + Key Conventions 里的
  I2S 所有权、`sdkconfig` 不覆盖 defaults、配网探测豁免
- **这个 fork 与上游差在哪** → `README.md` 顶部的 fork 说明

读代码时遇到 `§N` 就当成"这里曾经出过一次真实故障，别顺手改回去"；提改动前先把
所在文件的注释读完，很多结论（例如 `audio_receiver_is_playing()` 初值是 `true`
所以不能当播放状态用）已经写在原地了，不需要重新推一遍。
