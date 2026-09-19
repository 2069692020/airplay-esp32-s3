#include "aht20.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

#if defined(CONFIG_AHT20_ENABLED)
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "spiram_task.h"

static const char *TAG = "aht20";

/* 命令序列（AHT20 datasheet） */
#define AHT20_CMD_TRIG 0xAC   /* AC 33 00：触发一次测量 */
#define AHT20_CMD_CAL  0xBE   /* BE 08 00：上电校准 */
#define AHT20_FLAG_BUSY 0x80  /* status bit7 = 正在测量 */
#define AHT20_FLAG_CAL  0x08  /* status bit3 = 校准已使能 */

#define AHT20_WAIT_MS 80      /* 典型测量时间 */
#define AHT20_RETRY   6       /* 最多再等 6×40 ms ≈ 240 ms */
#define AHT20_TIMEOUT 500     /* 单次 I2C 事务超时 */

/* 20 位原始量的分母 */
#define AHT20_FULL_SCALE 1048576.0f

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;

/* 缓存的读数。写者只有采样任务，读者是 httpd 任务：两个 float 可能读到
   "新温度 + 旧湿度"的混搭，对显示无害，但别拿它做闭环控制。 */
static float s_temp_c;
static float s_rh_pct;
static int64_t s_ok_us;
static bool s_valid;
static bool s_logged;   /* 首帧日志是否已打 */
static unsigned s_fails;

static esp_err_t aht20_cmd3(uint8_t a, uint8_t b, uint8_t c) {
  uint8_t buf[3] = {a, b, c};
  return i2c_master_transmit(s_dev, buf, sizeof(buf), AHT20_TIMEOUT);
}

/* 单次测量：拿一轮 status + 5 个数据字节并换算。
 *
 * ⚠️ 帧长是 6 不是 7 —— 这里踩过一次：按 7 字节读时温度的最低字节取到了
 *   帧外的浮空值（所以每轮三次互相差几十度），湿度又少右移 4 位、整体放大
 *   16 倍。两个错都是"事务成功、数字胡说"，光看 I2C 返回值发现不了。
 * 布局（datasheet / Adafruit AHTX0 同此）：
 *   [0] status   bit7=busy, bit3=校准使能
 *   [1] RH[19:12]  [2] RH[11:4]  [3] 高半字节=RH[3:0]、低半字节=T[19:16]
 *   [4] T[15:8]    [5] T[7:0]
 */
/* 单次测量的返回值：
 *   1 = 拿到并通过全部判据；0 = I2C 事务本身失败（没应答 / 线不通）；
 *  -1 = 事务成功但数据被判据拒绝（校准位没起来 / 超出量程）—— 这两种情况的
 *      排查方向完全不同，绝不能合并成一句"读不到"。 */
static int aht20_read_once(uint8_t *status, float *tc_out, float *rh_out) {
  if (aht20_cmd3(AHT20_CMD_TRIG, 0x33, 0x00) != ESP_OK) {
    return 0;
  }
  vTaskDelay(pdMS_TO_TICKS(AHT20_WAIT_MS));

  uint8_t r[6] = {0};
  for (int i = 0; i <= AHT20_RETRY; i++) {
    if (i2c_master_receive(s_dev, r, sizeof(r), AHT20_TIMEOUT) != ESP_OK) {
      return 0;
    }
    if (!(r[0] & AHT20_FLAG_BUSY)) {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(40));
  }
  if (r[0] & AHT20_FLAG_BUSY) {
    return -1;  /* 一直在忙 = 指令其实没被理解 */
  }
  /* 校准使能位没置起来就不是一个正常工作的 AHT20（真器件上电校准后必为 1）*/
  if (!(r[0] & AHT20_FLAG_CAL)) {
    if (status) {
      *status = r[0];
    }
    return -1;
  }

  /* 20 位量的拼装：湿度跨 r[1..3]（r[3] 取高半字节），
     温度跨 r[3] 低半字节 + r[4] + r[5]。 */
  uint32_t raw_h = (((uint32_t)r[1]) << 12) | (((uint32_t)r[2]) << 4) |
                   (((uint32_t)r[3]) >> 4);
  uint32_t raw_t = ((((uint32_t)r[3]) & 0x0F) << 16) |
                   (((uint32_t)r[4]) << 8) | ((uint32_t)r[5]);
  float rh = (float)raw_h / AHT20_FULL_SCALE * 100.0f;
  float tc = (float)raw_t / AHT20_FULL_SCALE * 200.0f - 50.0f;

  /* 量程检查当地板用（帧读错时它能挡住一部分，但主要靠调用方的一轮三次
     一致性判别 —— 位序错了照样能算出一个"看着合理"的假数）。 */
  if (tc < -40.0f || tc > 85.0f || rh < 0.0f || rh > 100.5f) {
    if (status) {
      *status = r[0];
    }
    return -1;
  }
  if (status) {
    *status = r[0];
  }
  *tc_out = tc;
  *rh_out = rh;
  return 1;
}

/* 一轮 = 连采三次，要求互相吻合才发布。
 * 返回 2 = 通过并发布；1 = 三次都拿到了但互相不吻合（信号质量 / 位序）；
 *      0 = 三次里有事务失败（多半是没应答：没插、没供电、接反）；
 *     -1 = 事务全部成功，但数据被校准位或量程判据拒绝（器件在，状态不对）。
 * 这四种排查方向不同，所以必须分开报，也只有 0 才值得去扫总线。 */
#define AHT20_SPREAD_T 1.0f  /* °C */
#define AHT20_SPREAD_H 3.0f  /* %RH */

static int aht20_round(uint8_t *st_out, float *t_lo, float *t_hi) {
  float ts[3], hs[3];
  int ok = 0, txn_fail = 0;
  uint8_t last_st = 0;
  for (int i = 0; i < 3; i++) {
    uint8_t st = 0;
    float tc = 0, rh = 0;
    int rc = aht20_read_once(&st, &tc, &rh);
    if (st) {
      last_st = st;
    }
    if (rc == 1) {
      ts[ok] = tc;
      hs[ok] = rh;
      ok++;
    } else if (rc == 0) {
      txn_fail++;
    }
  }
  if (st_out) {
    *st_out = last_st;
  }
  if (ok < 3) {
    return txn_fail ? 0 : -1;
  }
  float tmin = ts[0], tmax = ts[0], hmin = hs[0], hmax = hs[0];
  for (int i = 1; i < 3; i++) {
    tmin = ts[i] < tmin ? ts[i] : tmin;
    tmax = ts[i] > tmax ? ts[i] : tmax;
    hmin = hs[i] < hmin ? hs[i] : hmin;
    hmax = hs[i] > hmax ? hs[i] : hmax;
  }
  if (tmax - tmin > AHT20_SPREAD_T || hmax - hmin > AHT20_SPREAD_H) {
    if (t_lo) {
      *t_lo = tmin;
      *t_hi = tmax;
    }
    return 1;
  }
  /* 取中间值，进一步压掉单点毛刺 */
  s_temp_c = ts[0] + ts[1] + ts[2] - tmin - tmax;
  s_rh_pct = hs[0] + hs[1] + hs[2] - hmin - hmax;
  s_ok_us = esp_timer_get_time();
  s_valid = true;
  return 2;
}

/* 扫一遍总线，把真正 ACK 的地址打出来。
 * "电平正常但 0x38 无应答"这句话分不清三种情况：地址不对 / SDA-SCL 接反 /
 * 根本没插。扫一次就能区分 —— 有别的地址 ACK 就是地址不对，一个都没有
 * 就是接线或供电问题。放在任务里做，最坏情况的超时不拖住开机。 */
static void aht20_scan_once(void) {
  char found[64] = {0};
  size_t off = 0;
  int n = 0;
  for (uint16_t a = 0x08; a <= 0x77; a++) {
    if (i2c_master_probe(s_bus, a, 20) != ESP_OK) {
      continue;
    }
    /* 先量剩余空间再写：snprintf 的返回值是"本应写入的长度"，一旦 off 超过
       缓冲，sizeof(found) - off 会以 size_t 下溢成一个巨大值。 */
    if (off + 8 >= sizeof(found)) {
      off += (size_t)snprintf(found + off, sizeof(found) - off, "+…");
      break;
    }
    off += (size_t)snprintf(found + off, sizeof(found) - off, "%s0x%02X",
                            n ? "," : "", a);
    n++;
  }
  if (n > 0) {
    ESP_LOGW(TAG, "总线上有 %d 个从机应答: %s —— 但 0x%02X 没有，"
                  "改 CONFIG_AHT20_I2C_ADDR 试试",
             n, found, CONFIG_AHT20_I2C_ADDR);
  } else {
    ESP_LOGW(TAG, "整条总线 0x08~0x77 无一应答 —— 传感器没插/没供电，"
                  "或 SDA=%d 与 SCL=%d 接反",
             CONFIG_AHT20_I2C_SDA, CONFIG_AHT20_I2C_SCL);
  }
}

static void aht20_task(void *arg) {
  (void)arg;
  const int def_ms = 5000;
  const int period_ms =
      CONFIG_AHT20_PERIOD_MS > 0 ? CONFIG_AHT20_PERIOD_MS : def_ms;

  /* 上电后传感器自己也要时间（datasheet: 首次校准可达数十 ms）。 */
  vTaskDelay(pdMS_TO_TICKS(200));

  for (;;) {
    uint8_t st = 0;
    float lo = 0, hi = 0;
    int rc = aht20_round(&st, &lo, &hi);
    if (rc == 2) {
      if (!s_logged) {
        s_logged = true;
        ESP_LOGI(TAG,
                 "AHT20 就绪 (sda=%d scl=%d addr=0x%02X): %.1f °C / %.1f %%RH",
                 CONFIG_AHT20_I2C_SDA, CONFIG_AHT20_I2C_SCL,
                 CONFIG_AHT20_I2C_ADDR, (double)s_temp_c, (double)s_rh_pct);
      } else if (s_fails) {
        ESP_LOGI(TAG, "恢复读数: %.1f °C / %.1f %%RH（此前连续失败 %u 次）",
                 (double)s_temp_c, (double)s_rh_pct, s_fails);
      }
      s_fails = 0;
      vTaskDelay(pdMS_TO_TICKS(period_ms));
      continue;
    }

    s_fails++;
    if (rc == 1 && (s_fails == 1 || s_fails % 20 == 0)) {
      /* 三次事务都成功，结果却互不吻合 —— "I2C 报成功"从来不等于"数字可信"。
         本轮的真实成因就是把 6 字节帧按 7 字节解（温度取到了帧外的浮空字节）。
         留着这条判别，是因为它对上拉缺失、线太长、从机没就绪同样敏感。 */
      ESP_LOGW(TAG,
               "AHT20 数据不稳定（第 %u 轮）：同轮三次温度 %.1f ~ %.1f °C，"
               "status=0x%02X —— 事务成功但结果互不吻合。按成本排序查："
               "帧长/位序、上拉是否缺失、线是否太长",
               s_fails, (double)lo, (double)hi, st);
      vTaskDelay(pdMS_TO_TICKS(period_ms));
      continue;
    }
    if (rc == -1 && (s_fails == 1 || s_fails % 20 == 0)) {
      /* 有应答、但状态字或量程被判据挡下 —— 器件在总线上，只是状态不对。
         这种时候扫总线是白费（一定能扫到它），所以走另一条诊断话术。 */
      ESP_LOGW(TAG,
               "AHT20 有应答但数据被判据拒绝（第 %u 轮）：status=0x%02X ——"
               " 校准位(bit3)未置起或读数超量程；可试断电重上电，"
               "或核对 CONFIG_AHT20_I2C_ADDR 是否真是 AHT20（非 AHT10/SHT）",
               s_fails, st);
      (void)aht20_cmd3(AHT20_CMD_CAL, 0x08, 0x00);
      vTaskDelay(pdMS_TO_TICKS(period_ms));
      continue;
    }
    if (s_fails == 1 || s_fails % 20 == 0) {
      /* 完全拿不到事务：这时总线扫描才有意义（区分地址不对 / 没插 / 接反） */
      int sda = gpio_get_level(CONFIG_AHT20_I2C_SDA);
      int scl = gpio_get_level(CONFIG_AHT20_I2C_SCL);
      ESP_LOGW(TAG,
               "读不到 AHT20（第 %u 次）：sda=%d 电平=%d scl=%d 电平=%d — "
               "空闲应为高，低说明没供电/接反/上拉太弱",
               s_fails, CONFIG_AHT20_I2C_SDA, sda, CONFIG_AHT20_I2C_SCL, scl);
      aht20_scan_once();
      (void)aht20_cmd3(AHT20_CMD_CAL, 0x08, 0x00);
    }
    /* 失败时拉长间隔：没接传感器的话没必要每 5 秒去占总线并刷日志 */
    vTaskDelay(pdMS_TO_TICKS(s_fails > 3 ? 30000 : period_ms));
  }
}

/* ---- 引脚冲突守卫 ----------------------------------------------------
 * ⚠️ 这一条是本轮实测踩出来的：SDA 配成 GPIO45，而 GPIO45 正是
 *   CONFIG_SD_SCLK_GPIO。i2c_new_master_bus() 会把该脚**重新复用**给 I2C，
 *   于是 SD 卡的时钟被抢走 —— 卡还是"已挂载"（FATFS 状态是缓存的），
 *   但 readdir 再也读不出东西，网页上表现为"目录里没有文件"。
 *   症状离根因很远，所以不能靠人记住"别用 45"：撞了就开不起来，并说出撞了谁。
 */
static const char *aht20_pin_owner(int pin) {
  if (pin < 0) {
    return NULL;
  }
#if defined(CONFIG_SD_CARD_ENABLED)
  if (pin == CONFIG_SD_CS_GPIO) return "SD 卡 CS";
  if (pin == CONFIG_SD_SCLK_GPIO) return "SD 卡 SCLK";
  if (pin == CONFIG_SD_MISO_GPIO) return "SD 卡 MISO";
  if (pin == CONFIG_SD_MOSI_GPIO) return "SD 卡 MOSI";
#endif
  if (pin == CONFIG_I2S_BCK_IO) return "I2S BCK";
  if (pin == CONFIG_I2S_WS_IO) return "I2S WS";
  if (pin == CONFIG_I2S_DO_IO) return "I2S DOUT";
#if defined(CONFIG_I2S_SCK_IO)
  if (pin == CONFIG_I2S_SCK_IO) return "I2S MCLK";
#endif
#if defined(CONFIG_DISPLAY_ENABLED) && defined(CONFIG_DISPLAY_BUS_I2C)
  if (pin == CONFIG_DISPLAY_I2C_SDA) return "显示 I2C SDA";
  if (pin == CONFIG_DISPLAY_I2C_SCL) return "显示 I2C SCL";
#endif
#if defined(CONFIG_LED_STATUS_GPIO)
  if (pin == CONFIG_LED_STATUS_GPIO) return "状态 LED";
  if (pin == CONFIG_LED_ERROR_GPIO) return "错误 LED";
  if (pin == CONFIG_LED_RGB_GPIO) return "RGB LED";
#endif
  if (pin == CONFIG_BTN_PLAY_PAUSE_GPIO) return "按键 播放/暂停";
  if (pin == CONFIG_BTN_VOLUME_UP_GPIO) return "按键 音量+";
  if (pin == CONFIG_BTN_VOLUME_DOWN_GPIO) return "按键 音量-";
  if (pin == CONFIG_BTN_NEXT_GPIO) return "按键 下一曲";
  if (pin == CONFIG_BTN_PREV_GPIO) return "按键 上一曲";
  return NULL;
}
#endif /* CONFIG_AHT20_ENABLED */

esp_err_t aht20_init(void) {
#if defined(CONFIG_AHT20_ENABLED)
  const char *own_sda = aht20_pin_owner(CONFIG_AHT20_I2C_SDA);
  const char *own_scl = aht20_pin_owner(CONFIG_AHT20_I2C_SCL);
  if (own_sda || own_scl) {
    ESP_LOGE(TAG,
             "引脚冲突: SDA=%d(%s) SCL=%d(%s) — AHT20 不启动，否则会把别人的"
             "脚抢走。换接线或改 CONFIG_AHT20_I2C_*。",
             CONFIG_AHT20_I2C_SDA, own_sda ? own_sda : "-",
             CONFIG_AHT20_I2C_SCL, own_scl ? own_scl : "-");
    return ESP_ERR_INVALID_ARG;
  }
  if (CONFIG_AHT20_I2C_SDA == CONFIG_AHT20_I2C_SCL) {
    ESP_LOGE(TAG, "SDA 与 SCL 是同一个脚 (%d)，AHT20 不启动",
             CONFIG_AHT20_I2C_SDA);
    return ESP_ERR_INVALID_ARG;
  }

  i2c_master_bus_config_t bus_cfg = {
      /* -1 = 让驱动挑一个空闲控制器。显示面板那条总线（esp32s3-generic 的
         board.c）同样用 -1 且先建，所以这里会拿到第二个控制器；S3 有 2 个。 */
      .i2c_port = -1,
      .sda_io_num = CONFIG_AHT20_I2C_SDA,
      .scl_io_num = CONFIG_AHT20_I2C_SCL,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "I2C 总线建立失败 (sda=%d scl=%d): %s", CONFIG_AHT20_I2C_SDA,
             CONFIG_AHT20_I2C_SCL, esp_err_to_name(err));
    return err;
  }

  i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = CONFIG_AHT20_I2C_ADDR,
      /* 速率走 CONFIG_AHT20_I2C_SPEED_KHZ（默认 100 kHz）。只有线太长或确实
         没有外部 4.7k~10k 上拉时才需要降到 20 kHz —— 本轮一开始读数乱并不是
         信号问题，是帧长解错了（见 aht20_read_once 的注释）。 */
      .scl_speed_hz = CONFIG_AHT20_I2C_SPEED_KHZ * 1000,
  };
  err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "加 AHT20 设备失败: %s", esp_err_to_name(err));
    i2c_del_master_bus(s_bus);
    s_bus = NULL;
    return err;
  }

  if (aht20_cmd3(AHT20_CMD_CAL, 0x08, 0x00) != ESP_OK) {
    ESP_LOGW(TAG, "上电校准命令没被应答 —— 传感器可能不在或线序不对，"
                  "仍会按周期重试");
  }

  BaseType_t ok = task_create_spiram(aht20_task, "aht20", 3072, NULL, 2, NULL,
                                     NULL);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "采样任务创建失败（内存不足）");
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(TAG, "AHT20 已启用: sda=%d scl=%d addr=0x%02X %dkHz 周期=%d ms",
           CONFIG_AHT20_I2C_SDA, CONFIG_AHT20_I2C_SCL, CONFIG_AHT20_I2C_ADDR,
           CONFIG_AHT20_I2C_SPEED_KHZ, CONFIG_AHT20_PERIOD_MS);
  return ESP_OK;
#else
  return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool aht20_get(float *temp_c, float *rh_pct, int *age_s) {
#if defined(CONFIG_AHT20_ENABLED)
  if (!s_valid) {
    return false;
  }
  int age = (int)((esp_timer_get_time() - s_ok_us) / 1000000);
  /* 超过 6 个周期没有新读数就当它不在了，别把旧值当现值显示 */
  if (age > 6 * (CONFIG_AHT20_PERIOD_MS > 0 ? CONFIG_AHT20_PERIOD_MS : 5000) /
          1000) {
    return false;
  }
  if (temp_c) {
    *temp_c = s_temp_c;
  }
  if (rh_pct) {
    *rh_pct = s_rh_pct;
  }
  if (age_s) {
    *age_s = age;
  }
  return true;
#else
  (void)temp_c;
  (void)rh_pct;
  (void)age_s;
  return false;
#endif
}
