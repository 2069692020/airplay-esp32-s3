<div align="center">

<img src="docs/assets/logo_airplay_esp32.png" alt="AirPlay ESP32" width="400">

# ESP32 AirPlay 2 Receiver

**Stream music from your Apple devices — or from any phone over Bluetooth — to any speaker for about $10**

[![Fork of rbouteiller/airplay-esp32](https://img.shields.io/badge/fork-of%20rbouteiller%2Fairplay--esp32-blue?style=flat-square)](https://github.com/rbouteiller/airplay-esp32)
[![License](https://img.shields.io/badge/license-Non--Commercial-blue?style=flat-square)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.5.5-red?style=flat-square)](https://docs.espressif.com/projects/esp-idf/)
[![Platform](https://img.shields.io/badge/platform-ESP32%20%7C%20S2%20%7C%20S3%20%7C%20C5-green?style=flat-square)](https://www.espressif.com/en/products/socs)

### [Documentation](https://rbouteiller.github.io/airplay-esp32/) · [Install from your browser](https://rbouteiller.github.io/airplay-esp32/getting-started/flashing/) · [Troubleshooting](https://rbouteiller.github.io/airplay-esp32/troubleshooting/)

</div>

---

## ⚠️ 这是一个个人 fork（非商用）

这个仓库不是上游项目本身，而是我拿
**[`rbouteiller/airplay-esp32`](https://github.com/rbouteiller/airplay-esp32)**
改出来的一个自用分支，目标只有一个：**跑我自己那一台机器** ——
ESP32-S3 N16R8 + PCM5102A DAC + TDA1308 耳放 + 128×64 SSD1306 OLED +
SPI SD 卡读卡座 + AHT20 温湿度。AirPlay 2 / HAP 配对 / RTSP / ALAC-AAC 解码 /
PTP 同步这些核心全部来自上游，署名与许可都归上游作者。

**许可**：上游是 **Non-Commercial License**（见 [LICENSE](LICENSE)），它授予
"use, copy, modify, and distribute ... **for non-commercial purposes only**"，并要求
版权声明与许可声明保留在所有副本里。所以公开发布这个修改版是被允许的，但：

- 许可证**不会**被替换成 MIT/Apache，上游署名会一直在；
- 你把这份代码拿去用，**同样只能非商业**——公开不等于你可以卖；
- 商用需要事先取得上游作者的书面许可。

**下面链接到 `rbouteiller.github.io` 的文档与浏览器刷机工具是上游的**，其中一些能力
在这个 fork 里已经不存在。fork 与上游不一致的地方，以本 README 为准：

| 上游文档说 | 这个 fork 的实情 |
|---|---|
| "Web configuration **and OTA updates**" | ✅ **成立**（2026-09-19 恢复）：双 4MB app 槽 + `otadata`，系统页可上传 `.bin` 升级。但**默认是关的** —— NVS 里没设升级口令时 `/api/ota/update` 一律 403。不想用网页升级就照旧 USB 烧 |
| "Bluetooth A2DP" | ❌ **ESP32-S3 没有经典蓝牙**，本构建不含 A2DP。上游的 `-bt` 环境只适用于 classic-BT 的 ESP32 板 |
| `docs/features/sendspin.md` | 本树**没有任何实现代码**，页首已标注 |

### 本 fork 加了什么

- **SD 卡本地播放**：SPI 上的 FATFS，MP3 / FLAC / WAV / AAC(M4A) / OGG，网页可
  浏览目录、整目录排队、上传、建文件夹、删除；24bit FLAC 的解码杂音已修
- **网络音乐 / 电台**：HTTP 直链与 **m3u / m3u8 歌单**顺序播放，HTTPS 电台走 CA bundle；
  播放信息（歌名/作者/专辑）从 ID3 与流元数据取，中文文件名走 GBK↔UTF-8 转码
- **中文网页界面**：App 式四视图（播放 / 音乐 / 设置 / 系统）+ `#hash` 路由 +
  常驻播放条；桌面宽度下导航固定在左上角侧栏
- **AHT20 温湿度 + SNTP 墙钟**：系统页读数、"上次复位时刻"；热点**常开**，
  随时能用 `ESP32-AirPlay-Setup` 进 `http://192.168.4.1`，不用再翻 DHCP 地址
- **控制面加固**：所有 HTTP 入口走单一 `gate_dispatch`，强制 Host 白名单 +
  同源 `Origin`/`Referer`（挡 DNS rebinding 与跨站 POST，**不需要 token**，
  `curl` 与本地脚本照旧能用）；RTSP/plist 层的若干越界与 use-after-free 修复
- **音频所有权变成代码约束**：`audio_output_write()` 会在 AirPlay 的 playback task
  仍持有 I2S 时**拒绝**外部写入，而不是让两个 writer 抢同一路 DMA

### 本 fork 去掉/改了什么

- `esp32s3` 构建里的蓝牙路径（S3 无经典蓝牙，编不进）
- 分区表换成 `components/boards/partitions-16m-ota.csv`：`ota_0`/`ota_1` 各 4MB
  + `otadata` + storage 7.875MB（中途曾为省 flash 改成无 OTA 单槽，后又恢复）
- 音频输出目前**只有 I2S 这一路能链接通过**；`CONFIG_AUDIO_OUTPUT_SPDIF` / `_USB`
  缺 4 个生命周期函数，开了会链不上（CI 的 `output-backends` job 因此是红的）
- 构建环境钉死 **ESP-IDF 5.5.5**（`dependencies.lock` 也是 5.5.5），**不要用 6.x**：
  `rtsp_rsa.c` 依赖 6.x 已移除的 `mbedtls_pk_rsa()`

---

## What is this?

This turns a cheap ESP32 board into a wireless AirPlay 2 speaker. Plug it into any
amplifier or powered speakers and it shows up on your iPhone, iPad or Mac just like a
HomePod or an AirPlay TV.

Works with **ESP32**, **ESP32-S2**, **ESP32-S3** and **ESP32-C5** chips, including the
[SqueezeAMP](https://github.com/philippe44/SqueezeAMP) (ESP32 + TAS5756) and
[Esparagus Audio Brick](https://sonocotta.com/espragus-audio-brick/) (ESP32 + TAS5825M)
boards, which have amplifiers built in.

ESP32-based boards also support **Bluetooth A2DP**, so anything that can pair with a
Bluetooth speaker can play to them when AirPlay is idle. The Esparagus Audio Brick
additionally supports **wired Ethernet** through an optional W5500 module.

**No cloud. No app. Just tap and play.**

## Quick start

The fastest route is the browser installer — no toolchain, no command line:

**[→ Install from your browser](https://rbouteiller.github.io/airplay-esp32/getting-started/flashing/)**

Building from parts instead? You need an ESP32-S3 dev board, a PCM5102A DAC and a female
pin header, roughly $10 total, and no soldering. See the
[getting started guide](https://rbouteiller.github.io/airplay-esp32/getting-started/).

Building from source:

```bash
git clone --recursive https://github.com/rbouteiller/airplay-esp32
cd airplay-esp32
pio run -e esp32s3 -t upload
pio run -e esp32s3 -t uploadfs   # required — writes the web UI to SPIFFS
```

## Features

- **AirPlay 2** — appears natively in Control Center and every AirPlay-capable app
- **ALAC and AAC decoding** — live streaming (Siri, calls) and music playback
- **Multi-room** — PTP-based timing for synchronised playback
- **Bluetooth A2DP** — receive audio from phones and tablets (**classic-BT ESP32 boards
  only**; ESP32-S3 has no classic Bluetooth, so this fork's `esp32s3` build has none)
- **W5500 Ethernet** — wired networking with automatic WiFi failover
- **Web configuration** — plus **OTA** in this fork: dual app slots and a browser
  uploader that stays **disabled** until you store an upgrade passphrase (USB flashing
  always works too)
- **48 kHz output** — optional 44.1 → 48 kHz conversion via a sinc resampler
- **Displays** — optional OLED or 320×170 colour TFT with track metadata
- **Hardware buttons** — optional play/pause, volume and track skip

Audio only, one speaker per board, and a decent WiFi signal is required.

## Documentation

| | |
| --- | --- |
| [Getting started](https://rbouteiller.github.io/airplay-esp32/getting-started/) | Shopping list, assembly, flashing, first boot |
| [Supported boards](https://rbouteiller.github.io/airplay-esp32/boards/) | SqueezeAMP, Esparagus Audio Brick, XIAO ESP32-C5, custom boards |
| [Features](https://rbouteiller.github.io/airplay-esp32/features/bluetooth/) | Bluetooth, Ethernet, displays, buttons, AirPlay tuning |
| [Reference](https://rbouteiller.github.io/airplay-esp32/reference/build-environments/) | Build environments, SPIFFS, OTA, architecture |
| [Troubleshooting](https://rbouteiller.github.io/airplay-esp32/troubleshooting/) | No sound, no setup WiFi, dropouts, build errors |

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) and the
[contributing guide](https://rbouteiller.github.io/airplay-esp32/contributing/).
Documentation lives in [`docs/`](docs/) and is built with [Zensical](https://zensical.org/)
— every page on the site has an edit link that takes you straight to the GitHub editor.

## Acknowledgements

- [Shairport Sync](https://github.com/mikebrady/shairport-sync) — the reference AirPlay implementation
- [openairplay/airplay2-receiver](https://github.com/openairplay/airplay2-receiver) — Python AirPlay 2 implementation
- [Espressif](https://github.com/espressif) — ESP-IDF framework and codec libraries

## Legal

**Non-commercial use only.** Commercial use requires explicit permission — see [LICENSE](LICENSE).

This is an independent project based on protocol analysis. Not affiliated with Apple Inc.
Not guaranteed to work with future iOS or macOS versions. Provided as-is without warranty.
