#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * Web 电台 — 播放 HTTP/HTTPS 音乐流 (m3u 歌单 / 单流 URL)
 *
 * 与 AirPlay 互斥: 播放时接管 I2S (调用 audio_output_stop 让 AirPlay 让出),
 * AirPlay 一来就自动停电台并交还 I2S。
 *
 * 支持两种输入:
 *   · 单个音频直链 (mp3/aac/flac/ogg/wav), 按后缀选解码器
 *   · **m3u / m3u8 歌单**, 顺序播放整个歌单 (web_radio_skip 可手动切曲)
 *
 * ⚠️ 歌单只按 **URL 后缀** (.m3u / .m3u8, 比较前先剥掉 ?query) 识别,
 *    不做内容嗅探 —— 无后缀但返回歌单的地址不会被识别。见 .c 里的取舍说明。
 *
 * 还未实现: PLS 歌单、封面图、SPIFFS 持久化、断线自动重连。
 *   卡顿处理目前是"关闭本次会话并回到 IDLE", 不自动重连 —— 需要用户再点一次。
 *
 * ⚠️ 依赖方向: 音频输出代码在 `main` 组件里, 而 `main` 依赖本组件。
 *    所以本组件**不能** include audio_output.h (会成循环依赖)。
 *    改成由 main 在启动时把函数指针注册进来 (见 web_radio_set_output_hooks)。
 */

/** 电台播放状态, 供网页轮询 */
typedef enum {
  WEB_RADIO_IDLE = 0,   // 未播放
  WEB_RADIO_CONNECTING, // 正在连接 HTTP 流
  WEB_RADIO_PLAYING,    // 正在解码播放
  WEB_RADIO_ERROR,      // 出错 (见 web_radio_get_error)
} web_radio_state_t;

/**
 * 当前曲目信息, 供网页显示 "正在播放"。
 *
 * 标题的回退链 (永不为空):
 *   文件内嵌标签 > m3u 的 #EXTINF > 从 URL 文件名猜 > "未知曲目"
 *
 * 作者 / 专辑**只有文件里真有标签**才有值; 没有时是空串, 网页整行不显示。
 * 标签只认 UTF-8 (ID3v2.4 与 FLAC Vorbis comment 都是 UTF-8);
 * UTF-16 编码的 ID3v2.3 会被跳过, 回退到 EXTINF 标题。
 */
typedef struct {
  int index;         // 当前第几曲 (1-based); 0 = 不是歌单模式
  int count;         // 歌单总曲数; 0 或 1 = 单曲模式
  bool truncated;    // 歌单是否被 MAX_PLAYLIST 上限截断
  char title[128];   // 显示用标题 (已走完上面的回退链)
  char artist[64];   // 可能为空串
  char album[64];    // 可能为空串
  char cur_url[256]; // 当前曲的真实地址 (歌单模式下与 get_url 的返回值不同)
} web_radio_track_info_t;

/**
 * 音频输出钩子。由 main.c 注册成 audio_output_* 的包装。
 * 用函数指针而不是直接调用, 是为了避免组件循环依赖 (见文件头说明)。
 */
typedef struct {
  /** 停掉 AirPlay 的 playback task, 让出 I2S。对应 audio_output_stop()。
   *  必须在 output_write 之前调用 —— playback_task 若在运行, 它会从
   *  audio_receiver_read() 读不到数据就灌静音, 和我们的 PCM 抢同一个 DMA。 */
  void (*output_stop)(void);
  /** 往 I2S 写 PCM。对应 audio_output_write(data, bytes, wait_ticks)。
   *  直接写 I2S, 不经过 AirPlay 的接收器 —— 与 A2DP 的做法一致。 */
  int (*output_write)(const void *data, size_t bytes, uint32_t wait_ticks);
  /** 把 I2S 重新配置成指定采样率。对应 audio_output_set_sample_rate(rate)。
   *  用于匹配流的原生采样率 (如 48kHz), 避免走重采样或变调。
   *  ⚠️ 必须在开始 output_write 之前调用 —— 该函数会 disable/enable I2S
   *  通道, 有 writer 在写时会打断 DMA。 */
  void (*output_set_rate)(uint32_t rate);
  /** 查询 AirPlay 的 playback_task 是否正在运行 (audio_output_start 是否
   *  已把它拉起来)。对应 audio_output_is_active()。
   *
   *  ⚠️ 用途: 只有 AirPlay 没在跑的时候, 我们才能安全地改 I2S 采样率。
   *  一旦 AirPlay 在跑, playback_task 会按 source_rate 重采样到当前 I2S 率;
   *  这时若把 I2S 从 44100 改成 48000, audio_resample_init(44100, 48000)
   *  会**启用**重采样, 而 AirPlay 的 44.1k 源就变成 44.1k→48k→44.1k 的
   *  双重转换, 多一次无谓的 SRC。更糟的是若重采样未启用, 44.1k 会被当
   *  48k 播出去 (慢 8.8%)。所以有 AirPlay 在跑时**不要**改率。 */
  bool (*output_is_active)(void);
  /** 设置源采样率 (影响重采样判断)。对应 audio_output_set_source_rate(rate) */
  void (*output_set_source_rate)(int rate);
  /** 取当前音量 Q15 (32768 = 0dB)。对应 airplay_get_volume_q15() */
  int32_t (*get_volume_q15)(void);
  /** 中止一个正在进行的 HTTP 请求, 让阻塞中的 perform() 立刻返回。
   *  实现见 main.c 的 radio_hook_abort_http()。
   *
   *  ⚠️ 为什么需要这个而不是直接用 esp_http_client_close():
   *   close() 只是**释放连接对象**, 并不能让正在阻塞读的
   *   esp_http_client_perform() 立刻返回 —— 实测要等到读超时 (~2 秒) 才回。
   *   而"下一曲/停止"必须在几百毫秒内生效, 等不了。
   *   (IDF 5.5.5 没有 esp_http_client_abort(), 实现里走的是
   *    esp_http_client_get_socket + shutdown。)
   *
   *  ⚠️ 本组件不直接 include esp_http_client.h —— 见文件头对组件依赖方向的
   *     说明。所以这里用函数指针, 由 main 包装后注册进来。 */
  void (*abort_http)(void *client);
  /** 把 I2S 采样率**还原成设备基准率**, 并重新拉起 AirPlay 的 playback_task。
   *
   *  ⚠️⚠️ 这个钩子是修一个严重 bug 的, 不能省 (2026-09-17 实测):
   *
   *   电台播放时会按流的原生采样率调 output_set_rate() (48kHz 的歌就把 I2S
   *   改成 48k), 而**没有任何地方还原**。同时电台停播时会把 AirPlay 的
   *   playback_task 停掉让出 DMA, 也**没有任何地方重新拉起**。
   *
   *   后果: 播完网络歌曲后用 iPhone 投 AirPlay —— 歌在"播放", 但
   *   · playback_task 没在跑 → 没人往 I2S 写数据 → **完全没声音**
   *   · 而且 AirPlay 是按 OUTPUT_RATE(44.1k) 重采样的, I2S 却停在 48k
   *   → 即使拉起来也会变调
   *
   *   实测必须**复位设备**才能恢复 —— 因为只有重启才会重新走
   *   start_airplay_services() -> audio_output_start()。
   *
   *   实现见 main.c 的 radio_hook_resume_airplay()。 */
  void (*resume_airplay)(void);
} web_radio_output_hooks_t;

/**
 * 初始化电台模块。只建缓冲和状态, **不会**主动播放。
 * 必须在 web_radio_set_output_hooks 之前或之后调用都可以, 但播放前必须
 * 已注册 hooks, 否则 web_radio_play_url 会直接返回错误。
 */
esp_err_t web_radio_init(void);

/**
 * 注册音频输出钩子。必须在使用电台前调用 (由 main.c 负责)。
 * 传 NULL 会解除注册。
 */
void web_radio_set_output_hooks(const web_radio_output_hooks_t *hooks);

/**
 * 开始播放一个 URL。
 *
 *   · 指向 http:// 或 https:// 的**音频直链** → 直接播
 *   · 后缀是 .m3u / .m3u8 → 先拉取歌单, 然后**顺序播放整个歌单**
 *
 * 会先停掉当前播放, 再接管 I2S。
 *
 * @param url 音频直链或 m3u 歌单地址 (以 \0 结尾, 最长 255)
 * @return ESP_OK 已接受 (异步开始播放, 用 web_radio_get_state 查进度)
 */
esp_err_t web_radio_play_url(const char *url);

/**
 * 切上一曲 / 下一曲。只在歌单模式下有意义。
 *
 * ⚠️ 与 web_radio_play_url 的区别: 本函数**不会**释放 I2S、不会重建任务,
 *    只是让当前曲目提前结束然后接着播下一曲 —— 所以没有可感知的停顿。
 *    (play_url 会先走一遍 stop, 最长阻塞 5 秒。)
 *
 * 边界行为 (与自动顺序播完不同, 是刻意的):
 *   · 最后一曲时 +1 → 回到第 1 曲 (环绕)
 *   · 第一曲时 -1   → 回到第 1 曲重播
 *   · 自动顺序播完最后一曲 → 停止 (IDLE), **不**环绕
 *
 * @param delta +1 = 下一曲, -1 = 上一曲; 其他值无效
 */
void web_radio_skip(int delta);

/** 停止播放。**只做内部清理, 不碰 AirPlay 的 playback_task**。
 *  供 web_radio_play_url() 内部换台用 —— 那条路径紧接着就要自己接管 I2S,
 *  不能把 AirPlay 的任务拉起来 (它会灌静音抢 DMA)。 */
esp_err_t web_radio_stop(void);

/** 停止播放, 并把 I2S **完整地**交还给 AirPlay:
 *  还原 I2S 采样率到设备基准率 + 重新拉起 AirPlay 的 playback_task。
 *
 *  ⚠️ 用户主动停止、以及 AirPlay 要接管时**必须**用这个, 不能用上面那个 ——
 *  否则 AirPlay 推流时没人往 I2S 写数据, 表现为"歌在播放但没有声音",
 *  且只有重启设备才能恢复 (见 hooks 里 resume_airplay 的说明)。 */
esp_err_t web_radio_stop_and_yield(void);

/** 当前是否正在播放 (含连接中) */
bool web_radio_is_active(void);

/** 取当前状态 (线程安全) */
web_radio_state_t web_radio_get_state(void);

/** 取当前播放的 URL (用于网页回显), buf 至少 256 字节。
 *  ⚠️ 歌单模式下返回的是**歌单本身的地址** (用户输入的那个), 不是当前曲目地址。
 *     当前曲目地址见 web_radio_get_track_info 的 cur_url。 */
void web_radio_get_url(char *buf, size_t buf_len);

/** 取当前曲目信息 (标题/作者/专辑/第几曲)。线程安全。
 *  任何时候都可调用; 未播放时结构体被清零 (index=0, count=0, 各字符串为空)。
 *  @param out 输出结构体, 不能为 NULL */
void web_radio_get_track_info(web_radio_track_info_t *out);

/** 取最后一条错误信息, 无错误时返回空串 */
void web_radio_get_error(char *buf, size_t buf_len);

/** 交给 AirPlay: 如果电台正在播放, 立刻停掉。
 *  由 main.c 注册到 audio_receiver 的 yield 回调上。 */
void web_radio_yield_to_airplay(void);

