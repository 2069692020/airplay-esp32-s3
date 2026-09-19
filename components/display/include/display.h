#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"

/**
 * OLED display module - Shows track metadata, playback position &
 * progress bar. Registers as an RTSP event observer to receive metadata
 * updates automatically.
 *
 * When CONFIG_DISPLAY_ENABLED is not set, display_init() is an inline no-op
 * and no display code is compiled or linked.
 */

/**
 * 正在播放的对外读数。面板是这套固件里**唯一**落地 AirPlay 曲目的地方
 * （`s_display`），网页要显示同一条信息就只能从这里取，别再开第二份缓存。
 *
 * ⚠️ `state == DISPLAY_NOW_PLAYING_STANDBY` 是唯一诚实的"没有活的 AirPlay
 *    会话"判据：v2 断连时会立刻清字符串并回到 STANDBY。
 *    不要用 `audio_receiver_is_playing()` —— `audio_timing_init()` 把它初始化成
 *    `true`，开机没有任何会话时它也报"在放"。
 */
#define DISPLAY_NOW_PLAYING_MAX 64 /* 必须 >= METADATA_STRING_MAX (rtsp_events.h) */

typedef enum {
  DISPLAY_NOW_PLAYING_STANDBY = 0,  /* 无会话（字符串已清空） */
  DISPLAY_NOW_PLAYING_CONNECTED,    /* 会话在，但没有流速率 */
  DISPLAY_NOW_PLAYING_PLAYING,
  DISPLAY_NOW_PLAYING_PAUSED,
} display_playback_state_t;

typedef struct {
  char title[DISPLAY_NOW_PLAYING_MAX];
  char artist[DISPLAY_NOW_PLAYING_MAX];
  char album[DISPLAY_NOW_PLAYING_MAX];
  uint32_t duration_secs;
  uint32_t position_secs; /* 取数时刻的估值，与面板进度条同源 */
  display_playback_state_t state;
} display_now_playing_t;

#ifdef CONFIG_DISPLAY_ENABLED

/**
 * Initialize the OLED display and register for RTSP events.
 *
 * @param bus  Pre-initialised bus handle to share with the board:
 *             - I2C mode: pass an i2c_master_bus_handle_t
 *             - SPI mode: pass (void*)(intptr_t)spi_host_device_t
 *             Pass NULL to let the display component initialise its own bus
 *             (uses the GPIO pins from Kconfig).
 */
void display_init(void *bus);

/**
 * 取一份正在播放的快照（整体拷贝，内部走面板那把自旋锁）。
 *
 * 只在临界区里做 memcpy，不打印不分配 —— 可以在 httpd 任务里调。
 * 返回 true 表示读到了面板的状态；false 只会在显示关闭时出现（那时
 * 本函数是下面那个 inline 版本）。
 */
bool display_get_now_playing(display_now_playing_t *out);

#else

static inline void display_init(void *bus) {
  (void)bus;
}

/* 显示没编进来 → 曲目信息根本没有落地过，调用方按"读不到"处理。 */
static inline bool display_get_now_playing(display_now_playing_t *out) {
  (void)out;
  return false;
}

#endif
