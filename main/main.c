#include "audio_output.h"
#include "audio_receiver.h"
#include "aht20.h"
#include "time_sync.h"
#include "buttons.h"
#include "spiram_task.h"
#include "display.h"
#include "dns_server.h"
#include "ethernet.h"
#include "led.h"
#include "hap.h"
#include "mdns_airplay.h"
#include "nvs_flash.h"
#include "playback_control.h"
#include "ptp_clock.h"
#include "rtsp_server.h"
#include "sd_card.h"
#include "sd_player.h"
#include "settings.h"
#include "web_server.h"
#include "web_radio.h"
#include "log_stream.h"
#include "wifi.h"
#include "spiffs_storage.h"
// 只为了 radio_hook_abort_http 里的 esp_http_client_abort()。
// (esp_http_client 已在 main/CMakeLists.txt 的 DEPS 里。)
#include "esp_http_client.h"

#ifdef CONFIG_BT_A2DP_ENABLE
#include "a2dp_sink.h"
#include "bt_coex.h"
#include "rtsp_events.h"
#endif

#ifdef CONFIG_DAC_TAS57XX
#include "dac_tas57xx.h"
#endif

#ifdef CONFIG_DAC_TAS58XX
#include "dac_tas58xx.h"
#endif

#include "iot_board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

// AP mode IP address (192.168.4.1 in network byte order)
#define AP_IP_ADDR 0x0104A8C0

static bool s_airplay_started = false;
static bool s_airplay_infrastructure_ready = false;

static void start_airplay_services(void) {
  if (s_airplay_started) {
    return;
  }

  ESP_LOGI(TAG, "Starting AirPlay services...");

  // One-time infrastructure init (PTP, HAP, audio receiver/output)
  if (!s_airplay_infrastructure_ready) {
    esp_err_t err = ptp_clock_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      ESP_LOGE(TAG, "Failed to init PTP clock: %s", esp_err_to_name(err));
      s_airplay_started = false;
      return;
    }

    ESP_ERROR_CHECK(hap_init());
    ESP_ERROR_CHECK(audio_receiver_init());
    ESP_ERROR_CHECK(audio_output_init());
    mdns_airplay_init();
    s_airplay_infrastructure_ready = true;
  }

  audio_output_start();

  ESP_ERROR_CHECK(rtsp_server_start());

  s_airplay_started = true;
  playback_control_set_source(PLAYBACK_SOURCE_AIRPLAY);
  ESP_LOGI(TAG, "AirPlay ready");
}
#ifdef CONFIG_BT_A2DP_ENABLE
static void stop_airplay_services(void) {
  if (!s_airplay_started) {
    return;
  }

  ESP_LOGI(TAG, "Stopping AirPlay services...");

  rtsp_server_stop();
  audio_output_stop();

  s_airplay_started = false;
  playback_control_set_source(PLAYBACK_SOURCE_NONE);
  ESP_LOGI(TAG, "AirPlay stopped");
}
#endif

static void network_monitor_task(void *pvParameters) {
  (void)pvParameters;
  bool had_network = ethernet_is_connected() || wifi_is_connected();
  bool dns_running = !had_network;
  bool wifi_started = wifi_is_connected() || !ethernet_is_connected();
  bool had_eth = ethernet_is_connected();

  // Start captive portal DNS if no network yet
  if (dns_running) {
    dns_server_start(AP_IP_ADDR);
  }

  // ⚠️ 校时必须在"进任务"时就补一次：下面整个循环是按 has_network 的**跳变**
  //    驱动的，而正常开机时网络在任务起来之前就已经通了 —— 跳变永不发生，
  //    只在跳变分支里调 time_sync_start() 会一次都跑不到。函数本身幂等。
  if (had_network) {
    time_sync_start();
  }

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(2000));

    bool eth_up = ethernet_is_connected();
    bool wifi_up = wifi_is_connected();
    bool has_network = eth_up || wifi_up;

    // Ethernet just came up — stop WiFi entirely
    if (eth_up && !had_eth && wifi_started) {
      ESP_LOGI(TAG, "Ethernet connected — stopping WiFi");
      wifi_stop();
      wifi_started = false;
      wifi_up = false;
    }

    // Ethernet dropped — bring up WiFi (AP + STA)
    if (!eth_up && had_eth) {
      ESP_LOGI(TAG, "Ethernet down — starting WiFi as fallback");
      wifi_init_apsta(NULL, NULL);
      wifi_started = true;
    }

    had_eth = eth_up;
    has_network = eth_up || wifi_is_connected();

    if (has_network == had_network) {
      continue;
    }

    if (has_network) {
      ESP_LOGI(TAG, "Network up (eth=%s, wifi=%s)", eth_up ? "yes" : "no",
               wifi_up ? "yes" : "no");
      start_airplay_services();
      time_sync_start();  // 幂等；断网后重新联网时靠这里补上
      if (dns_running) {
        dns_server_stop();
        dns_running = false;
      }
    } else {
      if (!dns_running) {
        dns_server_start(AP_IP_ADDR);
        dns_running = true;
      }
    }

    had_network = has_network;
  }
}

#ifdef CONFIG_BT_A2DP_ENABLE
static void bt_release_i2s_from_local_sources(void);

static void on_bt_state_changed(bool connected) {
  if (connected) {
    ESP_LOGI(TAG, "BT connected — disabling AirPlay");
    stop_airplay_services();
    // ⚠️ 本地音源 (SD / 电台) 也必须在这里放开 I2S —— 见该函数的注释。
    //    这是 §25 在 AirPlay 路径上修过的同一个 bug 的蓝牙版。
    bt_release_i2s_from_local_sources();
    bt_coex_post(BT_COEX_EVT_BT_CONNECTED);
    playback_control_set_source(PLAYBACK_SOURCE_BLUETOOTH);
  } else {
    ESP_LOGI(TAG, "BT disconnected — re-enabling AirPlay");
    bt_coex_post(BT_COEX_EVT_BT_DISCONNECTED);
    playback_control_set_source(PLAYBACK_SOURCE_NONE);
    if (ethernet_is_connected() || wifi_is_connected()) {
      start_airplay_services();
    }
  }
}

static void on_airplay_client_event(rtsp_event_t event,
                                    const rtsp_event_data_t *data,
                                    void *user_data) {
  (void)data;
  (void)user_data;
  if (bt_a2dp_sink_is_connected()) {
    return;
  }
  switch (event) {
  case RTSP_EVENT_CLIENT_CONNECTED:
    ESP_LOGI(TAG, "AirPlay client connected — disabling BT");
    bt_a2dp_sink_set_discoverable(false);
    bt_coex_post(BT_COEX_EVT_AIRPLAY_CONNECTED);
    break;
  case RTSP_EVENT_PLAYING:
    bt_coex_post(BT_COEX_EVT_AIRPLAY_PLAYING);
    break;
  case RTSP_EVENT_PAUSED:
    // Session still active — BT stays suspended and hidden so the phone
    // reconnects to AirPlay rather than falling back to BT.
    ESP_LOGI(TAG, "AirPlay paused — keeping BT suspended and hidden");
    bt_coex_post(BT_COEX_EVT_AIRPLAY_PAUSED);
    break;
  case RTSP_EVENT_DISCONNECTED:
    ESP_LOGI(TAG, "AirPlay client disconnected — BT resumes after idle delay");
    bt_a2dp_sink_set_discoverable(true);
    bt_coex_post(BT_COEX_EVT_AIRPLAY_DISCONNECTED);
    break;
  default:
    break;
  }
}
#endif

// ============================================================================
// 音源组件 (web_radio / sd_player) <-> 音频输出的适配函数
//
// 音频输出 API 在 main 组件里, 而 main 依赖这两个组件, 直接 include
// 会成循环依赖。所以它们只声明函数指针类型, 由这里注册实际实现。
//
// 两边的钩子结构体成员完全一致 (sd_player 少了 abort_http, 那是 web_radio 为
// HTTP 的阻塞读专门加的), 所以下面这几个适配函数**两个组件共用**。
// ============================================================================

// I2S 现在归谁: true = AirPlay (playback_task 在写), false = 某个本地音源拿着。
//
// 开机时 AirPlay 就占着 (start_airplay_services -> audio_output_start), 所以初值
// 是 true。本地音源在 play 里调 output_stop() 接管时置 false, 交还时置回 true。
//
// 用途是**幂等**: AirPlay 会话开始的信号不止一个 (ANNOUNCE / RECORD /
// SETUP-stream, v1 发前者、v2 发后两者 —— 见 rtsp_handlers.c 里三处
// audio_receiver_notify_yield), 每个都会把两个音源各调一遍 resume_airplay()。
// 没有这个标志的话, 已经交还过还要再 disable/enable 一次正在被写的 I2S。
static volatile bool s_airplay_owns_i2s = true;

static void audio_hook_output_stop(void) {
  audio_output_stop();
  s_airplay_owns_i2s = false; // 本地音源接管
}

#ifdef CONFIG_BT_A2DP_ENABLE
// 蓝牙要接管 I2S 之前, 让两个本地音源放开 DMA。
//
// ⚠️ 这里**不能**用 audio_hook_yield_all() / *_yield_to_airplay(): 那条路最后会
//    调 audio_output_start() 把 AirPlay 的 playback_task 拉起来, 而它读不到
//    AirPlay 数据就灌静音帧 —— 于是蓝牙写者 + AirPlay 写者一起抢同一个 DMA,
//    正好是 §25 修过的那个"顿 + 杂音"。所以走 *_stop(): 只停自己的任务,
//    不碰 AirPlay。
//
// ⚠️ 采样率要单独还原: *_stop() 刻意不做这件事 (那是 yield 分支的活), 而
//    bt_i2s_writer_task 自己也不设率。SD 放过 96k 的歌就把 I2S 留在 96k,
//    蓝牙的 44.1k 流会以 96k 播出 (= §22 那类变调问题的蓝牙版)。
//    必须等两个音源都确认退出之后再改 —— set_sample_rate 会对通道做
//    disable/reconfig/enable, 有活跃写者时不能碰。
static void bt_release_i2s_from_local_sources(void) {
  web_radio_stop();
  sd_player_stop();

  if (audio_output_is_active()) {
    // stop_airplay_services() 刚停过 AirPlay 的任务, 这里还是活的说明有别的东西
    // 把它拉起来了。此时改采样率会打断一个正在写的 I2S, 宁可不动。
    ESP_LOGW(TAG, "蓝牙接管前 I2S 仍有 AirPlay 写者, 跳过采样率还原");
    return;
  }
  audio_output_set_source_rate(CONFIG_OUTPUT_SAMPLE_RATE_HZ);
  audio_output_set_sample_rate(CONFIG_OUTPUT_SAMPLE_RATE_HZ);
  s_airplay_owns_i2s = false; // 现在归蓝牙, AirPlay 的任务没在跑
}
#endif // CONFIG_BT_A2DP_ENABLE

static int audio_hook_output_write(const void *data, size_t bytes,
                                   uint32_t wait_ticks) {
  return (int)audio_output_write(data, bytes, (TickType_t)wait_ticks);
}

static void audio_hook_output_set_rate(uint32_t rate) {
  audio_output_set_sample_rate(rate);
}

static bool audio_hook_output_is_active(void) {
  return audio_output_is_active();
}

static void audio_hook_output_set_source_rate(int rate) {
  audio_output_set_source_rate(rate);
}

static int32_t audio_hook_get_volume_q15(void) {
  return airplay_get_volume_q15();
}

// 中止一个正在跑的 HTTP 请求 (web_radio 的"切曲/停止"用)。
//
// ⚠️ IDF 5.5.5 **没有** esp_http_client_abort(), 所以这里用底层办法:
//    拿到 socket 之后 shutdown()。它会让阻塞在 recv 上的 perform() 立刻
//    返回错误, 而不是干等到读超时。
//
// ⚠️ 用 shutdown 而不是 close: close 会把 fd 还回系统, 而 http 客户端
//    自己随后还会再关一次 —— 中间若有别的任务拿到同一个 fd, 就误关了别人。
//    shutdown 只切断收发, fd 仍归客户端所有, 由它自己正常 cleanup。
static void radio_hook_abort_http(void *client) {
  if (!client) {
    return;
  }
  int fd = esp_http_client_get_socket((esp_http_client_handle_t)client);
  if (fd >= 0) {
    shutdown(fd, SHUT_RDWR);
  }
}

// 把 I2S 交还给 AirPlay: **还原采样率** + **重新拉起 playback_task**。
//
// ⚠️⚠️ 修的是一个"必须复位设备才能恢复"的严重 bug (2026-09-17 实测)。
//    电台/SD 播放时会按流的原生采样率改 I2S (48kHz 的歌就把 I2S 改成 48k),
//    而停播时**两件事都没人做**:
//      1. 采样率没还原 → AirPlay 按 44.1k 重采样, I2S 却在 48k → 变调
//      2. playback_task 没重新拉起 → 没人往 I2S 写 → **完全没声音**
//    结果: 播完本地/网络歌曲后 iPhone 投 AirPlay, 歌在播放但没声,
//    只有重启设备 (重新走 start_airplay_services -> audio_output_start) 才好。
//
//    顺序很重要: 先恢复采样率再拉起任务 —— 反过来的话 playback_task 会先
//    用错的采样率写一段, 听感上是一声怪响。
//
// ⚠️⚠️ 再补一道守卫: **别的本地音源正在播时直接返回** (2026-09-18 修)。
//
//    起因: SD 卡放歌的过程中用 iPhone 推 AirPlay —— 推不过去, 而且正在播的
//    SD 歌变成"顿 + 杂音"。
//
//    根因: yield 回调 (audio_hook_yield_all) 里两个音源**各调一次**本函数,
//    而"当前空闲"的那个也要走早退路径 (那是 §21 必须保留的行为, 见上面那段)。
//    若此刻**另一个**音源正占着 I2S, 这次 resume 会做两件破坏性的事:
//      1. audio_output_set_sample_rate() 对 I2S 做 disable/reconfig/enable,
//         而那个音源的 play task 正在同一个通道上写 PCM —— 该函数自己的注释
//         就写着 "Only safe to call when no writer task is actively using I2S"。
//      2. audio_output_start() 把 AirPlay 的 playback_task 拉起来, 它从
//         audio_receiver_read() 读不到数据就灌静音帧 —— 于是两个任务交替往
//         同一个 DMA 写, SD 的音乐和静音帧交错 = 顿 + 杂音。
//      而且这两步是**同步**跑在 RTSP 连接任务里、在 ANNOUNCE 的 200 OK 之前
//      (见 rtsp_handlers.c 的 handle_announce), 拖久了手机会放弃这次推流。
//
//    守卫只看"**别的**音源是否在播", 不看自己 —— §21 那个 bug 恰恰是
//    "自己已经播完 (is_active() 为 false) 但 I2S 还在自己手里", 那种情况
//    必须照常交还, 所以不能在这里加自己的 active 判断。
//    占着 I2S 的那个音源停播时 (状态已置 IDLE 之后) 会自己调到这里, 那时
//    两个音源都不 active, 交还照常执行。
static void audio_hook_resume_airplay(void) {
  if (sd_player_is_active() || web_radio_is_active()) {
    ESP_LOGW(TAG, "本地音源仍在播放, 暂不交还 I2S (等它自己停)");
    return;
  }
  // ⚠️ 幂等: 一次会话里 notify_yield 会被触发多次 (v2 的 RECORD 和 SETUP-stream
  //    各一次), 而只有第一次需要真正动手 —— 已经交还过再调一遍就等于对着一个
  //    正在被 playback_task 写的 I2S 再来一次 disable/enable。
  if (s_airplay_owns_i2s) {
    return;
  }
  s_airplay_owns_i2s = true;
  audio_output_set_source_rate(CONFIG_OUTPUT_SAMPLE_RATE_HZ);
  audio_output_set_sample_rate(CONFIG_OUTPUT_SAMPLE_RATE_HZ);
  audio_output_start();
}

// AirPlay 会话开始时要让**所有**本地音源让出 I2S。
//
// ⚠️ audio_receiver 的 yield 回调**只有一个槽位**, 所以两者必须在这里合并。
//    两个让出函数都是幂等的, 且都**不能**加 is_active() 守卫 —— 见
//    web_radio.c:2068 的说明 (播完的瞬间状态已经是 IDLE, 但 I2S 还占着)。
static void audio_hook_yield_all(void) {
  web_radio_yield_to_airplay();
  sd_player_yield_to_airplay();
}

void app_main(void) {
  ESP_LOGW(TAG, "Boot: reset reason %d", (int)esp_reset_reason());

  // Initialize NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
  ESP_ERROR_CHECK(settings_init());
#ifdef CONFIG_DAC_TAS57XX
  // Load persisted sub level offset (pre-init safe; applied on first volume).
  float sub_off;
  if (settings_get_sub_offset(&sub_off) == ESP_OK) {
    dac_tas57xx_set_sub_offset_db(sub_off);
  }
#elif defined(CONFIG_DAC_TAS58XX)
  // Load persisted sub level offset (pre-init safe; applied on first volume).
  float sub_off;
  if (settings_get_sub_offset(&sub_off) == ESP_OK) {
    dac_tas58xx_set_sub_offset_db(sub_off);
  }
  float sub_xo;
  if (settings_get_sub_crossover(&sub_xo) == ESP_OK) {
    dac_tas58xx_set_sub_crossover_hz(sub_xo);
  }
  static float sub_eq[2][SETTINGS_WAY_BANDS];
  if (settings_get_sub_eq(sub_eq) == ESP_OK) {
    dac_tas58xx_sub_eq_set_gains(TAS58XX_WAY_LOW, sub_eq[0]);
    dac_tas58xx_sub_eq_set_gains(TAS58XX_WAY_HIGH, sub_eq[1]);
  }
  // Second-amplifier role must be known before the DAC is initialised.
  uint8_t dual_mode;
  if (settings_get_dual_mode(&dual_mode) == ESP_OK) {
    if (!TAS58XX_BIAMP_SUPPORTED && dual_mode == TAS58XX_DUAL_BIAMP) {
      dual_mode = TAS58XX_DUAL_SUB;
    }
    dac_tas58xx_set_dual_mode((tas58xx_dual_mode_t)dual_mode);
  }
  float biamp_xo;
  if (settings_get_biamp_crossover(&biamp_xo) == ESP_OK) {
    dac_tas58xx_set_biamp_crossover_hz(biamp_xo);
  }
  bool biamp_swap;
  if (settings_get_biamp_swap(&biamp_swap) == ESP_OK) {
    dac_tas58xx_set_biamp_swap(biamp_swap);
  }
  static float biamp_eq[2][2][SETTINGS_WAY_BANDS];
  if (settings_get_biamp_eq(biamp_eq) == ESP_OK) {
    for (int spk = 0; spk < 2; spk++) {
      dac_tas58xx_biamp_set_gains(spk, TAS58XX_WAY_LOW, biamp_eq[spk][0]);
      dac_tas58xx_biamp_set_gains(spk, TAS58XX_WAY_HIGH, biamp_eq[spk][1]);
    }
  }
#endif
  spiffs_storage_init();

  // SD 卡 (SPI 模式 + FATFS)。**失败不致命** —— 没插卡是常态, 只是网页上
  // SD 面板显示"未挂载"。用户插好卡后在网页上点「重新挂载」即可, 不用重启
  // (sd_card 内部带 10 秒节流地重试挂载)。
  esp_err_t sd_err = sd_card_init();
  if (sd_err != ESP_OK) {
    ESP_LOGW(TAG, "SD card not available (%s) — SD playback disabled",
             esp_err_to_name(sd_err));
  }

  log_stream_init();
  ESP_ERROR_CHECK(playback_control_init());

  // SD 卡本地播放器。同样只初始化缓冲和状态, 不会主动播放。
  // 失败不致命 — 只是网页上的 SD 播放功能不可用, AirPlay 照常工作。
  if (sd_player_init() != ESP_OK) {
    ESP_LOGW(TAG, "SD player init failed — SD playback disabled");
  } else {
    static const sd_player_output_hooks_t sd_hooks = {
        .output_stop = audio_hook_output_stop,
        .output_write = audio_hook_output_write,
        .output_set_rate = audio_hook_output_set_rate,
        .output_set_source_rate = audio_hook_output_set_source_rate,
        .output_is_active = audio_hook_output_is_active,
        .get_volume_q15 = audio_hook_get_volume_q15,
        .resume_airplay = audio_hook_resume_airplay,
    };
    sd_player_set_output_hooks(&sd_hooks);
  }

  // Web 电台 (HTTP 流播放)。只初始化缓冲和状态, 不会主动播放。
  // 失败不致命 — 只是网页上的电台功能不可用, AirPlay 照常工作。
  if (web_radio_init() != ESP_OK) {
    ESP_LOGW(TAG, "Web radio init failed — radio feature disabled");
  } else {
    // 把音频输出函数注册给 web_radio。
    // 音频输出代码在 main 组件里, 而 main 依赖 web_radio, 直接 include
    // 会成循环依赖 —— 所以用函数指针注入 (见 web_radio.h 说明)。
    // 用适配函数而不是强转函数指针, 避免签名不匹配的未定义行为。
    static const web_radio_output_hooks_t radio_hooks = {
        .output_stop = audio_hook_output_stop,
        .output_write = audio_hook_output_write,
        .output_set_rate = audio_hook_output_set_rate,
        .output_set_source_rate = audio_hook_output_set_source_rate,
        .output_is_active = audio_hook_output_is_active,
        .get_volume_q15 = audio_hook_get_volume_q15,
        .abort_http = radio_hook_abort_http,
        .resume_airplay = audio_hook_resume_airplay,
    };
    web_radio_set_output_hooks(&radio_hooks);
  }

  // 告诉音频接收层: AirPlay 会话开始时, 让所有本地音源让出 I2S。
  // (回调只有一个槽位, 两个组件在里面合并 —— 见 audio_hook_yield_all。)
  audio_receiver_set_yield_callback(audio_hook_yield_all);

  led_init();

  // Initialize board-specific hardware (includes I2C/SPI bus for display and
  // DAC)
  ESP_LOGI(TAG, "Board: %s", iot_board_get_info());
  esp_err_t err = iot_board_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Board init failed: %s", esp_err_to_name(err));
  }

  // Pass the board-owned bus to the display so it reuses it rather than
  // creating a duplicate bus on the same pins.
#if defined(CONFIG_DISPLAY_BUS_SPI)
  display_init(iot_board_get_handle(BOARD_SPI_DISP_ID));
#else
  display_init(iot_board_get_handle(BOARD_I2C_DISP_ID));
#endif

  // Initialize LVGL-dependent board resources (e.g., touch input) after
  // display/LVGL port is ready.
  iot_board_init_lvgl_resources();

  // AHT20 温湿度。放在 iot_board_init() 之后是刻意的：显示那条 I2C 总线也用
  // i2c_port = -1 申请控制器，谁先调用谁拿 0 号。顺序定下来，日志里的端口
  // 归属才可复现。失败不致命 —— 没接传感器时只是网页上那一格空着。
  if (aht20_init() != ESP_OK) {
    ESP_LOGW(TAG, "AHT20 不可用 — 温湿度读数将为空");
  }

  // Try ethernet first
  bool eth_available = false;
  err = ethernet_init();
  if (err == ESP_OK) {
    // Wait for ethernet link + DHCP (up to 5s for link, then 10s more for DHCP)
    ESP_LOGI(TAG, "Waiting for ethernet...");
    for (int i = 0; i < 25 && !ethernet_is_link_up(); i++) {
      vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (ethernet_is_link_up() && !ethernet_is_connected()) {
      ESP_LOGI(TAG, "Ethernet link up, waiting for DHCP...");
      for (int i = 0; i < 50 && !ethernet_is_connected(); i++) {
        vTaskDelay(pdMS_TO_TICKS(200));
      }
    }
    eth_available = ethernet_is_connected();
    if (eth_available) {
      ESP_LOGI(TAG, "Ethernet connected");
    } else {
      ESP_LOGI(TAG, "Ethernet not connected (cable?), will use WiFi");
    }
  } else if (err != ESP_ERR_NOT_SUPPORTED) {
    ESP_LOGW(TAG, "Ethernet init failed: %s", esp_err_to_name(err));
  }

  // Start WiFi only if ethernet is not available
  if (!eth_available) {
    wifi_init_apsta(NULL, NULL);

    // Wait for initial WiFi connection if credentials exist
    if (settings_has_wifi_credentials()) {
      if (!wifi_wait_connected(30000)) {
        ESP_LOGI(TAG, "Connect to 'ESP32-AirPlay-Setup' -> http://192.168.4.1");
      }
    } else {
      ESP_LOGI(TAG, "Connect to 'ESP32-AirPlay-Setup' -> http://192.168.4.1");
    }
  } else {
    ESP_LOGI(TAG, "Ethernet connected — skipping WiFi");
  }

  // Start services that work on any interface
  web_server_start(80);
  task_create_spiram(network_monitor_task, "net_mon", 4096, NULL, 5, NULL,
                     NULL);

  bool connected = eth_available || wifi_is_connected();
  if (connected) {
    start_airplay_services();
  }

#ifdef CONFIG_BT_A2DP_ENABLE
  // Initialize Bluetooth A2DP Sink
  {
    char bt_name[65];
    settings_get_device_name(bt_name, sizeof(bt_name));
    esp_err_t bt_err = bt_a2dp_sink_init(bt_name, on_bt_state_changed);
    if (bt_err != ESP_OK) {
      ESP_LOGE(TAG, "BT A2DP init failed: %s", esp_err_to_name(bt_err));
    } else {
      if (bt_coex_start() != ESP_OK) {
        ESP_LOGE(TAG, "BT coexistence task start failed");
      }
      rtsp_events_register(on_airplay_client_event, NULL);
    }
  }
#endif

  // Boot baseline: free internal DRAM once WiFi (and BT, where enabled) are
  // resident but before any stream is active.  Compare against the
  // "Buffered start" log to see the headroom available for WiFi/TCP buffers.
  ESP_LOGI(TAG,
           "Boot baseline: free heap %lu internal (largest block %lu), "
           "%lu SPIRAM",
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  buttons_init();

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}
