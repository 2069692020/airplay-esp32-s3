#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "wifi.h"
#include "settings.h"

static const char *TAG = "wifi";

// Event group to signal WiFi connection
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// Re-enable AP after this many consecutive failures
#define AP_REENABLE_THRESHOLD 5
// lwIP DHCP hostnames are limited to 31 characters plus the trailing NUL.
#define DHCP_HOSTNAME_MAX_LEN 31

static int s_retry_num = 0;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static bool s_wifi_initialized = false;
static bool s_sta_connected = false;
static bool s_bssid_set = false;
static esp_timer_handle_t s_retry_timer = NULL;

// Saved AP config from init, used to re-enable AP without duplication
static wifi_config_t s_ap_config;

static void wifi_select_best_ap(const char *ssid);
static void scan_and_connect_task(void *arg);

void wifi_set_hostname(const char *device_name) {
  if (!s_sta_netif || !device_name) {
    return;
  }
  char hostname[DHCP_HOSTNAME_MAX_LEN + 1];
  settings_device_name_to_hostname(device_name, hostname, sizeof(hostname));
  esp_err_t err = esp_netif_set_hostname(s_sta_netif, hostname);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set hostname '%s': %s", hostname,
             esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "Hostname set to: %s", hostname);
  }
}

// 解掉「锁死在某个 BSSID 上」的状态。连接成功后会把那次关联的 BSSID 记下来,
// 之后重连只冲它去试 —— 路由器重启 / mesh 切换 / 换信道之后那个 BSSID 就没了,
// 于是每一条重连都必然失败。以前只有「用户在网页点一次扫描」才会清掉它
// (见 scan_locked)，设备可以这样永久离线。
static void clear_bssid_lock(void) {
  if (!s_bssid_set) {
    return;
  }
  wifi_config_t sta_cfg;
  if (esp_wifi_get_config(WIFI_IF_STA, &sta_cfg) == ESP_OK) {
    memset(sta_cfg.sta.bssid, 0, sizeof(sta_cfg.sta.bssid));
    sta_cfg.sta.bssid_set = false;
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
  }
  s_bssid_set = false;
  ESP_LOGI(TAG, "清除 BSSID 锁定, 下次重连改用扫描结果");
}

static void retry_timer_callback(void *arg) {
  if (!s_sta_connected) {
    // 走到这里说明快速重连已经全部失败、正在退避 —— 此时"锁住的那个 BSSID"
    // 本身就是最可疑的原因 (路由器重启 / mesh 换 AP / 换了信道)。在这里解锁
    // 而不是在 WIFI 事件回调里，是为了把 get/set_config 挪出 sys_evt 那
    // 2–4 KB 的栈 (扫描当初就是因为这个才另起任务)。
    if (s_retry_num >= AP_REENABLE_THRESHOLD) {
      clear_bssid_lock();
    }
    ESP_LOGI(TAG, "Retry timer fired, reconnecting (attempt %d)...",
             s_retry_num + 1);
    esp_wifi_connect();
  }
}

static void schedule_retry(void) {
  // Exponential backoff: 5s, 10s, 20s, 30s (max)
  int delay_s = 5;
  if (s_retry_num > AP_REENABLE_THRESHOLD) {
    int backoff_count = s_retry_num - AP_REENABLE_THRESHOLD;
    delay_s = 5 * (1 << (backoff_count > 3 ? 3 : backoff_count));
    if (delay_s > 30) {
      delay_s = 30;
    }
  }
  ESP_LOGI(TAG, "Scheduling retry in %d seconds", delay_s);
  esp_timer_start_once(s_retry_timer, (uint64_t)delay_s * 1000000);
}

static void enable_ap_mode(void) {
  wifi_mode_t mode;
  if (esp_wifi_get_mode(&mode) == ESP_OK && mode != WIFI_MODE_APSTA) {
    ESP_LOGI(TAG, "Re-enabling AP mode for configuration access");
    if (!s_ap_netif) {
      s_ap_netif = esp_netif_create_default_wifi_ap();
    }
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &s_ap_config);
  }
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    // Defer scan+connect to a separate task — the blocking scan uses too
    // much stack to run inside the sys_evt event loop (2–4 KB).
    // On the very first start there is no saved BSSID to lock onto, so sync
    // s_sta_connected with reality instead of trusting its initialiser.
    s_sta_connected = false;
    xTaskCreate(scan_and_connect_task, "wifi_scan", 4096, NULL, 3, NULL);
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    wifi_event_sta_disconnected_t *disconnected =
        (wifi_event_sta_disconnected_t *)event_data;
    ESP_LOGI(TAG, "Disconnected from AP, reason: %d", disconnected->reason);

    s_retry_num++;
    s_sta_connected = false;

    if (s_retry_num < AP_REENABLE_THRESHOLD) {
      // Fast retries — reconnect immediately
      ESP_LOGI(TAG, "Retrying connection (%d/%d)...", s_retry_num,
               AP_REENABLE_THRESHOLD);
      esp_wifi_connect();
    } else {
      if (s_retry_num == AP_REENABLE_THRESHOLD) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        ESP_LOGW(TAG,
                 "WiFi connection failed after %d attempts, switching to "
                 "backoff retries",
                 AP_REENABLE_THRESHOLD);
        enable_ap_mode();
      }
      // Delayed retries with backoff
      schedule_retry();
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0;
    s_sta_connected = true;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

    // ⚠️ 这里**不再关掉 AP** —— 热点常开。
    //    动机很实际：STA 地址是 DHCP 给的，会变；一旦不在路由器上或地址没记住，
    //    就只能去后台翻设备 IP。热点常开后随时能连上 192.168.4.1 进配置页。
    //    代价：单射频上每个信标周期(~102ms)发一次 beacon + 要维护热点侧的
    //    station 状态，对 AirPlay(几百 kbit/s) 的吞吐影响可以忽略；但 AP 必须
    //    跟 STA 同信道，所以上面 ap.channel 设成了 0（跟随 STA）。
    //    另外：enable_ap_mode() 仍然保留，它现在是幂等的（模式已是 APSTA 就什么都不做）。
    ESP_LOGI(TAG, "STA connected (AP 保持开启, 热点地址 192.168.4.1)");
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
    ESP_LOGI(TAG, "AP started");
  }
}

// One-shot task: scan for best AP then connect — runs outside the event loop
// to avoid overflowing the sys_evt stack.
static void scan_and_connect_task(void *arg) {
  wifi_config_t cfg;
  if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK &&
      strlen((char *)cfg.sta.ssid) > 0) {
    wifi_select_best_ap((char *)cfg.sta.ssid);
  }
  esp_wifi_connect();
  vTaskDelete(NULL);
}

// Scan for the best AP matching our SSID and set its BSSID in the STA config
static void wifi_select_best_ap(const char *ssid) {
  wifi_scan_config_t scan_config = {
      .ssid = (uint8_t *)ssid,
      .bssid = NULL,
      .channel = 0,
      .show_hidden = false,
      .scan_type = WIFI_SCAN_TYPE_ACTIVE,
      .scan_time = {.active = {.min = 0,
                               .max = 0}}, // 0, 0 needed for BT co-exist
  };

  esp_err_t err = esp_wifi_scan_start(&scan_config, true);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Best-AP scan failed: %s", esp_err_to_name(err));
    return;
  }

  uint16_t ap_count = 0;
  esp_wifi_scan_get_ap_num(&ap_count);
  if (ap_count == 0) {
    ESP_LOGW(TAG, "Best-AP scan: no APs found for SSID %s", ssid);
    esp_wifi_scan_get_ap_records(&ap_count, NULL);
    return;
  }

  wifi_ap_record_t *ap_list = malloc(sizeof(wifi_ap_record_t) * ap_count);
  if (!ap_list) {
    esp_wifi_scan_get_ap_records(&ap_count, NULL);
    return;
  }

  esp_wifi_scan_get_ap_records(&ap_count, ap_list);

  // Find AP with strongest signal
  int best_idx = 0;
  for (int i = 1; i < ap_count; i++) {
    if (ap_list[i].rssi > ap_list[best_idx].rssi) {
      best_idx = i;
    }
  }

#if CONFIG_WIFI_PREFER_5GHZ
  // Prefer the 5 GHz band when the same SSID is present on both bands.
  // 5 GHz APs use channels above 14; 2.4 GHz uses channels 1-14. Pick the
  // strongest AP in each band, then favour 5 GHz unless it is too weak and a
  // 2.4 GHz AP is available (in which case 2.4 GHz is the more reliable link).
  int best_5g = -1;
  int best_24 = -1;
  for (int i = 0; i < ap_count; i++) {
    if (ap_list[i].primary > 14) {
      if (best_5g < 0 || ap_list[i].rssi > ap_list[best_5g].rssi) {
        best_5g = i;
      }
    } else {
      if (best_24 < 0 || ap_list[i].rssi > ap_list[best_24].rssi) {
        best_24 = i;
      }
    }
  }
  if (best_5g >= 0 && (best_24 < 0 || ap_list[best_5g].rssi >=
                                          CONFIG_WIFI_PREFER_5GHZ_MIN_RSSI)) {
    best_idx = best_5g;
    ESP_LOGI(TAG, "Preferring 5 GHz AP (rssi=%d, ch=%d)", ap_list[best_5g].rssi,
             ap_list[best_5g].primary);
  } else if (best_5g >= 0) {
    ESP_LOGI(TAG, "5 GHz AP too weak (rssi=%d < %d), falling back to 2.4 GHz",
             ap_list[best_5g].rssi, CONFIG_WIFI_PREFER_5GHZ_MIN_RSSI);
    best_idx = best_24;
  }
#endif

  ESP_LOGI(TAG, "Found %d APs for SSID '%s', best: " MACSTR " (rssi=%d, ch=%d)",
           ap_count, ssid, MAC2STR(ap_list[best_idx].bssid),
           ap_list[best_idx].rssi, ap_list[best_idx].primary);

  for (int i = 0; i < ap_count; i++) {
    if (i != best_idx) {
      ESP_LOGI(TAG, "  Other AP: " MACSTR " (rssi=%d, ch=%d)",
               MAC2STR(ap_list[i].bssid), ap_list[i].rssi, ap_list[i].primary);
    }
  }

  // Set BSSID in the STA config to lock to the best AP
  wifi_config_t sta_cfg;
  esp_wifi_get_config(WIFI_IF_STA, &sta_cfg);
  memcpy(sta_cfg.sta.bssid, ap_list[best_idx].bssid, 6);
  sta_cfg.sta.bssid_set = true;
  esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
  s_bssid_set = true;

  free(ap_list);
}

static void wifi_init_base(void) {
  if (s_wifi_initialized) {
    return;
  }

  s_wifi_event_group = xEventGroupCreate();

  esp_err_t ret = esp_netif_init();
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    ESP_ERROR_CHECK(ret);
  }

  // Create event loop if it doesn't exist
  ret = esp_event_loop_create_default();
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    ESP_ERROR_CHECK(ret);
  }

  esp_event_handler_instance_t instance_any_id;
  esp_event_handler_instance_t instance_got_ip;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

  // Create one-shot retry timer (no background task needed)
  const esp_timer_create_args_t timer_args = {
      .callback = retry_timer_callback,
      .name = "wifi_retry",
  };
  ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_retry_timer));

  s_wifi_initialized = true;
}

void wifi_init_apsta(const char *ap_ssid, const char *ap_password) {
  wifi_init_base();

  if (!s_sta_netif) {
    s_sta_netif = esp_netif_create_default_wifi_sta();
    char dev_name[65];
    settings_get_device_name(dev_name, sizeof(dev_name));
    wifi_set_hostname(dev_name);
  }
  if (!s_ap_netif) {
    s_ap_netif = esp_netif_create_default_wifi_ap();
  }

  // Configure STA
  char ssid[33] = {0};
  char password[65] = {0};
  bool has_credentials = false;

  if (settings_get_wifi_ssid(ssid, sizeof(ssid)) == ESP_OK &&
      settings_get_wifi_password(password, sizeof(password)) == ESP_OK &&
      strlen(ssid) > 0) {
    has_credentials = true;
  }

  wifi_config_t sta_config = {0};
  strlcpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid));
  strlcpy((char *)sta_config.sta.password, password,
          sizeof(sta_config.sta.password));
  sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

  // Configure AP and save for later re-enable
  const char *default_ssid = ap_ssid ? ap_ssid : CONFIG_DEFAULT_AP_SSID;
  const char *default_password =
      ap_password ? ap_password : CONFIG_DEFAULT_AP_PASSWORD;

  memset(&s_ap_config, 0, sizeof(s_ap_config));
  strncpy((char *)s_ap_config.ap.ssid, default_ssid,
          sizeof(s_ap_config.ap.ssid) - 1);
  // ⚠️ 长度要按**截断之后**的缓冲算。用传进来的 strlen 会在 SSID 超长时留下
  //    一个比实际字节数大的 ssid_len（热点名从设备名生成时可能超 32）。
  s_ap_config.ap.ssid_len = strlen((char *)s_ap_config.ap.ssid);
  // channel 0 = 跟着 STA 走。ESP32 只有一个射频，AP+STA 并存时热点必须和路由
  // 器在同一信道上；原先写死 1 既没用 CONFIG_DEFAULT_AP_CHANNEL（那个 menuconfig
  // 项其实一直是装饰性的），又会在路由器不在信道 1 时和 STA 抢射频。
  // 现在 AP 常开（见 GOT_IP 分支），这条从"可有可无"变成必须。
  s_ap_config.ap.channel = 0;
  s_ap_config.ap.max_connection = 4;

  if (strlen(default_password) == 0) {
    s_ap_config.ap.authmode = WIFI_AUTH_OPEN;
  } else {
    strncpy((char *)s_ap_config.ap.password, default_password,
            sizeof(s_ap_config.ap.password) - 1);
    s_ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
  }

  s_retry_num = 0;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &s_ap_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "AP+STA mode started: AP SSID=%s", default_ssid);
  if (has_credentials) {
    ESP_LOGI(TAG, "Connecting to WiFi: %s", ssid);
  }
}

bool wifi_wait_connected(uint32_t timeout_ms) {
  if (!s_wifi_event_group) {
    return false;
  }

  TickType_t timeout_ticks =
      timeout_ms > 0 ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY;
  EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                         WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                         pdFALSE, pdFALSE, timeout_ticks);

  if (bits & WIFI_CONNECTED_BIT) {
    return true;
  }
  if (bits & WIFI_FAIL_BIT) {
    ESP_LOGE(TAG, "Failed to connect to WiFi");
  }
  return false;
}

void wifi_get_mac_str(char *mac_str, size_t len) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(mac_str, len, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
}

bool wifi_is_connected(void) {
  return s_sta_connected;
}

esp_err_t wifi_get_ip_str(char *ip_str, size_t len) {
  if (!s_sta_netif || !ip_str || len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  esp_netif_ip_info_t ip_info;
  esp_err_t err = esp_netif_get_ip_info(s_sta_netif, &ip_info);
  if (err == ESP_OK) {
    snprintf(ip_str, len, IPSTR, IP2STR(&ip_info.ip));
  }
  return err;
}

/* 热点是否开着 + 当前连了几个客户端。AP 常开之后，"到底还开着吗"必须
   看得见，不然只能靠猜或者去翻路由器后台。 */
esp_err_t wifi_ap_get_state(bool *ap_open, uint8_t *sta_count) {
  if (!ap_open && !sta_count) {
    return ESP_ERR_INVALID_ARG;
  }
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK) {
    return ESP_FAIL;
  }
  bool on = (mode == WIFI_MODE_APSTA || mode == WIFI_MODE_AP);
  if (ap_open) {
    *ap_open = on;
  }
  if (sta_count) {
    *sta_count = 0;
    if (on) {
      wifi_sta_list_t list;
      if (esp_wifi_ap_get_sta_list(&list) == ESP_OK) {
        *sta_count = (uint8_t)list.num;
      }
    }
  }
  return ESP_OK;
}


static esp_err_t scan_locked(wifi_ap_record_t **ap_list, uint16_t *ap_count);

// ⚠️ 扫描期间不能让一次重连插进来 (esp_wifi_connect 会打断 scan)，所以先停掉
//    待触发的重试。但**必须记得恢复** —— 以前这里只 stop 不重启，而全文件只有
//    schedule_retry() 会重启它: 设备正处于退避重连时，用户在网页上点一次
//    「扫描 WiFi」，自动重连就被**永久取消**了 (直到下一次网络事件才有救)。
//    esp_timer_stop 返回 ESP_OK 说明确实有一次排在等着，据此决定要不要续上。
//    拆成外层包一层，是因为内层有好几条 return，逐条补太容易漏。
esp_err_t wifi_scan(wifi_ap_record_t **ap_list, uint16_t *ap_count) {
  bool retry_was_pending = (esp_timer_stop(s_retry_timer) == ESP_OK);

  esp_err_t err = scan_locked(ap_list, ap_count);

  if (retry_was_pending && !s_sta_connected) {
    // 沿用现有的 s_retry_num，退避进度不会被打断
    schedule_retry();
  }
  return err;
}

static esp_err_t scan_locked(wifi_ap_record_t **ap_list, uint16_t *ap_count) {
  if (!ap_list || !ap_count) {
    return ESP_ERR_INVALID_ARG;
  }

  // The driver allows scanning while associated, so there is no reason to
  // drop the link first. Dropping it is in fact fatal to whatever asked for
  // the scan: the reply loses its route, lwIP resets the connection after
  // ~2.5 s of retransmits, and the station only gets an IP back ~5.8 s after
  // the disconnect.

  // Clear BSSID lock so next connect can use a fresh scan result
  clear_bssid_lock();

  wifi_scan_config_t scan_config = {
      .ssid = NULL,
      .bssid = NULL,
      .channel = 0,
      .show_hidden = true,
  };

  esp_err_t err = esp_wifi_scan_start(&scan_config, true);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
    return err;
  }

  uint16_t number = 0;
  err = esp_wifi_scan_get_ap_num(&number);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get AP count: %s", esp_err_to_name(err));
    return err;
  }

  if (number == 0) {
    *ap_list = NULL;
    *ap_count = 0;
    esp_wifi_clear_ap_list();
    return ESP_OK;
  }

  wifi_ap_record_t *aps = malloc(sizeof(wifi_ap_record_t) * number);
  if (!aps) {
    return ESP_ERR_NO_MEM;
  }

  err = esp_wifi_scan_get_ap_records(&number, aps);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get AP records: %s", esp_err_to_name(err));
    free(aps);
    return err;
  }

  *ap_list = aps;
  *ap_count = number;
  return ESP_OK;
}

void wifi_stop(void) {
  if (s_wifi_initialized) {
    esp_timer_stop(s_retry_timer);
    esp_wifi_stop();
    esp_wifi_deinit();
    s_wifi_initialized = false;
    s_sta_connected = false;
    s_retry_num = 0;
    if (s_wifi_event_group) {
      xEventGroupClearBits(s_wifi_event_group,
                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }
  }
}
