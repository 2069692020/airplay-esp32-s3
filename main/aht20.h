/**
 * AHT20 温湿度传感器（I2C，默认地址 0x38）。
 *
 * 采样跑在自己的低优先级周期任务里，读数缓存在内存中；web_server 只读缓存。
 * 这样切是因为一次 AHT20 测量要等 ~80 ms（最长 500 ms），在 httpd handler 里
 * 直接读会把整个 HTTP 服务连同一台设备上的音频线程一起拖住。
 *
 * ⚠️ 传感器不在（没供电 / 线接反）时 init 仍返回成功：总线建起来了就算启动
 * 成功，读不到数只是 aht20_get() 返回 false。这样热插拔可用，也不会因为一个
 * 外设把整机拖起不来。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/**
 * 建立 I2C 总线并启动采样任务。
 * @return ESP_OK 总线就绪；ESP_ERR_NOT_SUPPORTED 未启用；其它 = 总线建立失败
 */
esp_err_t aht20_init(void);

/**
 * 取最近一次有效读数。
 * @param temp_c  摄氏度，可为 NULL
 * @param rh_pct  相对湿度 %RH，可为 NULL
 * @param age_s   该读数距今多少秒，可为 NULL
 * @return false = 还没有有效读数（或已过有效期）
 */
bool aht20_get(float *temp_c, float *rh_pct, int *age_s);
