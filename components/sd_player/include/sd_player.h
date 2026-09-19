#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * SD 卡本地音乐播放器
 *
 * 数据流:
 *   FATFS 文件 ──reader task──> PSRAM 环形缓冲 ──play task──> 解码 ──> I2S
 *
 * 架构上是 components/web_radio 的姊妹组件, 刻意**不共用代码** (用户选择的
 * "独立组件" 方案), 所以 web_radio.c 里那几处踩过坑的修复在这里是**重新实现
 * 了一遍**。改动任意一边时请对照另一边 —— 注释里都标了出处。
 *
 * 与 AirPlay / 网络电台的关系 (和 web_radio 完全一致):
 *   · 播放时接管 I2S (调 output_stop 让 AirPlay 的 playback_task 让出 DMA)
 *   · AirPlay 会话一开始就自动停播并**完整交还** I2S
 *   · 和网络电台是"谁后点谁抢", 互斥由 main/network/web_server.c 编排
 *
 * ⚠️ 依赖方向: 音频输出代码在 `main` 组件里, 而 `main` 依赖本组件。
 *    所以本组件**不能** include audio_output.h (会成循环依赖)。
 *    改成由 main 在启动时把函数指针注册进来 (见 sd_player_set_output_hooks)。
 */

/** 播放状态, 供网页轮询 */
typedef enum {
  SD_PLAYER_IDLE = 0, // 未播放
  SD_PLAYER_PLAYING,  // 正在解码播放
  SD_PLAYER_ERROR,    // 出错 (见 sd_player_get_error)
} sd_player_state_t;

/**
 * 当前曲目信息, 供网页显示 "正在播放"。
 *
 * 标题的回退链 (永不为空):
 *   文件内嵌标签 > 去掉扩展名的文件名
 *
 * 作者 / 专辑**只有文件里真有标签**才有值; 没有时是空串, 网页整行不显示。
 * 标签只认 UTF-8 (ID3v2.4 与 FLAC Vorbis comment 都是 UTF-8);
 * UTF-16 编码的 ID3v2.3 会被跳过, 回退到文件名。
 */
typedef struct {
  int index;         // 当前第几曲 (1-based); 0 = 没有播放列表
  int count;         // 播放列表总曲数; 0 = 单曲
  bool truncated;    // 播放列表是否被上限截断
  char title[128];   // 显示用标题 (已走完上面的回退链)
  char artist[64];   // 可能为空串
  char album[64];    // 可能为空串
  char path[256];    // 当前曲的卡内相对路径 (UTF-8)
  uint32_t elapsed_sec; // 本曲已播秒数 (按写出的 PCM 算)
  uint32_t sample_rate; // 解码器报告的采样率, 0 = 还不知道
} sd_player_track_info_t;

/**
 * 音频输出钩子。由 main.c 注册成 audio_output_* 的包装。
 * 用函数指针而不是直接调用, 是为了避免组件循环依赖 (见文件头说明)。
 *
 * 成员与 web_radio_output_hooks_t 一致, 只是少了 abort_http —— 本地文件没有
 * 可中止的网络请求 (那是为 HTTP 的阻塞读专门加的)。
 */
typedef struct {
  /** 停掉 AirPlay 的 playback task, 让出 I2S。对应 audio_output_stop()。
   *  必须在 output_write 之前调用 —— playback_task 若在运行, 它会从
   *  audio_receiver_read() 读不到数据就灌静音, 和我们的 PCM 抢同一个 DMA。 */
  void (*output_stop)(void);
  /** 往 I2S 写 PCM。对应 audio_output_write(data, bytes, wait_ticks)。 */
  int (*output_write)(const void *data, size_t bytes, uint32_t wait_ticks);
  /** 把 I2S 重新配置成指定采样率。对应 audio_output_set_sample_rate(rate)。
   *  ⚠️ 必须在开始 output_write 之前调用。 */
  void (*output_set_rate)(uint32_t rate);
  /** 查询 AirPlay 的 playback_task 是否正在运行。
   *  ⚠️ 只有它没在跑的时候才能安全地改 I2S 采样率 —— 见 sd_player.c 里的
   *  守卫说明 (与 web_radio.c:1582-1596 同一处坑)。 */
  bool (*output_is_active)(void);
  /** 设置源采样率 (影响重采样判断)。对应 audio_output_set_source_rate(rate) */
  void (*output_set_source_rate)(int rate);
  /** 取当前音量 Q15 (32768 = 0dB)。对应 airplay_get_volume_q15() */
  int32_t (*get_volume_q15)(void);
  /** 把 I2S 采样率**还原成设备基准率**, 并重新拉起 AirPlay 的 playback_task。
   *  对应 main.c 的 radio_hook_resume_airplay()。
   *
   *  ⚠️⚠️ 不能省。播放期间做了两件有副作用的事: 按文件原生采样率改过 I2S,
   *  并停掉了 AirPlay 的 playback_task。不还原的话, 播完 SD 歌再投 AirPlay
   *  会"歌在播放但完全没声音", 且只有复位设备才能恢复。
   *  (这是 web_radio 在 2026-09-17 实测到的 bug, 见 web_radio.h 同名成员的说明。) */
  void (*resume_airplay)(void);
} sd_player_output_hooks_t;

/**
 * 初始化播放器。只建缓冲和状态, **不会**主动播放。
 * 必须在 sd_player_set_output_hooks 之后才能播放, 否则 sd_player_play 直接
 * 返回错误。SD 卡本身由 components/sd_card 负责挂载。
 */
esp_err_t sd_player_init(void);

/** 注册音频输出钩子。由 main.c 负责。传 NULL 会解除注册。 */
void sd_player_set_output_hooks(const sd_player_output_hooks_t *hooks);

/**
 * 播放一个路径。path 是**卡内相对路径**(UTF-8), 两种形态:
 *
 *   · 指向目录 (如 "/Music/周杰伦") → 枚举该目录下所有音频文件, 从第 1 首开始
 *   · 指向音频文件 (如 "/Music/七里香.mp3") → 播这一首
 *
 * ⚠️ 单曲模式也会把**所在目录**建成播放列表, 并把 index 定位到该曲 ——
 *    这样点单曲时「上一条 / 下一条」也是通的, 符合用户预期, 而不是退化成
 *    单曲循环。文件名不在列表里 (比如刚被删掉) 时退化成只有这一首。
 *
 * 会先停掉当前播放, 再接管 I2S。
 *
 * @return ESP_OK 已接受 (异步开始播放, 用 sd_player_get_state 查进度)
 */
esp_err_t sd_player_play(const char *path);

/**
 * 切上一曲 / 下一曲。
 *
 * ⚠️ 与 sd_player_play 的区别: 本函数**不会**释放 I2S、不会重建任务,
 *    只是让当前曲目提前结束然后接着播下一曲 —— 所以没有可感知的停顿。
 *
 * 边界行为 (与自动顺序播完不同, 是刻意的):
 *   · 最后一曲时 +1 → 回到第 1 曲 (环绕)
 *   · 第一曲时 -1   → 回到第 1 曲重播
 *   · 自动顺序播完最后一曲 → 停止 (IDLE), **不**环绕
 *
 * @param delta +1 = 下一曲, -1 = 上一曲; 其他值无效
 */
void sd_player_skip(int delta);

/** 停止播放。**只做内部清理, 不碰 AirPlay 的 playback_task**。
 *  供 sd_player_play() 内部换曲用, 以及被别的音源抢 I2S 时的过渡态。 */
esp_err_t sd_player_stop(void);

/** 停止播放, 并把 I2S **完整地**交还给 AirPlay:
 *  还原 I2S 采样率到设备基准率 + 重新拉起 AirPlay 的 playback_task。
 *
 * ⚠️ 用户主动停止、以及 AirPlay 要接管时**必须**用这个, 不能用上面那个 ——
 *  否则 AirPlay 推流时没人往 I2S 写数据, 表现为"歌在播放但没有声音"。 */
esp_err_t sd_player_stop_and_yield(void);

/** 当前是否正在播放 */
bool sd_player_is_active(void);

/** 取当前状态 (线程安全) */
sd_player_state_t sd_player_get_state(void);

/** 取当前曲目信息。线程安全。
 *  任何时候都可调用; 未播放时结构体被清零。out 不能为 NULL。 */
void sd_player_get_track_info(sd_player_track_info_t *out);

/** 取最后一条错误信息, 无错误时返回空串 */
void sd_player_get_error(char *buf, size_t buf_len);

/** 交给 AirPlay: 如果正在播放, 立刻停掉并交还 I2S。
 *  由 main.c 注册到 audio_receiver 的 yield 回调上 (和 web_radio 一起)。 */
void sd_player_yield_to_airplay(void);
