#include "web_server.h"

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_wifi.h"

#include "settings.h"
#include "led.h"
#include "playback_control.h"
#include "display.h"
#include "aht20.h"
#include "time_sync.h"
#include "wifi.h"
#include "ethernet.h"
#include "log_stream.h"
#include "rtsp_server.h"
#include "ota.h"
#include "web_radio.h"
#include "sd_card.h"
#include "sd_player.h"
#include "audio_output.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef CONFIG_DAC_TAS58XX
#include "eq_events.h"
#include "dac_tas58xx.h"
#include "dac_tas58xx_eq.h"
#endif

#ifdef CONFIG_DAC_TAS57XX
#include "dac_tas57xx.h"
#endif

/* Sub level-trim (2.1 subwoofer) is exposed by both the TAS57xx and TAS58xx
 * drivers with the same API shape. Map to whichever is configured so the
 * /api/audio/sub endpoints work regardless of DAC. */
#if defined(CONFIG_DAC_TAS57XX)
#define DAC_HAS_SUB_OFFSET       1
#define DAC_SUB_OFFSET_MIN_DB    TAS57XX_SUB_OFFSET_MIN_DB
#define DAC_SUB_OFFSET_MAX_DB    TAS57XX_SUB_OFFSET_MAX_DB
#define dac_get_sub_offset_db()  dac_tas57xx_get_sub_offset_db()
#define dac_set_sub_offset_db(x) dac_tas57xx_set_sub_offset_db(x)
/* The trim only moves devices flagged is_sub, which is index > 0, so a
 * single-amplifier board has nothing for it to act on. */
#define dac_has_sub() (dac_tas57xx_get_device_count() > 1)
#elif defined(CONFIG_DAC_TAS58XX)
#define DAC_HAS_SUB_OFFSET       1
#define DAC_SUB_OFFSET_MIN_DB    TAS58XX_SUB_OFFSET_MIN_DB
#define DAC_SUB_OFFSET_MAX_DB    TAS58XX_SUB_OFFSET_MAX_DB
#define dac_get_sub_offset_db()  dac_tas58xx_get_sub_offset_db()
#define dac_set_sub_offset_db(x) dac_tas58xx_set_sub_offset_db(x)
/* Only dual-DAC boards have a sub, and only while the second amplifier is
 * configured as a bridged mono subwoofer rather than a bi-amp channel. A role
 * chosen but not yet restarted into does not count. */
#define dac_has_sub()                    \
  (dac_tas58xx_get_device_count() > 1 && \
   dac_tas58xx_get_active_dual_mode() == TAS58XX_DUAL_SUB)
#endif

static const char *TAG = "web_server";
static httpd_handle_t s_server = NULL;

#define SPIFFS_CHUNK_SIZE 1024

static esp_err_t serve_spiffs_file(httpd_req_t *req, const char *path,
                                   const char *content_type) {
  FILE *f = fopen(path, "r");
  if (!f) {
    ESP_LOGE(TAG, "Failed to open %s", path);
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, content_type);
  char buf[SPIFFS_CHUNK_SIZE];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) {
      fclose(f);
      httpd_resp_send_chunk(req, NULL, 0);
      return ESP_FAIL;
    }
  }
  fclose(f);
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

// API handlers
static esp_err_t root_handler(httpd_req_t *req) {
  return serve_spiffs_file(req, "/spiffs/www/index.html", "text/html");
}

static esp_err_t favicon_handler(httpd_req_t *req) {
  httpd_resp_set_status(req, "204 No Content");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

// 四个页面共用一份设计系统样式 (data/www/base.css)。
// ⚠️ no-store 是故意的：SPIFFS 里的东西是随固件一起重刷的，而 httpd 这边没有
//    ETag 可以重新验证 —— 缓存住就会在刷完固件后继续用旧样式，那种"我明明改了
//  却没生效"最费人。局域网内 8 KB 的重传成本可以忽略。
static esp_err_t base_css_handler(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return serve_spiffs_file(req, "/spiffs/www/base.css", "text/css");
}

// 主页的应用逻辑 (data/www/app.js)。no-store 的理由同上：主页现在是
// 「外壳 + 四视图」的单页应用，逻辑全在这个文件里，缓存住旧脚本 = 界面改版
// 不生效，而且症状比样式表更隐蔽（页面看着完全正常，只是按钮不动）。
static esp_err_t app_js_handler(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return serve_spiffs_file(req, "/spiffs/www/app.js", "text/javascript");
}

static esp_err_t logs_page_handler(httpd_req_t *req) {
  return serve_spiffs_file(req, "/spiffs/www/logs.html", "text/html");
}

static esp_err_t speedtest_page_handler(httpd_req_t *req) {
  return serve_spiffs_file(req, "/spiffs/www/speedtest.html", "text/html");
}

// Tiny endpoint used by JS for RTT timing. Returns minimal body.
static esp_err_t speedtest_ping_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req, "ok", 2);
  return ESP_OK;
}

// Streams `bytes` octets of filler data so the browser can measure DL speed.
// Capped to avoid pathological requests starving audio.
#define SPEEDTEST_MAX_BYTES ((size_t)16 * 1024 * 1024)
#define SPEEDTEST_CHUNK     2048

// Same ceiling for the upload direction.  Without it a client can declare a
// huge Content-Length and then send nothing, holding one of the 2-3 httpd
// sockets forever and locking the whole web UI out.
#define SPEEDTEST_MAX_UPLOAD_BYTES ((size_t)16 * 1024 * 1024)
// Consecutive recv timeouts tolerated before abandoning an upload.  Each one is
// already seconds of client silence, so a few in a row means the peer is gone.
#define SPEEDTEST_MAX_TIMEOUTS 3

static esp_err_t speedtest_download_handler(httpd_req_t *req) {
  size_t bytes = (size_t)1024 * 1024;
  char qbuf[64];
  if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
    char val[16];
    if (httpd_query_key_value(qbuf, "bytes", val, sizeof(val)) == ESP_OK) {
      long v = strtol(val, NULL, 10);
      if (v > 0) {
        bytes = (size_t)v;
      }
    }
  }
  if (bytes > SPEEDTEST_MAX_BYTES) {
    bytes = SPEEDTEST_MAX_BYTES;
  }

  // Reuse a single buffer of filler bytes. Static so we don't repeatedly
  // hammer the heap; content is irrelevant but non-zero to thwart any
  // compression along the way.
  static uint8_t filler[SPEEDTEST_CHUNK];
  static bool filler_init = false;
  if (!filler_init) {
    for (size_t i = 0; i < sizeof(filler); i++) {
      filler[i] = (uint8_t)(i * 37);
    }
    filler_init = true;
  }

  httpd_resp_set_type(req, "application/octet-stream");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  size_t remaining = bytes;
  while (remaining > 0) {
    ssize_t n =
        remaining < SPEEDTEST_CHUNK ? (ssize_t)remaining : SPEEDTEST_CHUNK;
    if (httpd_resp_send_chunk(req, (const char *)filler, n) != ESP_OK) {
      return ESP_FAIL;
    }
    remaining -= (size_t)n;
  }
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

// Consumes a POST body and reports how many bytes were received.
static esp_err_t speedtest_upload_handler(httpd_req_t *req) {
  if (req->content_len > SPEEDTEST_MAX_UPLOAD_BYTES) {
    httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Upload too large");
    return ESP_FAIL;
  }
  size_t total = req->content_len;
  size_t got = 0;
  int timeouts = 0;
  uint8_t buf[SPEEDTEST_CHUNK];
  while (got < total) {
    size_t want = total - got;
    if (want > sizeof(buf)) {
      want = sizeof(buf);
    }
    int r = httpd_req_recv(req, (char *)buf, want);
    if (r <= 0) {
      if (r == HTTPD_SOCK_ERR_TIMEOUT) {
        if (++timeouts > SPEEDTEST_MAX_TIMEOUTS) {
          return ESP_FAIL;
        }
        continue;
      }
      return ESP_FAIL;
    }
    timeouts = 0;
    got += (size_t)r;
  }
  char reply[64];
  int n = snprintf(reply, sizeof(reply), "received=%u", (unsigned)got);
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send(req, reply, n);
  return ESP_OK;
}

// Captive portal detection handlers
// These endpoints are requested by various OS to detect captive portals
static esp_err_t captive_portal_redirect(httpd_req_t *req) {
  // Redirect to the configuration page
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

// Apple devices (iOS/macOS) check these
static esp_err_t captive_apple_handler(httpd_req_t *req) {
  // Apple expects specific response, redirect instead
  return captive_portal_redirect(req);
}

// Android checks this
static esp_err_t captive_android_handler(httpd_req_t *req) {
  // Android expects 204 for no captive portal, anything else triggers portal
  return captive_portal_redirect(req);
}

// Windows checks this
static esp_err_t captive_windows_handler(httpd_req_t *req) {
  return captive_portal_redirect(req);
}

#define WIFI_SCAN_MAX_RESULTS 32

// The scan can return the same AP more than once (a mesh system re-advertising
// under the same SSID, or the driver reporting the associated AP again). Keep
// the strongest sighting of each BSSID rather than showing duplicates.
static void wifi_scan_dedupe(wifi_ap_record_t *aps, uint16_t *count) {
  uint16_t out = 0;
  for (uint16_t i = 0; i < *count; i++) {
    int found = -1;
    for (uint16_t j = 0; j < out; j++) {
      if (memcmp(aps[j].bssid, aps[i].bssid, sizeof(aps[i].bssid)) == 0) {
        found = j;
        break;
      }
    }
    if (found >= 0) {
      if (aps[i].rssi > aps[found].rssi) {
        aps[found] = aps[i];
      }
      continue;
    }
    if (out != i) {
      aps[out] = aps[i];
    }
    out++;
  }
  *count = out;
}

// Hidden networks have no SSID to show, and the picker fills the SSID field
// from the entry the user taps — so an empty one would blank the field. Report
// them by BSSID instead of dropping them silently.
static void wifi_scan_label_hidden(wifi_ap_record_t *aps, uint16_t count) {
  for (uint16_t i = 0; i < count; i++) {
    if (aps[i].ssid[0] != '\0') {
      continue;
    }
    snprintf((char *)aps[i].ssid, sizeof(aps[i].ssid), "隐藏网络 (%02X:%02X)",
             aps[i].bssid[4], aps[i].bssid[5]);
  }
}

// A scan can only run with the station idle, so wifi_scan() drops the link for
// a few seconds. Answering first keeps that off the socket: a live connection
// turns into a black hole the moment the station leaves, and lwIP gives up on
// the retransmits after ~2.5 s — well before the reconnect completes — which
// reaches the browser as a connection reset instead of the JSON it asked for.
static void wifi_scan_task(void *arg) {
  httpd_req_t *req = (httpd_req_t *)arg;
  wifi_ap_record_t *ap_list = NULL;
  uint16_t ap_count = 0;

  esp_err_t err = wifi_scan(&ap_list, &ap_count);

  cJSON *json = cJSON_CreateObject();
  if (!json) {
    free(ap_list);
    goto out;
  }

  if (err == ESP_OK) {
    wifi_scan_dedupe(ap_list, &ap_count);
    wifi_scan_label_hidden(ap_list, ap_count);

    cJSON_AddBoolToObject(json, "success", true);
    cJSON *networks = cJSON_CreateArray();
    if (networks) {
      for (uint16_t i = 0; i < ap_count && i < WIFI_SCAN_MAX_RESULTS; i++) {
        cJSON *net = cJSON_CreateObject();
        if (!net) {
          break;
        }
        cJSON_AddStringToObject(net, "ssid", (char *)ap_list[i].ssid);
        cJSON_AddNumberToObject(net, "rssi", ap_list[i].rssi);
        cJSON_AddNumberToObject(net, "channel", ap_list[i].primary);
        cJSON_AddItemToArray(networks, net);
      }
      cJSON_AddItemToObject(json, "networks", networks);
    }
  } else {
    cJSON_AddBoolToObject(json, "success", false);
    cJSON_AddStringToObject(json, "error", esp_err_to_name(err));
  }
  free(ap_list);

  char *json_str = cJSON_PrintUnformatted(json);
  cJSON_Delete(json);
  if (json_str) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    free(json_str);
  }

out:
  httpd_req_async_handler_complete(req);
  vTaskDelete(NULL);
}

static esp_err_t wifi_scan_handler(httpd_req_t *req) {
  httpd_req_t *async_req = NULL;
  // Preserve the socket — the response is delivered from the scan task.
  esp_err_t err = httpd_req_async_handler_begin(req, &async_req);
  if (err != ESP_OK) {
    return err;
  }

  // 4096 covers the blocking scan; the AP list stays in the driver, not here.
  if (xTaskCreate(wifi_scan_task, "wifi_scan_json", 4096, async_req, 5,
                  NULL) != pdPASS) {
    httpd_req_async_handler_complete(async_req);
    return ESP_FAIL;
  }
  return ESP_OK;
}

static esp_err_t wifi_config_handler(httpd_req_t *req) {
  char content[512];
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  content[ret] = '\0';

  cJSON *json = cJSON_Parse(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *ssid_json = cJSON_GetObjectItem(json, "ssid");
  cJSON *password_json = cJSON_GetObjectItem(json, "password");

  cJSON *response = cJSON_CreateObject();
  if (ssid_json && cJSON_IsString(ssid_json)) {
    const char *ssid = cJSON_GetStringValue(ssid_json);
    const char *password = password_json && cJSON_IsString(password_json)
                               ? cJSON_GetStringValue(password_json)
                               : "";

    esp_err_t err = settings_set_wifi_credentials(ssid, password);
    if (err == ESP_OK) {
      cJSON_AddBoolToObject(response, "success", true);
      ESP_LOGI(TAG, "WiFi credentials saved. We are restarting...");
      // Schedule restart
      vTaskDelay(pdMS_TO_TICKS(1000));
      esp_restart();
    } else {
      cJSON_AddBoolToObject(response, "success", false);
      cJSON_AddStringToObject(response, "error", esp_err_to_name(err));
    }
  } else {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "Invalid SSID");
  }

  char *json_str = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);

  return ESP_OK;
}

static esp_err_t device_name_handler(httpd_req_t *req) {
  char content[256];
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  content[ret] = '\0';

  cJSON *json = cJSON_Parse(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *name_json = cJSON_GetObjectItem(json, "name");
  cJSON *response = cJSON_CreateObject();

  if (name_json && cJSON_IsString(name_json)) {
    const char *name = cJSON_GetStringValue(name_json);
    esp_err_t err = settings_set_device_name(name);
    if (err == ESP_OK) {
      wifi_set_hostname(name);
      ethernet_set_hostname(name);
      cJSON_AddBoolToObject(response, "success", true);
    } else {
      cJSON_AddBoolToObject(response, "success", false);
      cJSON_AddStringToObject(response, "error", esp_err_to_name(err));
    }
  } else {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "Invalid name");
  }

  char *json_str = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);

  return ESP_OK;
}

// ---------------------------------------------------------------------------
// 音量 (网页滑块)
// ---------------------------------------------------------------------------
//
// 和 AirPlay 共用同一个音量: airplay_set_volume_q15 会同时更新 Q15 缓存、
// DAC 和 NVS。所以网页上调完, 手机上连着 AirPlay 时也是这个音量。
//
// 对外用 0..100 的百分比而不是 dB:
//   · 滑块天生就是 0..100
//   · dB 是负值 (-30..0), 直接暴露给用户很反直觉
// 曲线在 airplay_volume_db_to_q15 里 (平方曲线), 两端都能到 (0=静音, 100=0dB)。

static esp_err_t volume_get_handler(httpd_req_t *req) {
  int32_t q15 = airplay_get_volume_q15();
  // 按平方曲线反解成 0..100, 保证"网页读回来的值"和"拖到那个位置"一致。
  float db = airplay_q15_to_volume_db(q15);
  int percent = (int)((db + 30.0f) / 30.0f * 100.0f + 0.5f);
  if (percent < 0) {
    percent = 0;
  }
  if (percent > 100) {
    percent = 100;
  }

  cJSON *json = cJSON_CreateObject();
  cJSON_AddNumberToObject(json, "percent", percent);
  cJSON_AddBoolToObject(json, "success", true);
  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t volume_post_handler(httpd_req_t *req) {
  char content[64];
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  content[ret] = '\0';

  cJSON *json = cJSON_Parse(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *response = cJSON_CreateObject();
  cJSON *val = cJSON_GetObjectItem(json, "percent");
  if (val && cJSON_IsNumber(val)) {
    int pct = (int)val->valuedouble;
    if (pct < 0) {
      pct = 0;
    }
    if (pct > 100) {
      pct = 100;
    }
    float db = (float)pct / 100.0f * 30.0f - 30.0f;
    int32_t q15 = airplay_volume_db_to_q15(db);
    airplay_set_volume_q15(q15);
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddNumberToObject(response, "percent", pct);
  } else {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "Expected {\"percent\": 0-100}");
  }

  char *json_str = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t led_brightness_get_handler(httpd_req_t *req) {  cJSON *json = cJSON_CreateObject();
  cJSON_AddNumberToObject(json, "brightness", led_get_brightness());
  cJSON_AddBoolToObject(json, "success", true);
  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t led_brightness_post_handler(httpd_req_t *req) {
  char content[64];
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  content[ret] = '\0';

  cJSON *json = cJSON_Parse(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *response = cJSON_CreateObject();
  cJSON *val = cJSON_GetObjectItem(json, "brightness");
  if (val && cJSON_IsNumber(val)) {
    int b = (int)val->valuedouble;
    if (b < 0) {
      b = 0;
    }
    if (b > 255) {
      b = 255;
    }
    esp_err_t err = led_set_brightness((uint8_t)b);
    if (err == ESP_OK) {
      cJSON_AddBoolToObject(response, "success", true);
    } else {
      cJSON_AddBoolToObject(response, "success", false);
      cJSON_AddStringToObject(response, "error", esp_err_to_name(err));
    }
  } else {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error",
                            "Expected {\"brightness\": 0-255}");
  }

  char *json_str = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);
  return ESP_OK;
}

/* Read a whole request body into a NUL-terminated heap buffer. httpd_req_recv()
 * can return a short read, so keep going until the declared length arrives. */
static char *recv_body(httpd_req_t *req, size_t max_len) {
  int len = req->content_len;
  if (len <= 0 || (size_t)len > max_len) {
    return NULL;
  }
  char *buf = malloc((size_t)len + 1);
  if (!buf) {
    return NULL;
  }
  int got = 0;
  while (got < len) {
    int r = httpd_req_recv(req, buf + got, (size_t)(len - got));
    if (r <= 0) {
      free(buf);
      return NULL;
    }
    got += r;
  }
  buf[len] = '\0';
  return buf;
}

/* True for a JSON number that is a whole value inside [lo, hi]. */
static bool json_int_in_range(const cJSON *v, int lo, int hi) {
  return cJSON_IsNumber(v) && v->valuedouble == (double)v->valueint &&
         v->valueint >= lo && v->valueint <= hi;
}

// ============================================================================
// Web 电台 (HTTP 流播放)
// ============================================================================

static const char *radio_state_name(web_radio_state_t s) {
  switch (s) {
  case WEB_RADIO_CONNECTING:
    return "connecting";
  case WEB_RADIO_PLAYING:
    return "playing";
  case WEB_RADIO_ERROR:
    return "error";
  case WEB_RADIO_IDLE:
  default:
    return "idle";
  }
}

static esp_err_t radio_state_handler(httpd_req_t *req) {
  char url[256];
  char err[160];
  web_radio_get_url(url, sizeof(url));
  web_radio_get_error(err, sizeof(err));

  // 曲目信息: 标题/作者/专辑/第几曲。字段可能为空串 ——
  // 网页据此决定整行要不要渲染 (作者和专辑都没有时不留空标签)。
  web_radio_track_info_t ti;
  web_radio_get_track_info(&ti);

  cJSON *json = cJSON_CreateObject();
  cJSON_AddStringToObject(json, "state",
                          radio_state_name(web_radio_get_state()));
  cJSON_AddBoolToObject(json, "active", web_radio_is_active());
  // url = 用户输入的那个地址 (歌单模式下是歌单地址, 供输入框回显);
  // cur_url = 当前真正在播的那一曲的地址。两者在单曲模式下相同。
  cJSON_AddStringToObject(json, "url", url);
  cJSON_AddStringToObject(json, "cur_url", ti.cur_url);
  cJSON_AddStringToObject(json, "error", err);
  cJSON_AddNumberToObject(json, "index", ti.index);
  cJSON_AddNumberToObject(json, "count", ti.count);
  cJSON_AddBoolToObject(json, "truncated", ti.truncated);
  cJSON_AddStringToObject(json, "title", ti.title);
  cJSON_AddStringToObject(json, "artist", ti.artist);
  cJSON_AddStringToObject(json, "album", ti.album);
  cJSON_AddBoolToObject(json, "success", true);

  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t radio_play_handler(httpd_req_t *req) {
  // URL 最长 255, 留点余量给 JSON 包装
  char *content = recv_body(req, 512);
  if (!content) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  cJSON *json = cJSON_Parse(content);
  free(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *response = cJSON_CreateObject();
  cJSON *url = cJSON_GetObjectItem(json, "url");
  if (!cJSON_IsString(url) || !url->valuestring || url->valuestring[0] == '\0') {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "missing url");
  } else {
    // 谁后点谁抢: 先把 SD 卡播放停掉。用不带 yield 的 stop —— 我们紧接着
    // 就要把 I2S 交给电台, 拉起 AirPlay 的 playback_task 只会来抢 DMA。
    sd_player_stop();

    esp_err_t err = web_radio_play_url(url->valuestring);
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    if (err != ESP_OK) {
      char e[160];
      web_radio_get_error(e, sizeof(e));
      cJSON_AddStringToObject(response, "error", e[0] ? e : "play failed");
    } else {
      cJSON_AddStringToObject(response, "state", "connecting");
    }
  }

  char *json_str = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(response);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t radio_stop_handler(httpd_req_t *req) {
  // 用户主动停止 → 用 stop_and_yield: 除了内部清理, 还要把 I2S 采样率还原、
  // 把 AirPlay 的 playback_task 重新拉起, 否则之后投 AirPlay 会没声音。
  web_radio_stop_and_yield();
  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "success", true);
  cJSON_AddStringToObject(json, "state", "idle");
  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

// 切上一曲 / 下一曲。只在歌单模式下有意义; 单曲模式下 web_radio_skip
// 会自己忽略 (pl_total() == 1)。
static esp_err_t radio_next_handler(httpd_req_t *req) {
  web_radio_skip(+1);
  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "success", true);
  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t radio_prev_handler(httpd_req_t *req) {
  web_radio_skip(-1);
  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "success", true);
  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

// ============================================================================
// SD 卡本地音乐
//
// 存储层是 components/sd_card, 播放器是 components/sd_player。
//
// ⚠️ 路径一律是**卡内相对路径** (以 '/' 开头, 不含 "/sdcard" 前缀),
//    和 sd_card.h 的约定一致 —— 这样面包屑显示出来才是 "/Music"。
//
// ⚠️ 和网络电台是"谁后点谁抢": SD 播放前先停电台, 电台播放前先停 SD。
//    用**不带 yield** 的 stop —— 紧接着就有音源要接管 I2S, 拉起 AirPlay 的
//    playback_task 反而会抢 DMA (见 web_radio.h:164-175 的说明)。
// ============================================================================

// 上传上限。整轨 FLAC 也就几十 MB, 32MB 够宽裕了。
#define SD_UPLOAD_MAX_BYTES (32 * 1024 * 1024)

static const char *sd_state_name(sd_player_state_t s) {
  switch (s) {
  case SD_PLAYER_PLAYING:
    return "playing";
  case SD_PLAYER_ERROR:
    return "error";
  case SD_PLAYER_IDLE:
  default:
    return "idle";
  }
}

// URL 解码 (%XX -> 字节, '+' -> 空格)。
//
// ⚠️ ESP-IDF 的 httpd_query_key_value() **不做** URL 解码 —— 拿到的是原样的
//    百分号编码。目录名和文件名都可能是中文 (一个汉字 UTF-8 三字节, 编码后是
//    9 个字符), 不解码的话查到的就是 "%E9%9F%B3%E4%B9%90" 这个字符串,
//    opendir 必然打不开。
static int hex_val(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

static void url_decode(const char *src, char *dst, size_t dst_len) {
  if (!dst || dst_len == 0) {
    return;
  }
  size_t o = 0;
  for (const char *p = src; p && *p && o + 1 < dst_len; p++) {
    if (*p == '%' && hex_val(p[1]) >= 0 && hex_val(p[2]) >= 0) {
      dst[o++] = (char)((hex_val(p[1]) << 4) | hex_val(p[2]));
      p += 2;
    } else if (*p == '+') {
      dst[o++] = ' ';
    } else {
      dst[o++] = *p;
    }
  }
  dst[o] = '\0';
}

// 取一个 query 参数并解码。找不到返回 false (out 保持为空串)。
//
// ⚠️ 缓冲给足整条 URI 的长度上限 (CONFIG_HTTPD_MAX_URI_LEN = 1024), 否则
//    httpd_query_key_value 会因放不下而返回 RESULT_TRUNC —— 那种情况下本函数
//    返回 false, 调用者会静默退回默认值 (上传时就会把文件写到根目录而不是
//    用户点开的那个文件夹)。给够就根本不会发生。
static bool query_get(httpd_req_t *req, const char *key, char *out,
                      size_t out_len) {
  char query[1024];
  char raw[1024];

  out[0] = '\0';
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    return false;
  }
  if (httpd_query_key_value(query, key, raw, sizeof(raw)) != ESP_OK) {
    return false;
  }
  url_decode(raw, out, out_len);
  return out[0] != '\0';
}

// 算上一级目录, 供网页的「返回上级」用。根目录的上一级还是根目录。
static void parent_dir(const char *dir, char *out, size_t out_len) {
  strlcpy(out, dir, out_len);
  if (strcmp(out, "/") == 0) {
    return;
  }
  char *slash = strrchr(out, '/');
  if (!slash) {
    strlcpy(out, "/", out_len);
    return;
  }
  if (slash == out) {
    strlcpy(out, "/", out_len); // "/Music" -> "/"
  } else {
    *slash = '\0'; // "/A/B" -> "/A"
  }
}

static esp_err_t sd_state_handler(httpd_req_t *req) {
  sd_player_track_info_t ti;
  sd_player_get_track_info(&ti);

  char err[160];
  sd_player_get_error(err, sizeof(err));

  cJSON *json = cJSON_CreateObject();
  cJSON_AddStringToObject(json, "state",
                          sd_state_name(sd_player_get_state()));
  cJSON_AddBoolToObject(json, "active", sd_player_is_active());
  cJSON_AddBoolToObject(json, "mounted", sd_card_is_mounted());
  cJSON_AddStringToObject(json, "error", err);
  cJSON_AddNumberToObject(json, "index", ti.index);
  cJSON_AddNumberToObject(json, "count", ti.count);
  cJSON_AddBoolToObject(json, "truncated", ti.truncated);
  cJSON_AddStringToObject(json, "title", ti.title);
  cJSON_AddStringToObject(json, "artist", ti.artist);
  cJSON_AddStringToObject(json, "album", ti.album);
  cJSON_AddStringToObject(json, "path", ti.path);
  cJSON_AddNumberToObject(json, "elapsed", ti.elapsed_sec);
  cJSON_AddNumberToObject(json, "sample_rate", ti.sample_rate);
  cJSON_AddBoolToObject(json, "success", true);

  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t sd_list_handler(httpd_req_t *req) {
  char dir[SD_CARD_PATH_MAX];
  if (!query_get(req, "dir", dir, sizeof(dir))) {
    strlcpy(dir, "/", sizeof(dir)); // 没给就用根目录
  }

  cJSON *json = cJSON_CreateObject();
  cJSON_AddStringToObject(json, "dir", dir);

  // 每次拉目录都顺手重试一次挂载 (内部带 10 秒节流) —— 这样用户热插卡之后
  // 刷新网页就能看到内容, 不用重启设备。
  sd_card_ensure_mounted();

  if (!sd_card_is_mounted()) {
    cJSON_AddBoolToObject(json, "mounted", false);
    cJSON_AddBoolToObject(json, "success", true); // 不是错误, 只是没卡
    char *json_str = cJSON_Print(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    free(json_str);
    cJSON_Delete(json);
    return ESP_OK;
  }

  sd_entry_t *entries = NULL;
  size_t n = 0;
  bool truncated = false;
  esp_err_t err = sd_card_list(dir, &entries, &n, &truncated);

  if (err != ESP_OK) {
    cJSON_AddBoolToObject(json, "mounted", true);
    cJSON_AddBoolToObject(json, "success", false);
    cJSON_AddStringToObject(json, "error", err == ESP_ERR_NOT_FOUND
                                               ? "目录不存在"
                                               : "目录打不开");
    char *json_str = cJSON_Print(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    free(json_str);
    cJSON_Delete(json);
    return ESP_OK;
  }

  cJSON *arr = cJSON_CreateArray();
  for (size_t i = 0; i < n; i++) {
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "name", entries[i].name);
    cJSON_AddBoolToObject(item, "is_dir", entries[i].is_dir);
    cJSON_AddNumberToObject(item, "size", (double)entries[i].size);
    // audio 只对文件有意义: 网页据此决定这一行能不能点播
    cJSON_AddBoolToObject(item, "audio",
                          !entries[i].is_dir &&
                              sd_card_is_audio_file(entries[i].name));
    cJSON_AddItemToArray(arr, item);
  }
  sd_card_free_list(entries);

  char parent[SD_CARD_PATH_MAX];
  parent_dir(dir, parent, sizeof(parent));

  uint64_t total = 0, free_b = 0;
  sd_card_info(&total, &free_b);

  cJSON_AddStringToObject(json, "parent", parent);
  cJSON_AddBoolToObject(json, "mounted", true);
  cJSON_AddBoolToObject(json, "truncated", truncated);
  cJSON_AddNumberToObject(json, "total", (double)total);
  cJSON_AddNumberToObject(json, "free", (double)free_b);
  cJSON_AddItemToObject(json, "entries", arr);
  cJSON_AddBoolToObject(json, "success", true);

  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

// POST /api/sd/play  body: {"path": "/Music/xxx.mp3"}
static esp_err_t sd_play_handler(httpd_req_t *req) {
  char *content = recv_body(req, 1024);
  if (!content) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  cJSON *json = cJSON_Parse(content);
  free(content);

  cJSON *response = cJSON_CreateObject();
  if (!json) {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "JSON 解析失败");
  } else {
    cJSON *path = cJSON_GetObjectItem(json, "path");
    if (!cJSON_IsString(path) || !path->valuestring ||
        path->valuestring[0] == '\0') {
      cJSON_AddBoolToObject(response, "success", false);
      cJSON_AddStringToObject(response, "error", "缺少 path");
    } else {
      // 谁后点谁抢: 先把网络电台停掉。用不带 yield 的 stop —— 我们紧接着
      // 就要自己接管 I2S, 拉起 AirPlay 的 playback_task 只会来抢 DMA。
      web_radio_stop();

      esp_err_t err = sd_player_play(path->valuestring);
      cJSON_AddBoolToObject(response, "success", err == ESP_OK);
      if (err != ESP_OK) {
        char e[160];
        sd_player_get_error(e, sizeof(e));
        cJSON_AddStringToObject(response, "error", e[0] ? e : "播放失败");
      } else {
        cJSON_AddStringToObject(response, "state", "playing");
      }
    }
    cJSON_Delete(json);
  }

  char *json_str = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(response);
  return ESP_OK;
}

// 停止 / 上一条 / 下一条。响应体形状一致。
static esp_err_t sd_simple_response(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "success", true);
  cJSON_AddStringToObject(json, "state", sd_state_name(sd_player_get_state()));
  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t sd_stop_handler(httpd_req_t *req) {
  // 用户主动停止 → 用 stop_and_yield: 除了内部清理, 还要把 I2S 采样率还原、
  // 把 AirPlay 的 playback_task 重新拉起, 否则之后投 AirPlay 会没声音。
  sd_player_stop_and_yield();
  return sd_simple_response(req);
}

static esp_err_t sd_next_handler(httpd_req_t *req) {
  sd_player_skip(+1);
  return sd_simple_response(req);
}

static esp_err_t sd_prev_handler(httpd_req_t *req) {
  sd_player_skip(-1);
  return sd_simple_response(req);
}

// POST /api/sd/mount —— 网页上的「重新挂载」按钮。不节流, 用户点一次就真探一次。
static esp_err_t sd_mount_handler(httpd_req_t *req) {
  esp_err_t err = sd_card_init();
  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "success", err == ESP_OK);
  cJSON_AddBoolToObject(json, "mounted", sd_card_is_mounted());
  if (err != ESP_OK) {
    cJSON_AddStringToObject(json, "error", esp_err_to_name(err));
  }
  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

// POST /api/sd/delete  body: {"path": "/Music/xxx.mp3"}
static esp_err_t sd_delete_handler(httpd_req_t *req) {
  char *content = recv_body(req, 1024);
  if (!content) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  cJSON *json = cJSON_Parse(content);
  free(content);

  cJSON *response = cJSON_CreateObject();
  if (!json) {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "JSON 解析失败");
  } else {
    cJSON *path = cJSON_GetObjectItem(json, "path");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(path) && path->valuestring &&
        path->valuestring[0] != '\0') {
      err = sd_card_delete(path->valuestring);
    }
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    if (err != ESP_OK) {
      cJSON_AddStringToObject(response, "error",
                              err == ESP_ERR_NOT_FOUND ? "文件不存在"
                              : err == ESP_ERR_INVALID_ARG ? "路径非法"
                                                           : "删除失败");
    }
    cJSON_Delete(json);
  }

  char *json_str = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(response);
  return ESP_OK;
}

// POST /api/sd/mkdir  body: {"dir": "/Music/NewFolder"}
static esp_err_t sd_mkdir_handler(httpd_req_t *req) {
  char *content = recv_body(req, 1024);
  if (!content) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  cJSON *json = cJSON_Parse(content);
  free(content);

  cJSON *response = cJSON_CreateObject();
  if (!json) {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "JSON 解析失败");
  } else {
    cJSON *dir = cJSON_GetObjectItem(json, "dir");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(dir) && dir->valuestring && dir->valuestring[0]) {
      err = sd_card_mkdir(dir->valuestring);
    }
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    if (err != ESP_OK) {
      cJSON_AddStringToObject(response, "error", "建目录失败 (已存在?)");
    }
    cJSON_Delete(json);
  }

  char *json_str = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(response);
  return ESP_OK;
}

// POST /api/sd/upload?dir=/Music&name=xxx.mp3
// body = 文件原始字节 (不是 multipart —— 和现有的 /api/fs/upload 一个做法,
// 网页那边用 fetch(url, {method:'POST', body:file}) 直接传 File 对象)。
//
// ⚠️ 校验逻辑分两步:
//   1. sd_card_path_check() 保证 dir 是卡内的合法相对路径 (无 "..")
//   2. name 里**不允许**出现 '/' —— 否则可以借文件名跳出 dir
static esp_err_t sd_upload_handler(httpd_req_t *req) {
  char dir[SD_CARD_PATH_MAX];
  char raw_name[SD_CARD_NAME_MAX];

  if (!query_get(req, "dir", dir, sizeof(dir))) {
    strlcpy(dir, "/", sizeof(dir));
  }
  if (!query_get(req, "name", raw_name, sizeof(raw_name))) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'name'");
    return ESP_FAIL;
  }

  if (sd_card_path_check(dir) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad 'dir'");
    return ESP_FAIL;
  }
  if (strchr(raw_name, '/') || strchr(raw_name, '\\') ||
      strcmp(raw_name, ".") == 0 || strcmp(raw_name, "..") == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad 'name'");
    return ESP_FAIL;
  }

  if (req->content_len == 0 ||
      req->content_len > (size_t)SD_UPLOAD_MAX_BYTES) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad content length");
    return ESP_FAIL;
  }

  if (!sd_card_is_mounted()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD not mounted");
    return ESP_FAIL;
  }

  // UTF-8 的相对路径 —— 既回显给网页 (也就是它刚传上来的那个名字)，也直接交给
  // sd_card_full_path() 去建文件。拼不下就直接拒掉: 与其建出一个名字被截断的
  // 文件, 不如让用户看到错误。
  char rel[SD_CARD_PATH_MAX];
  int rl;
  if (strcmp(dir, "/") == 0) {
    rl = snprintf(rel, sizeof(rel), "/%s", raw_name);
  } else {
    rl = snprintf(rel, sizeof(rel), "%s/%s", dir, raw_name);
  }
  if (rl < 0 || (size_t)rl >= sizeof(rel)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path too long");
    return ESP_FAIL;
  }

  // UTF-8 -> GBK 由 sd_card_full_path() 统一负责 (见 sd_card.c)，所以这里不必
  // 再自己拼一份 GBK 路径。
  char vfs[sizeof(SD_CARD_MOUNT_POINT) + SD_CARD_PATH_MAX + 8];
  if (sd_card_full_path(rel, vfs, sizeof(vfs)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path too long");
    return ESP_FAIL;
  }

  FILE *f = fopen(vfs, "wb");
  if (!f) {
    ESP_LOGE(TAG, "Failed to create %s", vfs);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Failed to create file");
    return ESP_FAIL;
  }

  // 分块转发, **不**整块进内存 —— 一首无损歌几十 MB, 塞不进也毫无必要。
  char buf[SPIFFS_CHUNK_SIZE];
  size_t remaining = req->content_len;
  while (remaining > 0) {
    size_t to_read = remaining < sizeof(buf) ? remaining : sizeof(buf);
    int received = httpd_req_recv(req, buf, to_read);
    if (received <= 0) {
      fclose(f);
      remove(vfs); // 半截文件不要留在卡上
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "Receive failed");
      return ESP_FAIL;
    }
    if (fwrite(buf, 1, (size_t)received, f) != (size_t)received) {
      fclose(f);
      remove(vfs); // 卡满了
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "Write failed (card full?)");
      return ESP_FAIL;
    }
    remaining -= (size_t)received;
  }
  fclose(f);

  ESP_LOGI(TAG, "Uploaded %u bytes to %s", (unsigned)req->content_len, rel);

  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "success", true);
  cJSON_AddNumberToObject(json, "size", (double)req->content_len);
  cJSON_AddStringToObject(json, "path", rel);
  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t channel_mode_get_handler(httpd_req_t *req) {  cJSON *json = cJSON_CreateObject();
  cJSON_AddNumberToObject(json, "mode", audio_output_get_channel_mode());
  cJSON_AddBoolToObject(json, "locked", audio_output_channel_mode_locked());
  cJSON_AddBoolToObject(json, "dsp", audio_output_channel_mode_in_dsp());
  cJSON_AddBoolToObject(json, "success", true);
  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t channel_mode_post_handler(httpd_req_t *req) {
  char *content = recv_body(req, 128);
  if (!content) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  cJSON *json = cJSON_Parse(content);
  free(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *response = cJSON_CreateObject();
  cJSON *val = cJSON_GetObjectItem(json, "mode");
  if (json_int_in_range(val, AUDIO_CHANNEL_STEREO, AUDIO_CHANNEL_MONO)) {
    audio_output_set_channel_mode((audio_channel_mode_t)val->valueint);
    cJSON_AddBoolToObject(response, "success", true);
    /* Boards with two amplifiers keep stereo whatever was asked for. */
    cJSON_AddNumberToObject(response, "mode", audio_output_get_channel_mode());
    cJSON_AddBoolToObject(response, "locked",
                          audio_output_channel_mode_locked());
  } else {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "Expected {\"mode\": 0-3}");
  }

  char *json_str = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);
  return ESP_OK;
}

#ifdef DAC_HAS_SUB_OFFSET
#ifdef CONFIG_DAC_TAS58XX
/* The NVS blob layout and the driver's band count are declared independently,
 * so a change to either would silently truncate or overrun the other. */
_Static_assert(SETTINGS_WAY_BANDS == TAS58XX_WAY_BANDS,
               "settings/driver per-way band count mismatch");
_Static_assert(SETTINGS_EQ_BANDS == TAS58XX_EQ_BANDS,
               "settings/driver EQ band count mismatch");

/* Both crossovers expose their EQ as 12-float curves plus the band centres
 * they currently sit on, which move whenever the crossover moves. */
static void way_add_array(cJSON *parent, const char *name,
                          const float vals[TAS58XX_WAY_BANDS], bool whole_hz) {
  cJSON *arr = cJSON_CreateArray();
  for (int i = 0; i < TAS58XX_WAY_BANDS; i++) {
    cJSON_AddItemToArray(
        arr, cJSON_CreateNumber(whole_hz ? (double)(int)(vals[i] + 0.5f)
                                         : (double)vals[i]));
  }
  cJSON_AddItemToObject(parent, name, arr);
}

static bool way_read_array(const cJSON *obj, const char *name,
                           float out[TAS58XX_WAY_BANDS]) {
  const cJSON *arr = cJSON_GetObjectItem(obj, name);
  if (!arr || !cJSON_IsArray(arr) ||
      cJSON_GetArraySize(arr) != TAS58XX_WAY_BANDS) {
    return false;
  }
  for (int i = 0; i < TAS58XX_WAY_BANDS; i++) {
    const cJSON *item = cJSON_GetArrayItem(arr, i);
    if (!cJSON_IsNumber(item)) {
      return false;
    }
    out[i] = (float)item->valuedouble;
  }
  return true;
}

static void sub_eq_add_freqs(cJSON *parent) {
  float f[TAS58XX_WAY_BANDS];
  dac_tas58xx_sub_band_freqs(TAS58XX_WAY_LOW, f);
  way_add_array(parent, "freqs_low", f, true);
  dac_tas58xx_sub_band_freqs(TAS58XX_WAY_HIGH, f);
  way_add_array(parent, "freqs_high", f, true);
}

/* The driver only relayouts and flattens the bands when the corner actually
 * moves, so hand the resulting state back rather than let the client guess. */
static void sub_eq_add_state(cJSON *parent) {
  cJSON_AddBoolToObject(parent, "eq_active", dac_tas58xx_sub_eq_active());
  sub_eq_add_freqs(parent);

  cJSON *gains = cJSON_CreateObject();
  float curve[TAS58XX_WAY_BANDS];
  dac_tas58xx_sub_eq_get_gains(TAS58XX_WAY_LOW, curve);
  way_add_array(gains, "sub", curve, false);
  dac_tas58xx_sub_eq_get_gains(TAS58XX_WAY_HIGH, curve);
  way_add_array(gains, "sat", curve, false);
  cJSON_AddItemToObject(parent, "gains", gains);
}

static esp_err_t sub_eq_persist(void) {
  float saved[2][SETTINGS_WAY_BANDS];
  dac_tas58xx_sub_eq_get_gains(TAS58XX_WAY_LOW, saved[0]);
  dac_tas58xx_sub_eq_get_gains(TAS58XX_WAY_HIGH, saved[1]);
  return settings_set_sub_eq(saved);
}
#endif /* CONFIG_DAC_TAS58XX */

static esp_err_t sub_offset_get_handler(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  cJSON_AddNumberToObject(json, "offset", dac_get_sub_offset_db());
  cJSON_AddNumberToObject(json, "min", DAC_SUB_OFFSET_MIN_DB);
  cJSON_AddNumberToObject(json, "max", DAC_SUB_OFFSET_MAX_DB);
  cJSON_AddBoolToObject(json, "available", dac_has_sub());
#ifdef CONFIG_DAC_TAS58XX
  cJSON_AddNumberToObject(json, "crossover",
                          dac_tas58xx_get_sub_crossover_hz());
  cJSON_AddNumberToObject(json, "xo_min", TAS58XX_XOVER_MIN_HZ);
  cJSON_AddNumberToObject(json, "xo_max", TAS58XX_XOVER_MAX_HZ);
  cJSON_AddNumberToObject(json, "bands", TAS58XX_WAY_BANDS);
  cJSON_AddNumberToObject(json, "min_db", TAS58XX_EQ_MIN_GAIN_DB);
  cJSON_AddNumberToObject(json, "max_db", TAS58XX_EQ_MAX_GAIN_DB);
  sub_eq_add_state(json);
#endif
  cJSON_AddBoolToObject(json, "success", true);
  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t sub_offset_post_handler(httpd_req_t *req) {
  char *content = recv_body(req, 2048);
  if (!content) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
    return ESP_FAIL;
  }

  cJSON *json = cJSON_Parse(content);
  free(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *response = cJSON_CreateObject();
  bool handled = false;
  esp_err_t save_err = ESP_OK;

  cJSON *val = cJSON_GetObjectItem(json, "offset");
  if (val && cJSON_IsNumber(val)) {
    float off = (float)val->valuedouble;
    if (off < DAC_SUB_OFFSET_MIN_DB) {
      off = DAC_SUB_OFFSET_MIN_DB;
    }
    if (off > DAC_SUB_OFFSET_MAX_DB) {
      off = DAC_SUB_OFFSET_MAX_DB;
    }
    dac_set_sub_offset_db(off);
    settings_set_sub_offset(off);
    handled = true;
  }

#ifdef CONFIG_DAC_TAS58XX
  cJSON *xo = cJSON_GetObjectItem(json, "crossover");
  if (xo && cJSON_IsNumber(xo)) {
    float hz = (float)xo->valuedouble;
    dac_tas58xx_set_sub_crossover_hz(hz);
    settings_set_sub_crossover(dac_tas58xx_get_sub_crossover_hz());
    /* Moving the corner relayouts the bands and flattens the curves. */
    save_err = sub_eq_persist();
    handled = true;
  }

  cJSON *gains = cJSON_GetObjectItem(json, "gains");
  if (gains && cJSON_IsObject(gains)) {
    float curve[TAS58XX_WAY_BANDS];
    bool any = false;
    if (way_read_array(gains, "sub", curve)) {
      dac_tas58xx_sub_eq_set_gains(TAS58XX_WAY_LOW, curve);
      any = true;
    }
    if (way_read_array(gains, "sat", curve)) {
      dac_tas58xx_sub_eq_set_gains(TAS58XX_WAY_HIGH, curve);
      any = true;
    }
    if (any) {
      esp_err_t e = sub_eq_persist();
      if (save_err == ESP_OK) {
        save_err = e;
      }
      handled = true;
    }
  }
#endif

  if (handled && save_err != ESP_OK) {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "已生效，但未能保存");
  } else if (handled) {
    cJSON_AddBoolToObject(response, "success", true);
#ifdef CONFIG_DAC_TAS58XX
    sub_eq_add_state(response);
#endif
  } else {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error",
                            "Expected {\"offset\": dB} or {\"crossover\": Hz}");
  }

  char *json_str = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);
  return ESP_OK;
}
#endif /* DAC_HAS_SUB_OFFSET */

#ifdef CONFIG_DAC_TAS58XX
/* Second-amplifier role on dual-DAC boards: bridged mono sub or bi-amp. */
static esp_err_t dual_mode_get_handler(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  cJSON_AddNumberToObject(json, "devices", dac_tas58xx_get_device_count());
  cJSON_AddNumberToObject(json, "mode", dac_tas58xx_get_dual_mode());
  cJSON_AddBoolToObject(json, "restart_required",
                        dac_tas58xx_get_dual_mode() !=
                            dac_tas58xx_get_active_dual_mode());
  cJSON_AddBoolToObject(json, "biamp", TAS58XX_BIAMP_SUPPORTED);
  cJSON_AddBoolToObject(json, "success", true);
  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t dual_mode_post_handler(httpd_req_t *req) {
  char *content = recv_body(req, 128);
  if (!content) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  cJSON *json = cJSON_Parse(content);
  free(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *response = cJSON_CreateObject();
  cJSON *val = cJSON_GetObjectItem(json, "mode");
  if (!json_int_in_range(val, TAS58XX_DUAL_SUB, TAS58XX_DUAL_BIAMP)) {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "Expected {\"mode\": 0-1}");
  } else if (val->valueint == TAS58XX_DUAL_BIAMP && !TAS58XX_BIAMP_SUPPORTED) {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "本机不支持双路放大");
  } else {
    dac_tas58xx_set_dual_mode((tas58xx_dual_mode_t)val->valueint);
    settings_set_dual_mode((uint8_t)val->valueint);
    cJSON_AddBoolToObject(response, "success", true);
    /* PBTL is a control-port setting that can only be changed while the
     * output stage is idle, so the change lands on the next boot. */
    cJSON_AddBoolToObject(response, "restart_required", true);
  }

  char *json_str = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);
  return ESP_OK;
}

/* Bi-amp: woofer/tweeter crossover plus a 12-band EQ per way per speaker. */
static void biamp_add_gain_array(cJSON *parent, const char *name, int dev,
                                 tas58xx_way_t way) {
  float gains[TAS58XX_WAY_BANDS];
  dac_tas58xx_biamp_get_gains(dev, way, gains);
  way_add_array(parent, name, gains, false);
}

static void biamp_add_freqs(cJSON *parent) {
  float f[TAS58XX_WAY_BANDS];
  dac_tas58xx_biamp_band_freqs(TAS58XX_WAY_LOW, f);
  way_add_array(parent, "freqs_low", f, true);
  dac_tas58xx_biamp_band_freqs(TAS58XX_WAY_HIGH, f);
  way_add_array(parent, "freqs_high", f, true);
}

/* The driver only relayouts and flattens the bands when the corner actually
 * moves, so hand the resulting state back rather than let the client guess. */
static void biamp_add_state(cJSON *parent) {
  biamp_add_freqs(parent);

  cJSON *gains = cJSON_CreateObject();
  biamp_add_gain_array(gains, "l_low", 0, TAS58XX_WAY_LOW);
  biamp_add_gain_array(gains, "l_high", 0, TAS58XX_WAY_HIGH);
  biamp_add_gain_array(gains, "r_low", 1, TAS58XX_WAY_LOW);
  biamp_add_gain_array(gains, "r_high", 1, TAS58XX_WAY_HIGH);
  cJSON_AddItemToObject(parent, "gains", gains);
}

static esp_err_t biamp_get_handler(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "active", dac_tas58xx_biamp_active());
  cJSON_AddNumberToObject(json, "crossover",
                          dac_tas58xx_get_biamp_crossover_hz());
  cJSON_AddNumberToObject(json, "xo_min", TAS58XX_BIAMP_XOVER_MIN_HZ);
  cJSON_AddNumberToObject(json, "xo_max", TAS58XX_BIAMP_XOVER_MAX_HZ);
  cJSON_AddBoolToObject(json, "swap", dac_tas58xx_get_biamp_swap());
  cJSON_AddNumberToObject(json, "bands", TAS58XX_WAY_BANDS);
  cJSON_AddNumberToObject(json, "min_db", TAS58XX_EQ_MIN_GAIN_DB);
  cJSON_AddNumberToObject(json, "max_db", TAS58XX_EQ_MAX_GAIN_DB);
  biamp_add_state(json);

  cJSON_AddBoolToObject(json, "success", true);
  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

/* Copy one "gains" member into the driver. Returns true if it was present. */
static bool biamp_apply_gain_member(const cJSON *gains, const char *name,
                                    int dev, tas58xx_way_t way) {
  float vals[TAS58XX_WAY_BANDS];
  if (!way_read_array(gains, name, vals)) {
    return false;
  }
  dac_tas58xx_biamp_set_gains(dev, way, vals);
  return true;
}

static esp_err_t biamp_persist_gains(void) {
  float saved[2][2][SETTINGS_WAY_BANDS];
  for (int spk = 0; spk < 2; spk++) {
    dac_tas58xx_biamp_get_gains(spk, TAS58XX_WAY_LOW, saved[spk][0]);
    dac_tas58xx_biamp_get_gains(spk, TAS58XX_WAY_HIGH, saved[spk][1]);
  }
  return settings_set_biamp_eq(saved);
}

static esp_err_t biamp_post_handler(httpd_req_t *req) {
  char *content = recv_body(req, 2048);
  if (!content) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
    return ESP_FAIL;
  }

  cJSON *json = cJSON_Parse(content);
  free(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  bool handled = false;
  esp_err_t save_err = ESP_OK;
  cJSON *response = cJSON_CreateObject();

  cJSON *xo = cJSON_GetObjectItem(json, "crossover");
  if (xo && cJSON_IsNumber(xo)) {
    dac_tas58xx_set_biamp_crossover_hz((float)xo->valuedouble);
    settings_set_biamp_crossover(dac_tas58xx_get_biamp_crossover_hz());
    /* Moving the corner relayouts the bands and flattens the curves. */
    save_err = biamp_persist_gains();
    handled = true;
  }

  cJSON *swap = cJSON_GetObjectItem(json, "swap");
  if (swap && cJSON_IsBool(swap)) {
    dac_tas58xx_set_biamp_swap(cJSON_IsTrue(swap));
    settings_set_biamp_swap(dac_tas58xx_get_biamp_swap());
    handled = true;
  }

  cJSON *gains = cJSON_GetObjectItem(json, "gains");
  if (gains && cJSON_IsObject(gains)) {
    bool any = biamp_apply_gain_member(gains, "l_low", 0, TAS58XX_WAY_LOW);
    any |= biamp_apply_gain_member(gains, "l_high", 0, TAS58XX_WAY_HIGH);
    any |= biamp_apply_gain_member(gains, "r_low", 1, TAS58XX_WAY_LOW);
    any |= biamp_apply_gain_member(gains, "r_high", 1, TAS58XX_WAY_HIGH);
    if (any) {
      esp_err_t e = biamp_persist_gains();
      if (save_err == ESP_OK) {
        save_err = e;
      }
      handled = true;
    }
  }

  if (handled && save_err != ESP_OK) {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "已生效，但未能保存");
  } else if (handled) {
    cJSON_AddBoolToObject(response, "success", true);
    /* Band centres track the crossover, so hand the new layout back. */
    biamp_add_state(response);
  } else {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(
        response, "error", 需要 crossover / swap / gains 字段);
  }

  char *json_str = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);
  return ESP_OK;
}
#endif /* CONFIG_DAC_TAS58XX */

static const char *reset_reason_str(esp_reset_reason_t r) {
  switch (r) {
  case ESP_RST_POWERON:
    return "poweron";
  case ESP_RST_EXT:
    return "external";
  case ESP_RST_SW:
    return "software";
  case ESP_RST_PANIC:
    return "panic";
  case ESP_RST_INT_WDT:
    return "int_wdt";
  case ESP_RST_TASK_WDT:
    return "task_wdt";
  case ESP_RST_WDT:
    return "other_wdt";
  case ESP_RST_DEEPSLEEP:
    return "deepsleep";
  case ESP_RST_BROWNOUT:
    return "brownout";
  case ESP_RST_SDIO:
    return "sdio";
  default:
    return "unknown";
  }
}

static esp_err_t system_info_handler(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  cJSON *info = cJSON_CreateObject();

  char ip_str[16] = {0};
  char mac_str[18] = {0};
  char device_name[65] = {0};
  bool wifi_connected = wifi_is_connected();
  bool eth_connected = ethernet_is_connected();

  // Show IP and MAC for the active interface
  if (eth_connected) {
    ethernet_get_ip_str(ip_str, sizeof(ip_str));
    ethernet_get_mac_str(mac_str, sizeof(mac_str));
  } else {
    wifi_get_ip_str(ip_str, sizeof(ip_str));
    wifi_get_mac_str(mac_str, sizeof(mac_str));
  }
  settings_get_device_name(device_name, sizeof(device_name));

  cJSON_AddStringToObject(info, "ip", ip_str);
  cJSON_AddStringToObject(info, "mac", mac_str);
  cJSON_AddStringToObject(info, "device_name", device_name);
  cJSON_AddBoolToObject(info, "wifi_connected", wifi_connected);
  cJSON_AddBoolToObject(info, "eth_connected", eth_connected);
  cJSON_AddNumberToObject(info, "free_heap", esp_get_free_heap_size());

  // WiFi link diagnostics (only meaningful when associated as STA)
  if (wifi_connected) {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
      char ssid_buf[33];
      size_t slen = strnlen((const char *)ap.ssid, sizeof(ap.ssid));
      if (slen > sizeof(ssid_buf) - 1) {
        slen = sizeof(ssid_buf) - 1;
      }
      memcpy(ssid_buf, ap.ssid, slen);
      ssid_buf[slen] = '\0';
      char bssid_buf[18];
      snprintf(bssid_buf, sizeof(bssid_buf), "%02x:%02x:%02x:%02x:%02x:%02x",
               ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4],
               ap.bssid[5]);
      const char *phy = "?";
      if (ap.phy_11n) {
        phy = "11n";
      } else if (ap.phy_11g) {
        phy = "11g";
      } else if (ap.phy_11b) {
        phy = "11b";
      } else if (ap.phy_lr) {
        phy = "LR";
      }
      cJSON_AddStringToObject(info, "wifi_ssid", ssid_buf);
      cJSON_AddStringToObject(info, "wifi_bssid", bssid_buf);
      cJSON_AddNumberToObject(info, "wifi_rssi", ap.rssi);
      cJSON_AddNumberToObject(info, "wifi_channel", ap.primary);
      cJSON_AddStringToObject(info, "wifi_phy", phy);
    }
  }
  // 热点状态。AP 现在常开，"到底还开着吗"要看得见 —— 这个字段的用途就是
  // 让他不必再为"DHCP 换 IP 之后要去路由器后台翻地址"而确认热点。
  {
    bool ap_open = false;
    uint8_t ap_sta = 0;
    if (wifi_ap_get_state(&ap_open, &ap_sta) == ESP_OK) {
      cJSON_AddBoolToObject(info, "ap_open", ap_open);
      cJSON_AddNumberToObject(info, "ap_stations", ap_sta);
      cJSON_AddStringToObject(info, "ap_url",
                              ap_open ? "http://192.168.4.1" : "");
    }
  }
  const esp_app_desc_t *app_desc = esp_app_get_description();
  cJSON_AddStringToObject(info, "firmware_version", app_desc->version);
  // 当前真正在跑的 OTA 槽。给网页升级当"生效证明"用：升级前后这个值应该
  // 从 ota_0 翻到 ota_1（`boot:` 那串日志编码的是 flash 模式，**看不出槽位**）。
  {
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run) {
      cJSON_AddStringToObject(info, "fw_slot", run->label);
    }
  }
  // 网页升级是否可用 = 有没有设口令。**每次现读 NVS，不缓存** —— 缓存一个
  // bool 就又造出一个"改了别处不知道"的陈旧标志（这套代码里同类坑不少）。
  {
    char ota_pw[SETTINGS_OTA_PASSWORD_MAX + 1];
    settings_get_ota_password(ota_pw, sizeof(ota_pw));
    cJSON_AddBoolToObject(info, "ota_enabled", ota_pw[0] != '\0');
  }
  cJSON_AddStringToObject(info, "reset_reason",
                          reset_reason_str(esp_reset_reason()));
  cJSON_AddNumberToObject(info, "uptime_s",
                          (double)(esp_timer_get_time() / 1000000));
  // 当前音频属主（playback_control 枚举）。注意它表达的是「AirPlay 服务在不在
  // / 蓝牙有没有占」，不是「手机正在推流」—— main.c 在 rtsp_server_start()
  // 成功后就置 AIRPLAY。前端措辞必须守住这条边界，别报说不准的状态。
  {
    const char *src = "none";
    switch (playback_control_get_source()) {
      case PLAYBACK_SOURCE_AIRPLAY:
        src = "airplay";
        break;
      case PLAYBACK_SOURCE_BLUETOOTH:
        src = "bluetooth";
        break;
      default:
        break;
    }
    cJSON_AddStringToObject(info, "playback_source", src);
  }
  // AirPlay「正在播放」—— 取面板那份唯一落地的快照（display_get_now_playing），
  // 所以网页看到的和 OLED 上的必然是同一条信息，不存在第二份会不同步的缓存。
  // ⚠️ 判据用 state != STANDBY，**不要**用 audio_receiver_is_playing()：
  //    audio_timing_init() 把它初始化成 true，开机没会话时也报"在放"（§30.4）。
  //    position 已在设备侧按墙钟插值过，前端别再自己加时间。
  {
    display_now_playing_t np;
    const char *np_state = "standby";
    bool have_np = display_get_now_playing(&np);
    if (have_np) {
      switch (np.state) {
        case DISPLAY_NOW_PLAYING_PLAYING:
          np_state = "playing";
          break;
        case DISPLAY_NOW_PLAYING_PAUSED:
          np_state = "paused";
          break;
        case DISPLAY_NOW_PLAYING_CONNECTED:
          np_state = "connected";
          break;
        default:
          break;
      }
    }
    cJSON_AddStringToObject(info, "airplay_state", np_state);
    if (have_np && np.state != DISPLAY_NOW_PLAYING_STANDBY &&
        np.title[0] != '\0') {
      cJSON_AddStringToObject(info, "airplay_title", np.title);
      if (np.artist[0] != '\0') {
        cJSON_AddStringToObject(info, "airplay_artist", np.artist);
      }
      if (np.album[0] != '\0') {
        cJSON_AddStringToObject(info, "airplay_album", np.album);
      }
      if (np.duration_secs > 0) {
        cJSON_AddNumberToObject(info, "airplay_duration_s",
                                (double)np.duration_secs);
        cJSON_AddNumberToObject(info, "airplay_position_s",
                                (double)np.position_secs);
      }
    }
  }
#ifdef CONFIG_AHT20_ENABLED
  // 温湿度只读缓存 —— 一次真实测量要等 ~80 ms，不能出现在 httpd handler 里。
  // 读不到就不给字段（前端据此隐藏），不要填 0：0 °C 看着像真值。
  {
    float tc = 0, rh = 0;
    int age = 0;
    if (aht20_get(&tc, &rh, &age)) {
      cJSON_AddBoolToObject(info, "sensor_ok", true);
      cJSON_AddNumberToObject(info, "temperature_c", (double)tc);
      cJSON_AddNumberToObject(info, "humidity_pct", (double)rh);
      cJSON_AddNumberToObject(info, "sensor_age_s", age);
    } else {
      cJSON_AddBoolToObject(info, "sensor_ok", false);
    }
  }
#endif
#ifdef CONFIG_TIME_SYNC_ENABLED
  // 时间由设备侧格式化后下发：让浏览器去做时区换算就会显示成"访问者的时区"，
  // 而这台设备的墙钟是按 CONFIG_TIME_SYNC_TZ 定的。
  {
    char ts[32];
    cJSON_AddBoolToObject(info, "time_synced", time_sync_is_synced());
    if (time_sync_local_string(ts, sizeof(ts))) {
      cJSON_AddStringToObject(info, "time_local", ts);
    }
    if (time_sync_boot_string(ts, sizeof(ts))) {
      cJSON_AddStringToObject(info, "boot_time_local", ts);
    }
  }
#endif
#ifdef CONFIG_DAC_TAS58XX
  cJSON_AddBoolToObject(info, "eq_supported", true);
#else
  cJSON_AddBoolToObject(info, "eq_supported", false);
#endif
#ifdef DAC_HAS_SUB_OFFSET
  cJSON_AddBoolToObject(info, "sub_supported", dac_has_sub());
#else
  cJSON_AddBoolToObject(info, "sub_supported", false);
#endif
#ifdef CONFIG_DAC_TAS58XX
  cJSON_AddBoolToObject(info, "dual_supported", true);
#else
  cJSON_AddBoolToObject(info, "dual_supported", false);
#endif

  cJSON_AddItemToObject(json, "info", info);
  cJSON_AddBoolToObject(json, "success", true);

  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);

  return ESP_OK;
}

// ============================================================================
// 固件升级 (OTA)
//
// 分工：策略（谁能刷、现在能不能刷）在下面两个 handler 里，收包/校验/写 flash
// 在 network/ota.c。口令**永远不要**写进日志。
// ============================================================================

#define OTA_PW_HEADER "X-OTA-Password"

// 口令比较不许提前返回：逐字节异或累加，长度不同也算不等。
static bool secret_equal(const char *a, const char *b) {
  size_t la = strlen(a);
  size_t lb = strlen(b);
  unsigned diff = (unsigned)(la ^ lb);
  size_t n = la < lb ? la : lb;
  for (size_t i = 0; i < n; i++) {
    diff |= (unsigned)((unsigned char)a[i] ^ (unsigned char)b[i]);
  }
  return diff == 0;
}

static esp_err_t ota_update_handler(httpd_req_t *req) {
  char want[SETTINGS_OTA_PASSWORD_MAX + 1];
  settings_get_ota_password(want, sizeof(want));

  if (want[0] == '\0') {
    // **默认关闭**：这套控制面整体没有鉴权，升级接口是唯一能"在局域网里执行
    // 任意代码"的入口，所以没设口令时宁可 403，也不要出现"忘了设 = 谁都能刷"。
    ESP_LOGW(TAG, "拒绝升级: 未设置升级口令（网页升级处于关闭状态）");
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_sendstr(req, "web update disabled: set an upgrade passphrase "
                            "first");
    return ESP_OK;
  }

  char got[SETTINGS_OTA_PASSWORD_MAX + 1];
  got[0] = '\0';
  // 缓冲区不够大时 httpd 返回 TRUNC 而不是截断写入 —— 当成"不对"处理，
  // 免得长口令被截成前缀后反而匹配上。
  if (httpd_req_get_hdr_value_str(req, OTA_PW_HEADER, got, sizeof(got)) !=
      ESP_OK) {
    ESP_LOGW(TAG, "拒绝升级: 缺少或过长的 %s 头", OTA_PW_HEADER);
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_sendstr(req, "missing or too long " OTA_PW_HEADER);
    return ESP_OK;
  }
  if (!secret_equal(want, got)) {
    ESP_LOGW(TAG, "拒绝升级: 口令不对");
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_sendstr(req, "bad passphrase");
    return ESP_OK;
  }

  if (req->content_len == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No firmware uploaded");
    return ESP_FAIL;
  }

  // SD / 网络音乐正在播时不许升级：写 flash 会关 cache，那两路正在拿 I2S 的
  // 任务会被硬拖住，症状是"升级过程中音频炸掉 + 不确定谁先恢复"。与其猜，
  // 不如让人先停。（AirPlay 那一路由 rtsp_server_stop() 正常摘掉。）
  if ((sd_player_is_active()) || (web_radio_is_active())) {
    ESP_LOGW(TAG, "拒绝升级: 本地播放正在进行，先停止 SD / 网络音乐");
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_sendstr(req, "stop local playback first (SD / web radio)");
    return ESP_OK;
  }

  ESP_LOGI(TAG, "开始网页固件升级 (%zu 字节)", req->content_len);
  rtsp_server_stop();

  esp_err_t err = ota_start_from_http(req);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "固件升级失败: %s", esp_err_to_name(err));
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, esp_err_to_name(err));
    return ESP_FAIL;
  }

  // 先把响应发出去再重启，否则浏览器只会看到一个断掉的连接。
  httpd_resp_sendstr(req, "Firmware update complete, rebooting now!\n");
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();

  return ESP_OK;
}

static esp_err_t ota_password_handler(httpd_req_t *req) {
  char content[512];
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  content[ret] = '\0';

  cJSON *json = cJSON_Parse(content);
  cJSON *response = cJSON_CreateObject();
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    cJSON_Delete(response);
    return ESP_FAIL;
  }

  cJSON *pw_j = cJSON_GetObjectItem(json, "password");
  cJSON *cur_j = cJSON_GetObjectItem(json, "current");
  const char *pw = (pw_j && cJSON_IsString(pw_j)) ? pw_j->valuestring : NULL;
  const char *cur =
      (cur_j && cJSON_IsString(cur_j)) ? cur_j->valuestring : "";

  char want[SETTINGS_OTA_PASSWORD_MAX + 1];
  settings_get_ota_password(want, sizeof(want));

  // 已经设过口令 → 改它必须带旧口令。否则局域网里任何一台机器都能把口令
  // 换成自己的，然后走 /api/ota/update。第一次设置不需要（不然永远开不了）。
  if (want[0] != '\0' && !secret_equal(want, cur)) {
    ESP_LOGW(TAG, "拒绝修改升级口令: 旧口令不对");
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_sendstr(req, "bad current passphrase");
    cJSON_Delete(json);
    cJSON_Delete(response);
    return ESP_OK;
  }

  if (!pw || strlen(pw) > SETTINGS_OTA_PASSWORD_MAX) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "password missing or too long");
    cJSON_Delete(json);
    cJSON_Delete(response);
    return ESP_OK;
  }

  esp_err_t err = settings_set_ota_password(pw);
  cJSON_AddBoolToObject(response, "success", err == ESP_OK);
  if (err != ESP_OK) {
    cJSON_AddStringToObject(response, "error", esp_err_to_name(err));
  }
  // 只回"现在开着还是关着"，不回口令本身
  cJSON_AddBoolToObject(response, "ota_enabled", pw[0] != '\0');

  char *json_str = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);
  return ESP_OK;
}

static esp_err_t system_restart_handler(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "success", true);

  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);

  ESP_LOGI(TAG, "Restart requested via web interface");
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();

  return ESP_OK;
}

/* ================================================================== */
/*  SPIFFS File Management API                                         */
/* ================================================================== */

// Allowed path prefixes for file upload (prevent writes outside SPIFFS)
static const char *ALLOWED_PREFIXES[] = {"/spiffs/"};

static bool is_path_allowed(const char *path) {
  for (int i = 0; i < sizeof(ALLOWED_PREFIXES) / sizeof(ALLOWED_PREFIXES[0]);
       i++) {
    if (strncmp(path, ALLOWED_PREFIXES[i], strlen(ALLOWED_PREFIXES[i])) == 0) {
      // Reject path traversal
      if (strstr(path, "..") != NULL) {
        return false;
      }
      return true;
    }
  }
  return false;
}

static esp_err_t fs_upload_handler(httpd_req_t *req) {
  // Get target path from query string
  char query[128] = {0};
  char path[64] = {0};

  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "path", path, sizeof(path)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Missing 'path' query parameter");
    return ESP_FAIL;
  }

  if (!is_path_allowed(path)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path not allowed");
    return ESP_FAIL;
  }

  if (req->content_len == 0 || req->content_len > (size_t)(64 * 1024)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body required (max 64KB)");
    return ESP_FAIL;
  }

  FILE *f = fopen(path, "wb");
  if (!f) {
    ESP_LOGE(TAG, "Failed to create %s", path);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Failed to create file");
    return ESP_FAIL;
  }

  char buf[SPIFFS_CHUNK_SIZE];
  size_t remaining = req->content_len;
  while (remaining > 0) {
    size_t to_read = remaining < sizeof(buf) ? remaining : sizeof(buf);
    int received = httpd_req_recv(req, buf, to_read);
    if (received <= 0) {
      fclose(f);
      remove(path);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "Receive failed");
      return ESP_FAIL;
    }
    fwrite(buf, 1, (size_t)received, f);
    remaining -= (size_t)received;
  }
  fclose(f);

  ESP_LOGI(TAG, "Uploaded %u bytes to %s", (unsigned)req->content_len, path);

  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "success", true);
  cJSON_AddNumberToObject(json, "size", (double)req->content_len);
  cJSON_AddStringToObject(json, "path", path);
  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t fs_delete_handler(httpd_req_t *req) {
  char query[128] = {0};
  char path[64] = {0};

  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "path", path, sizeof(path)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Missing 'path' query parameter");
    return ESP_FAIL;
  }

  if (!is_path_allowed(path)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path not allowed");
    return ESP_FAIL;
  }

  cJSON *json = cJSON_CreateObject();
  if (remove(path) == 0) {
    ESP_LOGI(TAG, "Deleted %s", path);
    cJSON_AddBoolToObject(json, "success", true);
  } else {
    cJSON_AddBoolToObject(json, "success", false);
    cJSON_AddStringToObject(json, "error", "File not found");
  }
  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t fs_list_handler(httpd_req_t *req) {
  char query[128] = {0};
  char dir_path[64] = "/spiffs";

  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    httpd_query_key_value(query, "dir", dir_path, sizeof(dir_path));
  }

  if (!is_path_allowed(dir_path) && strcmp(dir_path, "/spiffs") != 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path not allowed");
    return ESP_FAIL;
  }

  DIR *d = opendir(dir_path);
  cJSON *json = cJSON_CreateObject();
  cJSON *files = cJSON_CreateArray();

  if (d) {
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
      cJSON *item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "name", entry->d_name);

      char full_path[320];
      snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
      struct stat st;
      if (stat(full_path, &st) == 0) {
        cJSON_AddNumberToObject(item, "size", (double)st.st_size);
      }
      cJSON_AddItemToArray(files, item);
    }
    closedir(d);
    cJSON_AddBoolToObject(json, "success", true);
  } else {
    cJSON_AddBoolToObject(json, "success", false);
    cJSON_AddStringToObject(json, "error", "Cannot open directory");
  }

  cJSON_AddItemToObject(json, "files", files);
  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

/* ================================================================== */
/*  EQ Page + API  (only when TAS58xx DAC is configured)               */
/* ================================================================== */

#ifdef CONFIG_DAC_TAS58XX

static esp_err_t eq_page_handler(httpd_req_t *req) {
  return serve_spiffs_file(req, "/spiffs/www/eq.html", "text/html");
}

static esp_err_t eq_get_handler(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  cJSON *arr = cJSON_CreateArray();

  float gains[SETTINGS_EQ_BANDS];
  if (settings_get_eq_gains(gains) == ESP_OK) {
    for (int i = 0; i < SETTINGS_EQ_BANDS; i++) {
      cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)gains[i]));
    }
  } else {
    /* No saved EQ — return all zeros (flat) */
    for (int i = 0; i < SETTINGS_EQ_BANDS; i++) {
      cJSON_AddItemToArray(arr, cJSON_CreateNumber(0.0));
    }
  }

  cJSON_AddItemToObject(json, "gains", arr);
  cJSON_AddNumberToObject(json, "bands", SETTINGS_EQ_BANDS);
  cJSON_AddBoolToObject(json, "success", true);

  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t eq_post_handler(httpd_req_t *req) {
  char content[512];
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  content[ret] = '\0';

  cJSON *json = cJSON_Parse(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *response = cJSON_CreateObject();
  cJSON *gains_arr = cJSON_GetObjectItem(json, "gains");

  if (gains_arr && cJSON_IsArray(gains_arr) &&
      cJSON_GetArraySize(gains_arr) == SETTINGS_EQ_BANDS) {

    float gains[SETTINGS_EQ_BANDS];
    for (int i = 0; i < SETTINGS_EQ_BANDS; i++) {
      cJSON *item = cJSON_GetArrayItem(gains_arr, i);
      gains[i] = cJSON_IsNumber(item) ? (float)item->valuedouble : 0.0f;
      /* Clamp */
      if (gains[i] > 15.0f) {
        gains[i] = 15.0f;
      }
      if (gains[i] < -15.0f) {
        gains[i] = -15.0f;
      }
    }

    /* Emit event — listeners (settings + DAC) will handle it */
    eq_event_data_t ev_data;
    memcpy(ev_data.all_bands.gains_db, gains, sizeof(gains));
    eq_events_emit(EQ_EVENT_ALL_BANDS_SET, &ev_data);

    cJSON_AddBoolToObject(response, "success", true);
  } else {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error",
                            "Expected 'gains' array with 15 values");
  }

  char *json_str = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);
  return ESP_OK;
}

#endif /* CONFIG_DAC_TAS58XX */

// ============================================================================
// 请求来源守卫 —— 全服务**唯一**的准入点
// ============================================================================
//
// 这个 HTTP 服务是个没有任何鉴权的控制平面：写 WiFi 凭据、重启、读写 SPIFFS、
// 切音量、起停播放全是裸 POST。局域网内任意主机可访问是"音箱"这个产品形态自带
// 的属性，改不掉；真正能修的是**浏览器这条路**，它比局域网更危险，因为用户并不
// 知道自己访问了设备：
//
//   1) DNS rebinding —— 恶意页面让 evil.com 解析到 192.168.1.5。此后对
//      http://evil.com/api/... 的请求在浏览器眼里是**同源**的，同源策略整个失效。
//      挡法是校验 Host：合法访问的 Host 只会是设备自己的 IP 或 *.local，不会是
//      一个外部域名。
//   2) 跨源 POST —— 页面在 http://evil.com，直接 fetch http://192.168.1.5/...。
//      这种请求一定带 Origin（非 GET 必带；表单提交带 Referer），拿它的主机部分
//      和 Host 比一次就是标准的同源判断。
//
// 两条都需要：rebinding 时 Origin 和 Host 是**互相一致**的（都是 evil.com），
// 单靠同源比较放不过去。
//
// 只读请求头、不需要 token / session，所以**不影响任何非浏览器客户端**：curl、
// 脚本、命令行工具照常工作（它们不带 Origin）。也因此不存在"把网页控制面改到
// 打不开"的风险 —— 浏览器同源访问一定通过。
// 防不住的是局域网里故意构造的裸 POST，那要真鉴权（token/密码），留待后续。

// 取一个请求头到 buf。返回 false 表示没有这个头，或者装不下（装不下按"不放行"
// 处理，绝不按截断后的值去比较）。
// 参数不带 const：IDF 的 httpd_req_get_hdr_value_len/_str 收的就是非 const
// httpd_req_t*，写成 const 会在每次调用处触发 -Wdiscarded-qualifiers。
static bool gate_get_header(httpd_req_t *req, const char *name, char *buf,
                            size_t cap) {
  int len = httpd_req_get_hdr_value_len(req, name);
  if (len <= 0 || (size_t)len >= cap) {
    return false;
  }
  return httpd_req_get_hdr_value_str(req, name, buf, cap) == ESP_OK;
}

// IPv4 点分十进制字面量。手写而不用 inet_pton: 这个文件只需要判断字符串形态，
// 把 socket 头拉进来反而和现有 include 顺序打架 (AF_INET/inet_pton 不可见)。
static bool gate_is_ipv4_literal(const char *h) {
  int octets = 0;
  unsigned val = 0;
  size_t digits = 0;
  for (const char *p = h;; p++) {
    if (*p >= '0' && *p <= '9') {
      val = val * 10u + (unsigned)(*p - '0');
      if (++digits > 3 || val > 255u) {
        return false;
      }
      continue;
    }
    if (*p == '.') {
      if (digits == 0) {
        return false; // 前导/连续 '.'
      }
      octets++;
      val = 0;
      digits = 0;
      continue;
    }
    if (*p == '\0') {
      break;
    }
    return false; // 任何其它字符 (含 ':') 就不是 IPv4
  }
  return digits > 0 && octets == 3;
}

// Host 头里的值是不是"设备自己"。
static bool gate_host_is_self(const char *h) {
  if (!h || !*h) {
    return false;
  }
  if (gate_is_ipv4_literal(h)) {
    return true; // 直连设备 IP / 热点 192.168.4.1
  }
  if (strcasecmp(h, "localhost") == 0) {
    return true;
  }
  size_t n = strlen(h);
  // mDNS 名 (esp32-airplay.local)。只认这个后缀 —— 外部域名一律不算。
  return n > 6 && strcasecmp(h + (n - 6), ".local") == 0;
}

// 只保留 "://" 之后、'/' 和 ':' 之前的主机部分。原地修改。
static void gate_narrow_to_host(char *s) {
  char *p = strstr(s, "://");
  if (p) {
    memmove(s, p + 3, strlen(p + 3) + 1);
  }
  for (char *q = s; *q; q++) {
    if (*q == '/' || *q == ':' || *q == '?' || *q == '#') {
      *q = '\0';
      break;
    }
  }
}

static bool gate_origin_allowed(httpd_req_t *req) {
  char host[128];
  if (!gate_get_header(req, "Host", host, sizeof(host))) {
    return true; // 没有 Host (HTTP/1.0 老客户端): 不因此拒绝
  }
  gate_narrow_to_host(host);
  if (!gate_host_is_self(host)) {
    ESP_LOGW(TAG, "拒绝: Host=\"%s\" 不是本机 (疑似 DNS rebinding)", host);
    return false;
  }

  char origin[160];
  bool have_origin = gate_get_header(req, "Origin", origin, sizeof(origin));
  if (!have_origin) {
    // 表单/顶层跳转不带 Origin，但会带 Referer。
    have_origin = gate_get_header(req, "Referer", origin, sizeof(origin));
  }
  if (!have_origin) {
    return true; // 命令行工具之类: 没有跨源概念
  }
  gate_narrow_to_host(origin);
  if (strcasecmp(origin, host) != 0) {
    ESP_LOGW(TAG, "拒绝: 来源 %s != 本机 %s (跨源请求)", origin, host);
    return false;
  }
  return true;
}

// 每个 URI 都挂在这个函数上，真 handler 存在 user_ctx 里（注册点 50 处，守卫只
// 这一处）。
typedef esp_err_t (*uri_handler_fn)(httpd_req_t *r);

/* 配网探测路径豁免。
 *
 * ⚠️ 批次 4 的 Host 白名单把这些也一起挡了：手机连上 ESP32-AirPlay-Setup 后
 *    探测用的请求带的是 Host: captive.apple.com / connectivitycheck.gstatic.com
 *    这类**外部域名**，`gate_host_is_self()` 判"不是本机"→ 403 → 配置页不会
 *    自动弹出来（2026-09-19 启动日志里就是这条：拒绝 Host="captive.apple.com"）。
 *
 * 只放 GET，只放这五条：它们不读不写任何状态，响应是一个写死的 302 到
 * http://192.168.4.1/，rebinding 攻击者从这里最多拿到一个跳转，拿不到内容。
 * 其它路径（尤其任何 POST）照旧全部过守卫。 */
static bool gate_is_captive_probe(const httpd_req_t *req) {
  if (req->method != HTTP_GET) {
    return false;
  }
  static const char *const probes[] = {
      "/hotspot-detect.html", "/library/test/success.html", "/generate_204",
      "/connecttest.txt", "/redirect"};
  for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
    if (strcmp(req->uri, probes[i]) == 0) {
      return true;
    }
  }
  return false;
}

static esp_err_t gate_dispatch(httpd_req_t *req) {
  if (!gate_is_captive_probe(req) && !gate_origin_allowed(req)) {
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "cross-origin request blocked", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  uri_handler_fn inner = (uri_handler_fn)req->user_ctx;
  return inner ? inner(req) : ESP_ERR_NOT_FOUND;
}

// 用这个代替 httpd_register_uri_handler()。
static esp_err_t register_uri(const httpd_uri_t *u) {
  httpd_uri_t g = *u;
  g.user_ctx = (void *)u->handler; // 真 handler 挪进 user_ctx
  g.handler = gate_dispatch;
  return httpd_register_uri_handler(s_server, &g);
}

esp_err_t web_server_start(uint16_t port) {
  if (s_server) {
    ESP_LOGW(TAG, "Web server already running");
    return ESP_OK;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = port;
#ifdef CONFIG_BT_ENABLED
  config.max_open_sockets = 2;   // BT: tighter socket budget (LWIP 12)
  config.send_wait_timeout = 10; // BT/WiFi coexistence slows TCP drain
#else
  config.max_open_sockets = 3; // Limit to save lwIP socket slots for AirPlay
#endif
  config.lru_purge_enable = true; // Reclaim stale sockets when all are in use
  config.max_uri_handlers =
      30; // Room for captive portal + EQ + speedtest + brightness + channel
  config.max_uri_handlers += 5; // web radio: state / play / stop / next / prev
  // SD card: state / list / play / stop / next / prev / mount / delete / mkdir /
  // upload
  config.max_uri_handlers += 10;
  config.max_uri_handlers += 1;  // app.js（主页脚本从内联拆成单独文件）
#ifdef DAC_HAS_SUB_OFFSET
  config.max_uri_handlers += 2; // sub level get/post
#endif
#ifdef CONFIG_DAC_TAS58XX
  config.max_uri_handlers += 2; // dual DAC role get/post
  config.max_uri_handlers += 2; // bi-amp get/post
#endif
  config.max_resp_headers = 8;
  config.stack_size = 8192;

  esp_err_t err = httpd_start(&s_server, &config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start web server: %s", esp_err_to_name(err));
    return err;
  }

  // Register handlers
  httpd_uri_t root_uri = {
      .uri = "/", .method = HTTP_GET, .handler = root_handler};
  register_uri(&root_uri);

  httpd_uri_t favicon_uri = {
      .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_handler};
  register_uri(&favicon_uri);

  httpd_uri_t base_css_uri = {
      .uri = "/base.css", .method = HTTP_GET, .handler = base_css_handler};
  register_uri(&base_css_uri);

  httpd_uri_t app_js_uri = {
      .uri = "/app.js", .method = HTTP_GET, .handler = app_js_handler};
  register_uri(&app_js_uri);

  httpd_uri_t logs_uri = {
      .uri = "/logs", .method = HTTP_GET, .handler = logs_page_handler};
  register_uri(&logs_uri);

  httpd_uri_t speedtest_page_uri = {.uri = "/speedtest",
                                    .method = HTTP_GET,
                                    .handler = speedtest_page_handler};
  register_uri(&speedtest_page_uri);

  httpd_uri_t speedtest_ping_uri = {.uri = "/api/speedtest/ping",
                                    .method = HTTP_GET,
                                    .handler = speedtest_ping_handler};
  register_uri(&speedtest_ping_uri);

  httpd_uri_t speedtest_dl_uri = {.uri = "/api/speedtest/download",
                                  .method = HTTP_GET,
                                  .handler = speedtest_download_handler};
  register_uri(&speedtest_dl_uri);

  httpd_uri_t speedtest_ul_uri = {.uri = "/api/speedtest/upload",
                                  .method = HTTP_POST,
                                  .handler = speedtest_upload_handler};
  register_uri(&speedtest_ul_uri);

  httpd_uri_t wifi_scan_uri = {.uri = "/api/wifi/scan",
                               .method = HTTP_GET,
                               .handler = wifi_scan_handler};
  register_uri(&wifi_scan_uri);

  httpd_uri_t wifi_config_uri = {.uri = "/api/wifi/config",
                                 .method = HTTP_POST,
                                 .handler = wifi_config_handler};
  register_uri(&wifi_config_uri);

  httpd_uri_t device_name_uri = {.uri = "/api/device/name",
                                 .method = HTTP_POST,
                                 .handler = device_name_handler};
  register_uri(&device_name_uri);

  httpd_uri_t volume_get_uri = {.uri = "/api/volume",
                                .method = HTTP_GET,
                                .handler = volume_get_handler};
  register_uri(&volume_get_uri);

  httpd_uri_t volume_post_uri = {.uri = "/api/volume",
                                 .method = HTTP_POST,
                                 .handler = volume_post_handler};
  register_uri(&volume_post_uri);

  httpd_uri_t led_brightness_get_uri = {.uri = "/api/led/brightness",                                        .method = HTTP_GET,
                                        .handler = led_brightness_get_handler};
  register_uri(&led_brightness_get_uri);

  httpd_uri_t led_brightness_post_uri = {.uri = "/api/led/brightness",
                                         .method = HTTP_POST,
                                         .handler =
                                             led_brightness_post_handler};
  register_uri(&led_brightness_post_uri);

  httpd_uri_t channel_mode_get_uri = {.uri = "/api/audio/channel",
                                      .method = HTTP_GET,
                                      .handler = channel_mode_get_handler};
  register_uri(&channel_mode_get_uri);

  httpd_uri_t channel_mode_post_uri = {.uri = "/api/audio/channel",
                                       .method = HTTP_POST,
                                       .handler = channel_mode_post_handler};
  register_uri(&channel_mode_post_uri);

#ifdef DAC_HAS_SUB_OFFSET
  httpd_uri_t sub_offset_get_uri = {.uri = "/api/audio/sub",
                                    .method = HTTP_GET,
                                    .handler = sub_offset_get_handler};
  register_uri(&sub_offset_get_uri);

  httpd_uri_t sub_offset_post_uri = {.uri = "/api/audio/sub",
                                     .method = HTTP_POST,
                                     .handler = sub_offset_post_handler};
  register_uri(&sub_offset_post_uri);
#endif

#ifdef CONFIG_DAC_TAS58XX
  httpd_uri_t dual_mode_get_uri = {.uri = "/api/audio/dual",
                                   .method = HTTP_GET,
                                   .handler = dual_mode_get_handler};
  register_uri(&dual_mode_get_uri);

  httpd_uri_t dual_mode_post_uri = {.uri = "/api/audio/dual",
                                    .method = HTTP_POST,
                                    .handler = dual_mode_post_handler};
  register_uri(&dual_mode_post_uri);

  httpd_uri_t biamp_get_uri = {.uri = "/api/audio/biamp",
                               .method = HTTP_GET,
                               .handler = biamp_get_handler};
  register_uri(&biamp_get_uri);

  httpd_uri_t biamp_post_uri = {.uri = "/api/audio/biamp",
                                .method = HTTP_POST,
                                .handler = biamp_post_handler};
  register_uri(&biamp_post_uri);
#endif

  // No OTA endpoint: the partition table has no OTA slot, firmware is flashed
  // over USB. Re-adding the route would only offer a write that cannot land.

  httpd_uri_t system_info_uri = {.uri = "/api/system/info",
                                 .method = HTTP_GET,
                                 .handler = system_info_handler};
  register_uri(&system_info_uri);

  httpd_uri_t system_restart_uri = {.uri = "/api/system/restart",
                                    .method = HTTP_POST,
                                    .handler = system_restart_handler};
  register_uri(&system_restart_uri);

  // 固件升级（策略见 ota_update_handler：没设口令就是 403）
  httpd_uri_t ota_update_uri = {.uri = "/api/ota/update",
                                .method = HTTP_POST,
                                .handler = ota_update_handler};
  register_uri(&ota_update_uri);

  httpd_uri_t ota_password_uri = {.uri = "/api/ota/password",
                                  .method = HTTP_POST,
                                  .handler = ota_password_handler};
  register_uri(&ota_password_uri);

  // File management API
  httpd_uri_t fs_upload_uri = {.uri = "/api/fs/upload",
                               .method = HTTP_POST,
                               .handler = fs_upload_handler};
  register_uri(&fs_upload_uri);

  httpd_uri_t fs_delete_uri = {.uri = "/api/fs/delete",
                               .method = HTTP_POST,
                               .handler = fs_delete_handler};
  register_uri(&fs_delete_uri);

  httpd_uri_t fs_list_uri = {
      .uri = "/api/fs/list", .method = HTTP_GET, .handler = fs_list_handler};
  register_uri(&fs_list_uri);

  // Captive portal detection endpoints
  // Apple iOS/macOS
  httpd_uri_t apple_captive1 = {.uri = "/hotspot-detect.html",
                                .method = HTTP_GET,
                                .handler = captive_apple_handler};
  register_uri(&apple_captive1);

  httpd_uri_t apple_captive2 = {.uri = "/library/test/success.html",
                                .method = HTTP_GET,
                                .handler = captive_apple_handler};
  register_uri(&apple_captive2);

  // Android
  httpd_uri_t android_captive = {.uri = "/generate_204",
                                 .method = HTTP_GET,
                                 .handler = captive_android_handler};
  register_uri(&android_captive);

  // Windows
  httpd_uri_t windows_captive = {.uri = "/connecttest.txt",
                                 .method = HTTP_GET,
                                 .handler = captive_windows_handler};
  register_uri(&windows_captive);
  windows_captive.uri = "/redirect";
  register_uri(&windows_captive);

#ifdef CONFIG_DAC_TAS58XX
  httpd_uri_t eq_page_uri = {
      .uri = "/eq", .method = HTTP_GET, .handler = eq_page_handler};
  register_uri(&eq_page_uri);

  httpd_uri_t eq_get_uri = {
      .uri = "/api/eq", .method = HTTP_GET, .handler = eq_get_handler};
  register_uri(&eq_get_uri);

  httpd_uri_t eq_post_uri = {
      .uri = "/api/eq", .method = HTTP_POST, .handler = eq_post_handler};
  register_uri(&eq_post_uri);
#endif

  log_stream_register(s_server);

  // ---- Web 电台 (HTTP 流播放) ----
  static const httpd_uri_t radio_state_uri = {
      .uri = "/api/music", .method = HTTP_GET, .handler = radio_state_handler};
  register_uri(&radio_state_uri);

  static const httpd_uri_t radio_play_uri = {
      .uri = "/api/music/play", .method = HTTP_POST, .handler = radio_play_handler};
  register_uri(&radio_play_uri);

  static const httpd_uri_t radio_stop_uri = {
      .uri = "/api/music/stop", .method = HTTP_POST, .handler = radio_stop_handler};
  register_uri(&radio_stop_uri);

  static const httpd_uri_t radio_next_uri = {
      .uri = "/api/music/next", .method = HTTP_POST, .handler = radio_next_handler};
  register_uri(&radio_next_uri);

  static const httpd_uri_t radio_prev_uri = {
      .uri = "/api/music/prev", .method = HTTP_POST, .handler = radio_prev_handler};
  register_uri(&radio_prev_uri);

  // ---- SD 卡本地音乐 ----
  static const httpd_uri_t sd_state_uri = {
      .uri = "/api/sd/state", .method = HTTP_GET, .handler = sd_state_handler};
  register_uri(&sd_state_uri);

  static const httpd_uri_t sd_list_uri = {
      .uri = "/api/sd/list", .method = HTTP_GET, .handler = sd_list_handler};
  register_uri(&sd_list_uri);

  static const httpd_uri_t sd_play_uri = {
      .uri = "/api/sd/play", .method = HTTP_POST, .handler = sd_play_handler};
  register_uri(&sd_play_uri);

  static const httpd_uri_t sd_stop_uri = {
      .uri = "/api/sd/stop", .method = HTTP_POST, .handler = sd_stop_handler};
  register_uri(&sd_stop_uri);

  static const httpd_uri_t sd_next_uri = {
      .uri = "/api/sd/next", .method = HTTP_POST, .handler = sd_next_handler};
  register_uri(&sd_next_uri);

  static const httpd_uri_t sd_prev_uri = {
      .uri = "/api/sd/prev", .method = HTTP_POST, .handler = sd_prev_handler};
  register_uri(&sd_prev_uri);

  static const httpd_uri_t sd_mount_uri = {
      .uri = "/api/sd/mount", .method = HTTP_POST, .handler = sd_mount_handler};
  register_uri(&sd_mount_uri);

  static const httpd_uri_t sd_delete_uri = {
      .uri = "/api/sd/delete", .method = HTTP_POST, .handler = sd_delete_handler};
  register_uri(&sd_delete_uri);

  static const httpd_uri_t sd_mkdir_uri = {
      .uri = "/api/sd/mkdir", .method = HTTP_POST, .handler = sd_mkdir_handler};
  register_uri(&sd_mkdir_uri);

  static const httpd_uri_t sd_upload_uri = {
      .uri = "/api/sd/upload", .method = HTTP_POST, .handler = sd_upload_handler};
  register_uri(&sd_upload_uri);

  ESP_LOGI(TAG, "Web server started on port %d with captive portal support",
           port);
  return ESP_OK;
}

void web_server_stop(void) {
  if (s_server) {
    httpd_stop(s_server);
    s_server = NULL;
    ESP_LOGI(TAG, "Web server stopped");
  }
}
