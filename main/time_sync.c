#include "time_sync.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <string.h>

#if defined(CONFIG_TIME_SYNC_ENABLED)
#include "lwip/apps/sntp.h"
#include "spiram_task.h"
#endif

static const char *TAG = "time_sync";

/* 上电后系统墙钟从 1970 起算。早于这个值一律当"没同步"，比信一个标志位可靠：
   它同时覆盖 SNTP 之外的对时来源（例如以后接 AirPlay 的时间）。 */
#define TIME_SYNC_EPOCH ((time_t)1700000000) /* 2023-11-14 */

static time_t time_sync_now_utc(void) {
  time_t t = time(NULL);
  return (t >= TIME_SYNC_EPOCH) ? t : (time_t)0;
}

bool time_sync_is_synced(void) { return time_sync_now_utc() != 0; }

#if defined(CONFIG_TIME_SYNC_ENABLED)
static bool s_started;

static void time_sync_task(void *arg) {
  (void)arg;
  /* 唯一职责是在日志里留下一条"对上了"的证据。判据用墙钟本身而不是某个
     内部标志，这样别的对时来源也算数。 */
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    char buf[32];
    if (time_sync_local_string(buf, sizeof(buf))) {
      ESP_LOGI(TAG, "NTP 同步完成: %s (tz=%s)", buf, CONFIG_TIME_SYNC_TZ);
      break;
    }
  }
  /* 打完就退出：留着它每 5 秒醒一次没有任何后续用途。
     xTaskCreate 建的任务自删时，栈由内核回收。 */
  vTaskDelete(NULL);
}

esp_err_t time_sync_start(void) {
  if (s_started) {
    return ESP_OK; /* 网络来回切换时会被多次叫到 */
  }
  s_started = true;

  /* POSIX 时区写法：名字在前、偏移在后且符号相反 —— CST-8 表示 UTC+8。 */
  setenv("TZ", CONFIG_TIME_SYNC_TZ, 1);
  tzset();

  /* 拷一份到静态缓冲：sntp_setservername() 在不同 LWIP 版本里 const 性不一致，
     直接传 Kconfig 字符串字面量偶尔会以 -Wdiscarded-qualifiers 报错。 */
  static char server[64];
  strncpy(server, CONFIG_TIME_SYNC_SERVER, sizeof(server) - 1);
  server[sizeof(server) - 1] = '\0';

  sntp_setoperatingmode(SNTP_OPMODE_POLL);
  sntp_setservername(0, server);
  sntp_init();

  ESP_LOGI(TAG, "SNTP 已启动: server=%s tz=%s（重同步间隔由 LWIP 决定）",
           server, CONFIG_TIME_SYNC_TZ);
  if (task_create_spiram(time_sync_task, "time_mon", 3072, NULL, 2, NULL,
                         NULL) != pdPASS) {
    ESP_LOGW(TAG, "同步状态监视任务创建失败（不影响对时本身）");
  }
  return ESP_OK;
}
#else  /* !CONFIG_TIME_SYNC_ENABLED */
esp_err_t time_sync_start(void) { return ESP_ERR_NOT_SUPPORTED; }
#endif /* CONFIG_TIME_SYNC_ENABLED */

bool time_sync_local_string(char *out, size_t n) {
  time_t t = time_sync_now_utc();
  if (!t || !out || n == 0) {
    return false;
  }
  struct tm lt;
  if (localtime_r(&t, &lt) == NULL) {
    return false;
  }
  return strftime(out, n, "%Y-%m-%d %H:%M:%S", &lt) > 0;
}

bool time_sync_boot_string(char *out, size_t n) {
  time_t t = time_sync_now_utc();
  if (!t || !out || n == 0) {
    return false;
  }
  /* esp_timer 从 app 起点开始计，所以这里比真实复位时刻晚一个 bootloader 的
     时间（约半秒）—— 对"几点复的位"够用，别说它是精确值。 */
  int64_t up = esp_timer_get_time() / 1000000;
  time_t boot = (time_t)((int64_t)t - up);
  struct tm lt;
  if (localtime_r(&boot, &lt) == NULL) {
    return false;
  }
  return strftime(out, n, "%Y-%m-%d %H:%M:%S", &lt) > 0;
}
