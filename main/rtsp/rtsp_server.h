#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * Start the AirPlay RTSP server on port 7000
 * Handles initial connection requests from iOS devices
 */
esp_err_t rtsp_server_start(void);

/**
 * Stop the RTSP server
 */
void rtsp_server_stop(void);

/**
 * Set volume from AirPlay (in dB, range -144 to 0)
 * @param volume_db Volume in dB (0 = max, -144 = mute)
 */
void airplay_set_volume(float volume_db);

/**
 * Set volume from the web UI (Q15 scale factor).
 *
 * 与 airplay_set_volume 共用同一套曲线和同一个 Q15 缓存, 所以网页和 AirPlay
 * 调的是**同一个音量** —— 不是各管各的。没有活跃 AirPlay 连接时只更新
 * DAC + NVS; 有连接时同步给它。下次 AirPlay 连接建立时会补同步 (见
 * airplay_take_volume_pending_sync)。
 *
 * @param q15 Q15 fixed-point multiplier (0 = mute, 32768 = unity)
 */
void airplay_set_volume_q15(int32_t q15);

/** 网页设过音量后, 取一次"是否需要同步给 AirPlay"并清除标志。 */
bool airplay_take_volume_pending_sync(void);

/**
 * 应用音量 (dB): 换算 + 更新 Q15 缓存 + 落 settings (顺带推给 DAC)。
 *
 * 全项目写音量的**唯一**入口。它刻意不碰 rtsp_conn_t —— 音量是设备级状态
 * (一个 DAC、一个 NVS 键), 而 conn 是会被单条连接释放掉的对象。
 * AirPlay 自己报来的音量 (SET_PARAMETER) 也走这里。
 */
void airplay_apply_volume_db(float volume_db);

/** 取上一次网页设置的音量 (dB), 用于同步给新建立的 AirPlay 连接。 */
float airplay_get_pending_volume_db(void);

/** 当前音量 (dB)。取代 rtsp_conn_t 上的 volume_db 字段。 */
float airplay_get_volume_db(void);

/** dB → Q15 (曲线与 airplay_apply_volume_db 一致)。 */
int32_t airplay_volume_db_to_q15(float volume_db);

/** Q15 → dB (上面那个的逆运算)。 */
float airplay_q15_to_volume_db(int32_t q15);

/**
 * Get current volume as Q15 scale factor for audio processing
 * @return Q15 fixed-point multiplier (0 = mute, 32768 = unity)
 */
int32_t airplay_get_volume_q15(void);

/**
 * Request resume during the AirPlay v1 grace period.
 * Called from the play/pause button when the source is still AirPlay
 * but the RTSP connection has been torn down (paused). Sends a DACP
 * playpause to the phone so it reconnects.
 */
void rtsp_server_request_resume(void);
