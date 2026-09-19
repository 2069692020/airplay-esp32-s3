/**
 * 互联网校时（LWIP SNTP）。
 *
 * ⚠️ 别和 main/network/ntp_clock.c 搞混：那个模块是 **AirPlay 1 的时序同步**
 *    （量本机与发送端之间的时钟偏移，喂给 audio_timing 用），跟"现在是几点"
 *    没有任何关系。系统墙钟只有这里是来源。
 *
 * 时间戳的唯一用途是让网页能回答"上次复位是几点发生的"：
 * 启动时刻 = 当前时间 - 运行时长，未同步时相关字段直接不给，不猜。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "esp_err.h"

/** 启动 SNTP（幂等；联网之后再调）。未启用时返回 ESP_ERR_NOT_SUPPORTED。 */
esp_err_t time_sync_start(void);

/** 系统时钟是否已经同步（判据是墙钟本身，因此也覆盖别的对时来源）。 */
bool time_sync_is_synced(void);

/** 当前本地时间字符串 "%Y-%m-%d %H:%M:%S"。未同步返回 false。 */
bool time_sync_local_string(char *out, size_t n);

/** 本次启动（= 上一次复位）的本地时刻。未同步返回 false。 */
bool time_sync_boot_string(char *out, size_t n);
