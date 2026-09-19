/*
 * SD 卡本地音乐播放器 —— 读文件 -> 解码 -> I2S
 *
 * 数据流:
 *   FATFS 文件 ──reader task──> PSRAM 环形缓冲 ──play task──> 解码
 *                                                          ──> output_write ──> I2S
 *
 * ⚠️⚠️ 本文件是 components/web_radio/web_radio.c 的姊妹实现。用户选择"独立组件"
 *     方案 (不做代码复用), 所以那边踩过的坑在这里**重新实现了一遍**。凡是标了
 *     "见 web_radio.c:xxxx" 的段落, 都是不能随手改的 —— 改动前请先把那边的
 *     说明读一遍。两边要一起维护。
 *
 * 与 web_radio 的关键差异 (本地文件比 HTTP 流简单的地方):
 *
 *   1. **没有 HTTP 任务**, 只有一个 reader task 做 fopen/fread。
 *      所以不需要 web_radio 那套 abort_http (shutdown socket) 的折磨 ——
 *      那个是为了让阻塞在 recv 上的 perform() 立刻返回才有的。fread 一次最多
 *      几十毫秒, 等它自然结束就行。
 *
 *   2. **不需要 generation (世代号)**。HTTP 任务的 perform() 退出延迟无法预测,
 *      所以要靠世代号区分"我还属不属于当前会话"; 文件读写没有这个问题。
 *
 *   3. **reader 在 play task 里启动, 不在 play() 里预启动**。web_radio 为了
 *      单曲模式不引入额外延迟而在 play_url 里先把 http_task 拉起来, 代价是
 *      "先启动再 ring_reset" 的竞态。这里让 play task 先 ring_reset 再启动
 *      reader, 竞态从根上没有了, 代价只有一次任务创建 (~0.1ms)。
 *
 *   4. 采样率/声道从解码器问, 逻辑与 web_radio 一致; 但**曲间必须重置统计**,
 *      否则上一曲的采样率会带过来 (web_radio.c:964 track_begin 的教训)。
 */

#include "sd_player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_audio_simple_dec_reg.h"

#include "decoder/impl/esp_flac_dec.h"
#include "decoder/impl/esp_mp3_dec.h"
// esp_audio_simple_dec_register_default() 覆盖 WAV / M4A(AAC) / TS / OGG ——
// 这是本地播放相对网络播放白捡的收益 (web_radio 只注册了 MP3/FLAC)。能不能
// 真的用上取决于 esp_audio_codec 的 Kconfig 开关, 失败了也不影响 MP3/FLAC。
#include "simple_dec/esp_audio_simple_dec_default.h"

#include "sd_card.h"
#include "tags.h"

// esp_mp3_dec_parse_frame / esp_flac_dec_parse_frame 在 .a 里 (弱符号), 但这个
// 版本的组件**没有导出对应的头文件**。签名必须和 esp_es_parse_func_t 一致。
// (同一处坑见 web_radio.c:74-77。)
esp_es_parse_err_t esp_mp3_dec_parse_frame(esp_es_parse_raw_t *data,
                                           esp_es_parse_frame_info_t *info);
esp_es_parse_err_t esp_flac_dec_parse_frame(esp_es_parse_raw_t *data,
                                            esp_es_parse_frame_info_t *info);

static const char *TAG = "sd_player";

// ---------------------------------------------------------------------------
// 可调参数
// ---------------------------------------------------------------------------

// 环形缓冲大小。放 PSRAM。
//
// ⚠️ 比 web_radio 的 256KB 大一倍, 因为 SD 卡读有**不可预测的停顿**
//    (卡内部垃圾回收/坏块重映射, 实测能到上百毫秒), 而 SPI 10MHz 下的持续
//    吞吐只有 ~1MB/s。缓冲越大越能扛住这些停顿。
//    512KB ≈ MP3(128kbps) 33 秒 / CD 级 FLAC 6 秒 / 24bit96k FLAC 0.6 秒。
//    高码率 FLAC 若还是跟不上, 把 CONFIG_SD_SPI_FREQ_KHZ 提到 20000。
#define RING_SIZE_BYTES (512 * 1024)

// 每次 fread 的大小。8KB 在 10MHz SPI 上约 8~10ms, 既能吃满总线又不会让
// reader 在 fread 里卡太久 (停止/切曲要等它退出)。
#define READ_CHUNK_BYTES (8 * 1024)

// 解码输出缓冲 (与 web_radio 一致: FLAC 一个 block 最大 4096 样本 = 16KB,
// 给 64KB 一次到位)。仍保留按 needed_size 扩容的路径。
#define PCM_OUT_BYTES (64 * 1024)

// 解码输入缓冲。⚠️ 存在理由见 play_task 开头的大段说明 —— simple dec 要求
// "喂进去但没消费完的字节不能被覆盖"。
// 64KB 按最坏情况定: FLAC 一个 block 压缩后约 20KB, 留 3 倍余量。
#define DEC_IN_BYTES (64 * 1024)

#define SD_TASK_STACK 6144

// ⚠️ 任务放置 (核 + 优先级) —— 必须和 web_radio 保持一致, 否则会干扰 AirPlay。
//
// audio_output.c 的注释明确写了: playback_task 优先级 9 必须高于所有音源任务,
// 因为源任务抢占它会让 DMA 环形缓冲跑空 (issue #122 的根因)。
// AirPlay 的 UDP 接收任务跑在 **核 0 / 优先级 8**, 所以:
//   - 优先级取 6 → 低于 playback_task(9), 也低于 UDP rx(8)
//   - 两个吃 CPU 的任务都放 **核 1**, 与 AirPlay 的接收链 (核 0) 分开
// 见 web_radio.c:127-140。
#define SD_TASK_PRIO 6 // < AUDIO_PLAYBACK_TASK_PRIORITY(9)
#define SD_TASK_CORE 1 // 避开 AirPlay 接收任务的核 0

// 播放列表上限。1000 x 256 字节 = 256KB, 放 PSRAM。
#define SD_PLAYLIST_MAX 1000

// 连续解码错误到多少次就丢掉本块剩余字节, 让 parser 重新同步。
// (与 web_radio.c:1437 的 WEB_RADIO_DEC_ERR_BAIL 同一个作用)
#define SD_DEC_ERR_BAIL 30

// 等 reader task 退出 / 等 play task 退出的上限
#define SD_READER_WAIT_MS 3000
#define SD_PLAY_WAIT_MS 5000

// ---------------------------------------------------------------------------
// 内部状态
// ---------------------------------------------------------------------------

typedef struct {
  uint8_t *buf;         // PSRAM
  size_t size;          // 总容量
  volatile size_t head; // 写入位置 (reader task)
  volatile size_t tail; // 读取位置 (play task)
} ring_t;

static ring_t s_ring;

static TaskHandle_t s_play_task = NULL;
static TaskHandle_t s_reader_task = NULL;

static volatile sd_player_state_t s_state = SD_PLAYER_IDLE;
static char s_error[160];

static volatile bool s_stop_req = false;    // 用户要求停止
static volatile bool s_reader_running = false;
static volatile bool s_reader_abort = false;  // 本曲被主动掐断 (切曲)
static volatile bool s_reader_failed = false; // 本曲读失败 (不是放完)

// 播放列表: 卡内相对路径 (UTF-8), PSRAM 分配。
static char(*s_pl)[SD_CARD_PATH_MAX] = NULL;
static int s_pl_count = 0;
static int s_pl_idx = 0;
static bool s_pl_truncated = false;

// 当前曲的 VFS 路径 (GBK, 带 /sdcard 前缀)。track_begin 里算一次,
// reader 直接用 —— 免得 UTF-8->GBK 的反查 (线性扫 2 万多个码位) 跑两遍。
#define SD_VFS_MAX (sizeof(SD_CARD_MOUNT_POINT) + SD_CARD_PATH_MAX + 8)
static char s_cur_vfs[SD_VFS_MAX];

static sd_player_track_info_t s_ti;
static volatile bool s_skip_req = false;
static volatile int s_skip_delta = 0;

// 解码器输出的实际格式 (从流里解出来, 不靠猜)
static uint32_t s_dec_sample_rate = 0;
static uint8_t s_dec_channels = 0;
static uint8_t s_dec_bits = 0;

static sd_player_output_hooks_t s_hooks = {0};
static bool s_hooks_ready = false;

void sd_player_set_output_hooks(const sd_player_output_hooks_t *hooks) {
  if (!hooks || !hooks->output_stop || !hooks->output_write ||
      !hooks->output_set_rate || !hooks->output_set_source_rate ||
      !hooks->output_is_active || !hooks->get_volume_q15 ||
      !hooks->resume_airplay) {
    s_hooks_ready = false;
    memset(&s_hooks, 0, sizeof(s_hooks));
    ESP_LOGW(TAG, "输出钩子已解除注册");
    return;
  }
  s_hooks = *hooks;
  s_hooks_ready = true;
  ESP_LOGI(TAG, "输出钩子已注册");
}

// ---------------------------------------------------------------------------
// 环形缓冲 (与 web_radio.c:194-253 同一套; 单生产者单消费者, 不需要锁)
// ---------------------------------------------------------------------------

static size_t ring_used(void) {
  size_t h = s_ring.head;
  size_t t = s_ring.tail;
  if (h >= t) {
    return h - t;
  }
  return s_ring.size - t + h;
}

static size_t ring_free(void) { return s_ring.size - ring_used() - 1; }

static size_t ring_write(const uint8_t *data, size_t len) {
  size_t space = ring_free();
  if (len > space) {
    len = space;
  }
  if (len == 0) {
    return 0;
  }

  size_t h = s_ring.head;
  size_t first = s_ring.size - h;
  if (first > len) {
    first = len;
  }
  memcpy(s_ring.buf + h, data, first);
  if (len > first) {
    memcpy(s_ring.buf, data + first, len - first);
  }
  s_ring.head = (h + len) % s_ring.size;
  return len;
}

static size_t ring_read(uint8_t *out, size_t len) {
  size_t used = ring_used();
  if (len > used) {
    len = used;
  }
  if (len == 0) {
    return 0;
  }

  size_t t = s_ring.tail;
  size_t first = s_ring.size - t;
  if (first > len) {
    first = len;
  }
  memcpy(out, s_ring.buf + t, first);
  if (len > first) {
    memcpy(out + first, s_ring.buf, len - first);
  }
  s_ring.tail = (t + len) % s_ring.size;
  return len;
}

// ⚠️ 只能在**两个任务都停下来**的时候调 —— 它不做任何同步。
static void ring_reset(void) {
  s_ring.head = 0;
  s_ring.tail = 0;
}

// ---------------------------------------------------------------------------
// 路径 / 后缀
// ---------------------------------------------------------------------------

// 卡内相对路径 (UTF-8) -> 可直接 fopen 的 VFS 路径。
// UTF-8 -> GBK 由 sd_card_full_path() 统一负责 (见 sd_card.c)，这里只是转发 +
// 起个本地叫得通的名字。
static esp_err_t rel_to_vfs(const char *rel, char *out, size_t out_len) {
  return sd_card_full_path(rel, out, out_len);
}

// 按扩展名挑容器类型。认不出来按 MP3 试 —— 后缀猜错时解码器会返回 NOT_SUPPORT
// 并让本曲失败, **不会静默放错音**, 所以这个简化可以接受。
// (同 web_radio.c:285)
static esp_audio_simple_dec_type_t dec_type_from_name(const char *name) {
  const char *dot = strrchr(name, '.');
  if (!dot) {
    return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
  }
  if (strcasecmp(dot, ".flac") == 0) {
    return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
  }
  if (strcasecmp(dot, ".wav") == 0) {
    return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
  }
  if (strcasecmp(dot, ".m4a") == 0 || strcasecmp(dot, ".aac") == 0) {
    return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
  }
  if (strcasecmp(dot, ".ogg") == 0 || strcasecmp(dot, ".oga") == 0) {
    return ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
  }
  return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
}

// ---------------------------------------------------------------------------
// 解码输出 -> 16bit PCM
// ---------------------------------------------------------------------------

// ⚠️⚠️ esp_audio_simple_dec 的输出样本宽度**不是**恒定的 16bit —— 它按源文件的
//     位深原样输出, 位深由 esp_audio_simple_dec_get_info() 的 bits_per_sample
//     给出 (头文件把它归在"can be used for play"那一类, 也就是**输出**格式)。
//
//     实测 (2026-09-17, 24bit/48kHz FLAC):
//       前 24 字节按 3 字节一组切 = 0, 0, -1, -1, +1, 0, 0, 0   ← 音频
//       按 4 字节一组切         = 0, 0xFFFF0000, 0x01FFFFFF    ← 跳变, 不是音频
//       且 decoded_size/3/2 = 24576/3/2 = 4096 = FLAC 标准块长 (按 4 字节算是
//       3072, 对不上)。
//     ⇒ 24bit 是 **每样本 3 字节、小端、紧凑排列**。
//
//     之前这里恒按 int16 处理, 于是 24bit 素材被"每 2 字节重新切一刀": 有声音、
//     速度也对, 但内容全是垃圾 —— 用户听到的就是"全是嘈杂的杂音"。
//     16bit 的 MP3 / FLAC 一直正常, 所以这个 bug 只在 24bit 素材上暴露。
//     (components/web_radio/web_radio.c 里有同一段逻辑, 同一处坑, 一起改了。)

// 从 bps 字节的小端样本里取出有符号 32 位值 (bps <= 4)。
static int32_t sample_to_s32(const uint8_t *p, size_t bps) {
  uint32_t v = 0;
  for (size_t i = 0; i < bps; i++) {
    v |= (uint32_t)p[i] << (8 * i);
  }
  // bps < 4 时高位要手动补符号位; bps == 4 本身就是完整的 int32
  // (注意 1u << 32 是未定义行为, 所以这个分支必须排除 bps == 4)。
  if (bps < 4 && (v & (1u << (bps * 8 - 1)))) {
    v |= ~((1u << (bps * 8)) - 1u);
  }
  return (int32_t)v;
}

// 把解码器输出**就地**降成 16bit 交错 PCM, 返回转换后的字节数。
// ⚠️ bps <= 2 时一个字节都不动, 直接返回原长度 —— 16bit 素材走的是和以前
//    完全相同的路径, 不受这次改动影响。
static size_t pcm_to_s16(int16_t *buf, size_t bytes, size_t bps) {
  if (bps <= 2) {
    return bytes;
  }
  size_t n = bytes / bps; // 所有声道的样本总数
  const uint8_t *src = (const uint8_t *)buf;
  int shift = (int)(bps * 8) - 16;
  // 就地转换是安全的: 写入位置 2i 永远 <= 读取位置 bps*i, 不会覆盖还没读的数据。
  for (size_t i = 0; i < n; i++) {
    buf[i] = (int16_t)(sample_to_s32(src + i * bps, bps) >> shift);
  }
  return n * sizeof(int16_t);
}

// ---------------------------------------------------------------------------
// 音量
// ---------------------------------------------------------------------------

// 把 Q15 音量 (32768 = 0dB) 应用到 16-bit PCM。原地改。(同 web_radio.c:1086)
static void apply_volume(int16_t *pcm, size_t samples) {
  if (!s_hooks_ready || !s_hooks.get_volume_q15) {
    return;
  }
  int32_t vol_q15 = s_hooks.get_volume_q15();
  if (vol_q15 >= 32768) {
    return; // unity, 不动
  }
  if (vol_q15 < 0) {
    vol_q15 = 0;
  }
  for (size_t i = 0; i < samples; i++) {
    pcm[i] = (int16_t)(((int32_t)pcm[i] * vol_q15) >> 15);
  }
}

// ---------------------------------------------------------------------------
// 播放列表
// ---------------------------------------------------------------------------

static void set_error(const char *msg) {
  strlcpy(s_error, msg ? msg : "", sizeof(s_error));
  s_state = SD_PLAYER_ERROR;
  ESP_LOGE(TAG, "%s", s_error);
}

static void playlist_free(void) {
  if (s_pl) {
    heap_caps_free(s_pl);
    s_pl = NULL;
  }
  s_pl_count = 0;
  s_pl_idx = 0;
  s_pl_truncated = false;
}

// 拼一个卡内路径。dir 为 "/" 时不要拼出 "//name"。
// 返回 false 表示拼出来超长被截断了 —— 调用者应该跳过这一条, 而不是拿着一个
// 被截断的错路径去 fopen (那只会得到一个打不开的文件名)。
static bool join_rel(const char *dir, const char *name, char *out,
                     size_t out_len) {
  int n;
  if (strcmp(dir, "/") == 0) {
    n = snprintf(out, out_len, "/%s", name);
  } else {
    n = snprintf(out, out_len, "%s/%s", dir, name);
  }
  return n > 0 && (size_t)n < out_len;
}

// 枚举目录里所有音频文件, 建成播放列表。
//
// select_path 非 NULL 时, 把 s_pl_idx 定位到它 (点单曲的场景); 找不到就当 0。
static esp_err_t playlist_build(const char *dir, const char *select_path) {
  playlist_free();

  sd_entry_t *entries = NULL;
  size_t n = 0;
  bool trunc = false;
  esp_err_t err = sd_card_list(dir, &entries, &n, &trunc);
  if (err != ESP_OK) {
    return err;
  }

  s_pl = heap_caps_calloc(SD_PLAYLIST_MAX, SD_CARD_PATH_MAX, MALLOC_CAP_SPIRAM);
  if (!s_pl) {
    sd_card_free_list(entries);
    return ESP_ERR_NO_MEM;
  }

  for (size_t i = 0; i < n && s_pl_count < SD_PLAYLIST_MAX; i++) {
    if (entries[i].is_dir || !sd_card_is_audio_file(entries[i].name)) {
      continue;
    }
    if (!join_rel(dir, entries[i].name, s_pl[s_pl_count], SD_CARD_PATH_MAX)) {
      ESP_LOGW(TAG, "跳过路径过长的文件: %s", entries[i].name);
      continue;
    }
    s_pl_count++;
  }
  sd_card_free_list(entries);

  s_pl_truncated = trunc || s_pl_count >= SD_PLAYLIST_MAX;

  if (s_pl_count == 0) {
    return ESP_ERR_NOT_FOUND; // 目录里没有能播的文件
  }

  s_pl_idx = 0;
  if (select_path) {
    for (int i = 0; i < s_pl_count; i++) {
      if (strcmp(s_pl[i], select_path) == 0) {
        s_pl_idx = i;
        break;
      }
    }
  }
  return ESP_OK;
}

// 进入第 idx 曲: 更新对外可见的曲目信息 + 算好 VFS 路径 + 重置本曲统计。
//
// ⚠️ 跨曲必须重置这些全局量 —— 它们都是**单份**的, 不清就会带着上一曲的值跑
//    (网页上采样率不更新、声道数错、已播时间从上一曲接着涨)。
//    (同 web_radio.c:964)
static void track_begin(int idx) {
  s_ti.index = idx + 1;
  s_ti.count = s_pl_count;
  s_ti.truncated = s_pl_truncated;
  s_ti.artist[0] = '\0';
  s_ti.album[0] = '\0';
  s_ti.elapsed_sec = 0;
  s_ti.sample_rate = 0;

  strlcpy(s_ti.path, s_pl[idx], sizeof(s_ti.path));

  // 标题先用文件名铺底, 标签解析成功再覆盖 (标签解不出来就保住文件名,
  // 网页上永远有东西显示)。
  sd_title_from_name(s_pl[idx], s_ti.title, sizeof(s_ti.title));

  // 本曲的 VFS 路径 —— reader 任务直接用, 不用再转一次
  if (rel_to_vfs(s_pl[idx], s_cur_vfs, sizeof(s_cur_vfs)) != ESP_OK) {
    s_cur_vfs[0] = '\0';
  }

  // 读文件头拿标签。失败不影响播放。
  if (s_cur_vfs[0]) {
    char t[128], ar[64], al[64];
    sd_tags_read(s_cur_vfs, t, sizeof(t), ar, sizeof(ar), al, sizeof(al));
    if (t[0]) {
      strlcpy(s_ti.title, t, sizeof(s_ti.title));
    }
    if (ar[0]) {
      strlcpy(s_ti.artist, ar, sizeof(s_ti.artist));
    }
    if (al[0]) {
      strlcpy(s_ti.album, al, sizeof(s_ti.album));
    }
  }

  // 本曲统计归零
  s_pl_idx = idx;
  s_reader_failed = false;
  s_reader_abort = false;
  s_dec_sample_rate = 0;
  s_dec_channels = 0;
}

// 消费切曲请求并算出新的曲目序号。没有请求就原样返回 cur。
//
// ⚠️⚠️ 必须是**唯一**处理 s_skip_req 的地方。这个标志在两个位置会被看到
//    (曲目循环顶部 / 本曲解码循环顶部), 谁先看到谁就调用它 —— 如果只清标志
//    而不算 cur, 后来那个位置就再也算不出来了, 「下一曲」会退化成重播当前曲。
//    (同 web_radio.c:1029)
static int apply_skip(int cur) {
  if (!s_skip_req) {
    return cur;
  }
  int d = s_skip_delta;
  s_skip_req = false;
  s_skip_delta = 0;

  if (s_pl_count <= 1) {
    return cur;
  }
  // 手动「下一条」在最后一曲 → 环绕回第一曲;
  // 手动「上一条」在第一曲 → 回第一曲重播 (不绕到末尾)。
  int n = (d > 0) ? (cur + 1) % s_pl_count : (cur > 0 ? cur - 1 : 0);
  ESP_LOGI(TAG, "切到第 %d 曲 (共 %d)", n + 1, s_pl_count);
  return n;
}

// ---------------------------------------------------------------------------
// reader task: 文件 -> 环形缓冲
// ---------------------------------------------------------------------------

// arg 是曲目序号 (用 intptr_t 传, 不能传栈上的指针)
static void reader_task(void *arg) {
  (void)arg;

  FILE *f = fopen(s_cur_vfs, "rb");
  if (!f) {
    ESP_LOGE(TAG, "打开失败: %s", s_ti.path);
    s_reader_failed = true;
    goto done;
  }

  uint8_t *buf = heap_caps_malloc(READ_CHUNK_BYTES, MALLOC_CAP_SPIRAM);
  if (!buf) {
    ESP_LOGE(TAG, "读缓冲分配失败");
    fclose(f);
    s_reader_failed = true;
    goto done;
  }

  ESP_LOGI(TAG, "开始读: %s", s_ti.path);

  while (!s_stop_req && !s_reader_abort) {
    size_t space = ring_free();
    if (space == 0) {
      // 缓冲满 —— 播放任务还没消费完。**只能等**, 这就是流控。
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    size_t want = space < READ_CHUNK_BYTES ? space : READ_CHUNK_BYTES;
    size_t got = fread(buf, 1, want, f);

    if (got == 0) {
      // EOF 或被掐断。ferror 才是真出错 —— 被我们主动掐断不算失败
      // (掐断时 fread 正常返回 0)。
      if (ferror(f) && !s_reader_abort && !s_stop_req) {
        ESP_LOGE(TAG, "读文件出错: %s", s_ti.path);
        s_reader_failed = true;
      }
      break;
    }

    size_t written = ring_write(buf, got);
    if (written < got) {
      // 只可能发生在"算完空间之后又被消费掉"? 不会 —— 单消费者只会让空间
      // 变多。真出现了说明有 bug, 丢掉的字节会让解码报错, 记一笔。
      ESP_LOGW(TAG, "环形缓冲只写进 %u/%u 字节", (unsigned)written,
               (unsigned)got);
    }
  }

  heap_caps_free(buf);
  fclose(f);

done:
  s_reader_running = false;
  s_reader_task = NULL;
  vTaskDelete(NULL);
}

static esp_err_t start_reader(void) {
  s_reader_abort = false;
  s_reader_failed = false;
  s_reader_running = true;

  if (xTaskCreatePinnedToCore(reader_task, "sd_reader", SD_TASK_STACK, NULL,
                              SD_TASK_PRIO, &s_reader_task,
                              SD_TASK_CORE) != pdPASS) {
    s_reader_running = false;
    set_error("读取任务创建失败");
    return ESP_FAIL;
  }
  return ESP_OK;
}

// 等 reader 退出。切曲/停止时必须等到它真的没了才能 ring_reset()。
// 返回 false 表示超时 —— reader 还活着, 这时**绝不能**进下一曲:
// 它会继续往环形缓冲里灌上一个文件的字节, 而新曲的 ring_reset() 会把 tail
// 归零, 旧数据立刻变成"新曲的数据"被解码。(web_radio.c:1685 同一个处理。)
static bool stop_reader_and_wait(void) {
  s_reader_abort = true;
  for (int i = 0; i < (SD_READER_WAIT_MS / 20) && s_reader_task != NULL; i++) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  if (s_reader_task != NULL) {
    // fread 一次最多几十毫秒, 正常情况下不该走到这里
    ESP_LOGW(TAG, "reader 任务未按时退出");
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// play task: 环形缓冲 -> 解码 -> I2S
// ---------------------------------------------------------------------------

static void play_task(void *arg) {
  int cur = (int)(intptr_t)arg; // 起始曲目序号

  // ⚠️⚠️ 解码输入缓冲 (PSRAM, 64KB) —— 这是整个文件最容易搞错的地方。
  //
  // esp_audio_simple_dec 的输入契约 (见 espressif 自己的 simple_decoder_test.c
  // 以及头文件里 raw.consumed 的定义):
  //
  //   1. 一次 process() **不代表**把喂进去的字节用完了。它可能只消费了前面
  //      若干字节 (raw.consumed), 剩下的要你**原样再喂一遍**。
  //   2. 返回 OK 且 decoded_size==0 也是**正常**的, 意思是"这些字节我收下了 /
  //      还没凑够一帧", 不是错误。
  //   3. 没消费完的字节, 下次调用时内容**必须保持不变**:
  //        "ATTENTION: when input raw data unconsumed (`raw.len > 0`) do not
  //         overwrite its content. Or-else unexpected error may happen for
  //         data corrupt."
  //
  // 所以正确做法是:
  //     raw.len -= raw.consumed;
  //     raw.buffer += raw.consumed;
  // 把游标在**同一块缓冲内部**往前推, 直到 raw.len == 0 才读下一块。
  //
  // 之前 web_radio 这里是错的 (每轮读一块新数据, process 一次就丢), 症状是
  // "所有计数器都健康, 但 PCM 是间断的", 听感就是持续的糊/爆。
  // 详见 web_radio.c:1303-1335。**别在这里改成"读一块解一块"。**
  uint8_t *enc_buf = heap_caps_malloc(DEC_IN_BYTES, MALLOC_CAP_SPIRAM);
  size_t pcm_buf_size = PCM_OUT_BYTES;
  int16_t *pcm_buf = heap_caps_malloc(pcm_buf_size, MALLOC_CAP_SPIRAM);
  if (!enc_buf || !pcm_buf) {
    set_error("内存不足");
    goto cleanup;
  }

  // =========================================================================
  // ⚠️ 曲目循环 —— 顺序播放就靠这一层。
  //
  //  1. play_task **跨曲存活**。一首放完不退出任务, 而是接着开下一曲 ——
  //     任务退出/重建意味着重新抢 I2S、走一遍 sd_player_stop() 的最长 5 秒
  //     阻塞等待, 曲间会有很明显的停顿。
  //  2. I2S **全程握在播放器手里**, 曲间不调 output_stop()/output_start()。
  //  3. 曲间切换**绝不能**调 sd_player_play()/sd_player_stop() —— 那两个函数
  //     都会等 s_play_task == NULL, 而这里就是 s_play_task 本身, 自等自必然
  //     超时。所以 next/prev 走 s_skip_req 标志。
  //  (同 web_radio.c:1344-1371。)
  //
  // ⚠️⚠️ 这个循环体里**只允许有一个 `continue`**, 就是最末尾"自动播下一曲"
  //      那一个 —— 它前面紧跟着 `cur++`, 是两个绑在一起的语句。
  //      在循环体**前面**再加一个 `continue` 会直接跳到 `cur++` 上, 结果是
  //      跳过 next_track 整段 (关解码器 / 等 reader 退出 / 失败检查), 而且刚
  //      设好的 cur 又被 +1, 直接多跳一首。曲间切换一律走 `goto next_track`。
  // =========================================================================
  for (;;) {
    cur = apply_skip(cur);

    if (cur >= s_pl_count) {
      break; // 越界 (理论上不该发生), 收工
    }

    track_begin(cur);

    // ⚠️ 顺序: 先 ring_reset() 再启动 reader。
    //    反过来的话 (先启动 reader 再 reset) 会把新曲刚写进去的头部字节清掉,
    //    表现为每首歌开头一小段杂音。上一曲的 reader 在上一轮的 next_track
    //    里已经确认退出了, 所以这里 reset 是安全的。
    ring_reset();

    if (start_reader() != ESP_OK) {
      break;
    }

    // ---- 本曲开始 ----
    esp_audio_simple_dec_handle_t dec = NULL;
    esp_audio_simple_dec_type_t dec_type = dec_type_from_name(s_pl[cur]);

    esp_audio_simple_dec_cfg_t dec_cfg = {
        .dec_type = dec_type,
        .dec_cfg = NULL,
        .cfg_size = 0,
        .use_frame_dec = false, // 让内部 parser 处理任意长度的输入
    };

    esp_audio_err_t aerr = esp_audio_simple_dec_open(&dec_cfg, &dec);
    if (aerr != ESP_AUDIO_ERR_OK) {
      char msg[160];
      snprintf(msg, sizeof(msg), "解码器打开失败 (第%d曲, type=%d): %d", cur + 1,
               (int)dec_type, aerr);
      set_error(msg);
      s_reader_failed = true; // 让下面判失败, 不自动跳下一曲
      goto next_track;
    }
    ESP_LOGI(TAG, "第%d曲 解码器已打开 (type=%d)", cur + 1, (int)dec_type);

    bool got_format = false;
    uint32_t elapsed_samples = 0; // 本曲写出的 PCM 采样帧数 (每声道)
    uint32_t dbg_decode_errs = 0;

    while (!s_stop_req) {
      // 切曲: 丢掉上一曲在缓冲里的残留 (最多 512KB)。
      // 不丢的话要**听它播完**才切得过去 —— 那是几十秒的延迟。
      //
      // ⚠️ 这里用 apply_skip() 而不是只清标志 —— 它会顺手把目标曲目算好存进
      //    cur。只清不算的话, 回到循环顶部标志已空, cur 没变, 「下一曲」就退化
      //    成重播当前曲。(web_radio.c:1443 实测踩过。)
      if (s_skip_req) {
        cur = apply_skip(cur);
        ring_reset();
        ESP_LOGI(TAG, "丢弃缓冲残留, 立即切曲");
        goto next_track;
      }

      size_t avail = ring_used();

      if (avail == 0) {
        if (!s_reader_running) {
          ESP_LOGI(TAG, "第%d曲 读完且缓冲已放完", cur + 1);
          break; // 本曲放完 (或读失败)
        }
        // 还在读, 等一会儿
        vTaskDelay(pdMS_TO_TICKS(20));
        continue;
      }

      // 从环形缓冲取一块新数据 (此时 enc_buf 上一批必然已消费完)
      size_t want = avail > DEC_IN_BYTES ? DEC_IN_BYTES : avail;
      size_t got = ring_read(enc_buf, want);
      if (got == 0) {
        continue;
      }

      esp_audio_simple_dec_raw_t raw = {
          .buffer = enc_buf,
          .len = (uint32_t)got,
          .eos = false,
          .consumed = 0,
          .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
      };

      // ⚠️ 内层循环: 把这一块**喂干净**才去读下一块。这是修复的核心 ——
      //    见函数开头对 raw.consumed 契约的说明。
      while (raw.len > 0 && !s_stop_req) {
        // 单声道流要把解码结果**原地翻倍**扩成立体声 (见下面
        // s_dec_channels == 1)，所以解码器最多只准用一半缓冲区。
        // 声道数要到首帧解出来之后才从解码器拿到，首帧那一趟靠下面的
        // 扩容兜底。
        uint32_t out_cap = (uint32_t)pcm_buf_size;
        if (s_dec_channels == 1) {
          out_cap /= 2;
        }
        esp_audio_simple_dec_out_t out = {
            .buffer = (uint8_t *)pcm_buf,
            .len = out_cap,
            .needed_size = 0,
            .decoded_size = 0,
        };

        esp_audio_err_t r = esp_audio_simple_dec_process(dec, &raw, &out);

        // 即使出错也要推进游标, 否则会卡在同一批字节上转不出去。
        uint32_t consumed = raw.consumed;

        if (r == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
          // 输出缓冲不够。按 needed_size 真正扩容, 然后重试同一帧
          // (这次不推进游标 —— 帧还没解出来)。
          //
          // ⚠️ 单声道要再乘 2：上面把 out.len 收成了 pcm_buf_size/2，若只扩到
          //    needed_size，下一趟 out.len 仍然小于 needed_size，这个分支就会
          //    原地打转 —— 必须保证 pcm_buf_size >= 2 * needed_size。
          size_t need =
              out.needed_size ? out.needed_size : (pcm_buf_size * 2);
          if (s_dec_channels == 1) {
            need *= 2;
          }
          if (need > pcm_buf_size) {
            int16_t *nb = heap_caps_realloc(pcm_buf, need, MALLOC_CAP_SPIRAM);
            if (!nb) {
              set_error("PCM 缓冲扩容失败");
              goto cleanup;
            }
            pcm_buf = nb;
            pcm_buf_size = need;
            ESP_LOGI(TAG, "PCM 缓冲扩到 %u 字节", (unsigned)need);
          }
          continue;
        }

        if (r != ESP_AUDIO_ERR_OK) {
          dbg_decode_errs++;
          if (dbg_decode_errs <= 6) {
            ESP_LOGW(TAG, "解码错误 %d (已消费 %u/%u)", r, (unsigned)consumed,
                     (unsigned)raw.len);
          }
          // 连续错太多次说明这批数据没救了: 丢掉剩余部分, 从下一个环形缓冲块
          // 重新开始 (parser 会重新找帧头)。
          if (dbg_decode_errs % SD_DEC_ERR_BAIL == 0) {
            ESP_LOGW(TAG, "连续解码错误 %u 次, 丢弃本块剩余 %u 字节",
                     (unsigned)dbg_decode_errs, (unsigned)raw.len);
            break;
          }
          raw.len -= consumed;
          raw.buffer += consumed;
          if (consumed == 0) {
            break; // 推进不动了, 别死循环
          }
          continue;
        }

        // OK: 推进游标。**注意 consumed 可能是 0** (字节已被解码器收进内部
        // 缓存), 那种情况下 raw.len 不变, 必须跳出内层循环去读下一块 ——
        // 否则就是死循环。
        raw.len -= consumed;
        raw.buffer += consumed;

        if (out.decoded_size == 0) {
          // OK 但没出 PCM: 数据不够一帧, 已被解码器收下。正常路径。
          if (consumed == 0) {
            break;
          }
          continue;
        }

        // 首次拿到 PCM 时问解码器真实格式
        if (!got_format) {
          esp_audio_simple_dec_info_t info = {0};
          esp_audio_err_t ierr = esp_audio_simple_dec_get_info(dec, &info);
          if (ierr == ESP_AUDIO_ERR_OK && info.sample_rate > 0) {
            got_format = true;
            s_dec_sample_rate = info.sample_rate;
            s_dec_channels = info.channel;
            s_dec_bits = info.bits_per_sample;
            s_ti.sample_rate = info.sample_rate;
            ESP_LOGI(TAG, "文件格式: %u Hz, %u ch, %u bit",
                     (unsigned)info.sample_rate, (unsigned)info.channel,
                     (unsigned)info.bits_per_sample);

            // I2S 输出率跟随文件的原生采样率。
            //
            // 背景: 项目默认 OUTPUT_RATE=44100, AirPlay 用它的 playback_task 把
            // 44.1k 源重采样到 OUTPUT_RATE。但我们**不启用**那个任务 (见下面
            // "绝对不要调 output_start"), 所以没有 SRC。48kHz 的文件若直接喂给
            // 44.1k 的 I2S 会快 8.8% (音调偏高)。
            //
            // ⚠️ 守卫: 只有 AirPlay 的 playback_task **没在跑**时才改 I2S 采样率。
            //    一旦它在跑, set_sample_rate 会改变 playback_task 重采样器的目标
            //    率, 把 AirPlay 的 44.1k 源拖进无谓的双重转换。改用
            //    output_set_source_rate 更安全 —— 它只影响重采样判断。
            //    (同 web_radio.c:1582, 那边实测踩过。)
            s_hooks.output_set_source_rate((int)info.sample_rate);

            if (!s_hooks.output_is_active()) {
              s_hooks.output_set_rate(info.sample_rate);
              ESP_LOGI(TAG, "I2S 采样率已切到 %u Hz",
                       (unsigned)info.sample_rate);
            } else {
              ESP_LOGW(TAG, "AirPlay 仍占用输出, 不切 I2S 率 (源 %u Hz)",
                       (unsigned)info.sample_rate);
            }
          }
        }

        // ⚠️ 这里**绝对不要**调 output_start()!
        //
        // audio_output_start() 启动的是 AirPlay 的 playback_task, 它从
        // audio_receiver_read() 取数据 —— 我们的数据根本不走那条路。一旦启动,
        // 它每 ~8ms 读不到数据就往同一个 I2S DMA 灌一帧静音, 和我们直写的 PCM
        // 互相覆盖, 结果是持续的爆音/杂音。
        //
        // 正确做法看 A2DP (同样是外部音源): 只调 audio_output_write() 直写 I2S,
        // 前提是 playback_task 已停 —— sd_player_play() 里的 output_stop()
        // 负责这个。(同 web_radio.c:1600。)
        // ---- 解码器输出可能是 24bit, 先统一降成 16bit 再交给 I2S ----
        // 位深取自解码器自己的报告 (见 pcm_to_s16 上面那段说明)。
        size_t bps = (size_t)((s_dec_bits + 7) / 8);
        if (bps < 2) {
          bps = 2; // 位深未知 / <= 16bit: 按 16bit 处理 (最常见的情况)
        }
        out.decoded_size =
            (uint32_t)pcm_to_s16(pcm_buf, out.decoded_size, bps);

        size_t samples = out.decoded_size / sizeof(int16_t);
        apply_volume(pcm_buf, samples);

        // I2S 是 16bit 立体声 slot。单声道文件必须复制成两声道, 否则 I2S 会按
        // "L/R 交替" 去读单声道样本 —— 左右耳各听到一半采样, 等于把采样率减半,
        // 声音又闷又怪。
        if (s_dec_channels == 1) {
          // ⚠️ 原地复制要把数据翻倍。稳态下 out.len 已经收成一半、装不下不可能
          //    发生；唯一还能走到这里装不下的情况是**首帧** —— 那一趟声道数还
          //    不知道，out.len 给的是全尺寸。
          size_t want = samples * 2 * sizeof(int16_t);
          if (want > pcm_buf_size) {
            int16_t *nb = heap_caps_realloc(pcm_buf, want, MALLOC_CAP_SPIRAM);
            if (nb) {
              pcm_buf = nb;
              pcm_buf_size = want;
            } else {
              // 扩不动: 截到装得下的帧数。代价是这一帧尾巴上几十毫秒的音频，
              // 换来的是不写坏 PSRAM 堆。
              samples = pcm_buf_size / (2 * sizeof(int16_t));
              want = samples * 2 * sizeof(int16_t);
            }
          }
          int16_t *p = pcm_buf;
          for (size_t i = samples; i > 0; i--) {
            p[(i - 1) * 2] = p[i - 1];
            p[(i - 1) * 2 + 1] = p[i - 1];
          }
          out.decoded_size = (uint32_t)want;
        }

        s_hooks.output_write(pcm_buf, out.decoded_size, 500);

        // 已播时长: 按实际交给 I2S 的**采样帧数**算。
        //
        // ⚠️ 恒除以 4, 不用 s_dec_channels —— I2S 是 16bit 立体声 slot, 上面的
        //    单声道复制已经把 decoded_size 翻倍了, 所以到这里永远是
        //    "每帧 2 声道 x 2 字节 = 4 字节"。用 s_dec_channels 算的话, 一旦
        //    解码器还没报格式 (s_dec_channels == 0) 就会除零。
        //    这个数只给网页显示用, 不追求精确 (暂停/切曲会重置)。
        elapsed_samples += out.decoded_size / 4;
        if (s_dec_sample_rate) {
          s_ti.elapsed_sec = elapsed_samples / s_dec_sample_rate;
        }
      }
    } // while 本曲解码循环

  next_track:
    if (dec) {
      esp_audio_simple_dec_close(dec);
      dec = NULL;
    }

    // ⚠️⚠️ 必须在等 reader 退出**之前**把两个标志记下来。
    //    stop_reader_and_wait() 会无条件置 s_reader_abort (它无法区分"reader 还
    //    在跑要掐断"和"reader 已经自然结束"), 等它返回后再读 s_reader_abort 就
    //    永远是 true —— 那样下面的 `if (aborted) continue` 会把**每一首**自然
    //    放完的歌都当成"被手动切走", 结果是播完一首就原地重播, 永远不自动进入
    //    下一曲。
    bool aborted = s_reader_abort;
    bool failed = s_reader_failed;

    // ⚠️ 必须等 reader 真正退出再进下一曲。否则它还在往环形缓冲里灌**上一曲
    //    的字节**, 而下一曲的 ring_reset() 会把 tail 归零 —— 旧数据立刻变成
    //    "新曲的数据" 被解码, 表现为切曲后马上解码失败或放出一段杂音。
    //    (web_radio.c:1674 同一个坑。)
    if (!stop_reader_and_wait()) {
      ESP_LOGW(TAG, "reader 收不回来, 停止播放以免串曲");
      s_stop_req = true;
      break;
    }

    if (s_stop_req) {
      break; // 用户点了停止
    }

    // 读取失败 (不是放完、也不是被掐断) → 停住报错, **不自动跳下一曲**。
    // 否则卡一有问题就会一首接一首地跳, 日志刷屏且毫无意义。
    // (同 web_radio.c:1703。格式不支持的文件也会走到这里 —— 想跳过坏文件的话
    //  手动点「下一条」即可。)
    if (failed && !aborted) {
      if (s_state != SD_PLAYER_ERROR) {
        set_error("读取或解码失败");
      }
      break;
    }

    // ⚠️ 被掐断 (用户点了「下一曲/上一曲」) → **不要**走下面的 cur++。
    //    目标曲目在循环开头已经按 s_skip_req 算好了, 再 ++ 就多跳一首。
    if (aborted) {
      ESP_LOGI(TAG, "本曲被手动切走, 转到第 %d 曲", cur + 1);
      continue;
    }

    // 自然放完 → 决定有没有下一曲
    if (cur + 1 < s_pl_count) {
      cur++;
      ESP_LOGI(TAG, "自动播下一曲 (第 %d/%d)", cur + 1, s_pl_count);
      continue;
    }

    // 最后一曲放完 → 停止 (不环绕; 手动点「下一条」才环绕)
    ESP_LOGI(TAG, "播放列表完毕 (%d 曲)", s_pl_count);
    s_state = SD_PLAYER_IDLE;
    break;
  } // for 曲目循环

  // 注意: 这里不需要 audio_output_stop()。
  // 我们从没启动过 AirPlay 的 playback_task (见上面"绝对不要调 output_start"
  // 的说明), 所以没有需要停的东西。I2S 就是从 AirPlay 手里接管过来的, 交还由
  // sd_player_stop_and_yield() 或 AirPlay 的 yield 回调完成。

cleanup:
  // ⚠️ 走到这里可能是从内层循环直接跳出来的 (内存不足 / PCM 扩容失败),
  //    那条路上 reader 可能**还在跑**。必须收掉它, 否则:
  //      · play 任务退出后没人再消费环形缓冲, reader 会一直空转到缓冲满,
  //        然后卡在"等空间"的循环里永远不退出;
  //      · sd_player_stop() 于是永远等不到它, 5 秒超时后**保持** s_stop_req,
  //        之后再也播不了 (只能重启)。
  //    正常收尾路径上 reader 已经收过了, 这个判断不会命中。
  if (s_reader_task != NULL) {
    s_stop_req = true;
    stop_reader_and_wait();
  }

  heap_caps_free(enc_buf);
  heap_caps_free(pcm_buf);

  s_play_task = NULL;
  vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------

esp_err_t sd_player_init(void) {
  memset(&s_ring, 0, sizeof(s_ring));

  // ⚠️ MP3 / FLAC 必须**手动注册**给 simple dec。
  //   esp_audio_simple_dec_register_default() 在 Kconfig 里只有 WAV/M4A/TS/OGG
  //   四个开关, **不含 MP3/FLAC** —— 不注册的话 open 会返回 -7 (NOT_SUPPORT)。
  //   (见 web_radio.c:1752, 同一处坑。)
  //
  //   先调 register_default() 把 WAV/M4A/OGG 也带上 —— 本地播放不需要考虑
  //   流量, 支持的格式越多越好。失败不致命 (那几种格式放不了而已)。
  esp_audio_err_t derr = esp_audio_simple_dec_register_default();
  if (derr != ESP_AUDIO_ERR_OK) {
    ESP_LOGW(TAG, "默认解码器注册失败 (%d) —— WAV/M4A/OGG 可能不可用", derr);
  }

  static const struct {
    esp_audio_simple_dec_type_t type;
    esp_audio_dec_ops_t ops;
    esp_es_parse_func_t parser;
    const char *name;
  } codecs[] = {
      {ESP_AUDIO_SIMPLE_DEC_TYPE_MP3, ESP_MP3_DEC_DEFAULT_OPS(),
       esp_mp3_dec_parse_frame, "MP3"},
      {ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC, ESP_FLAC_DEC_DEFAULT_OPS(),
       esp_flac_dec_parse_frame, "FLAC"},
  };

  for (size_t i = 0; i < sizeof(codecs) / sizeof(codecs[0]); i++) {
    esp_audio_simple_dec_reg_info_t reg_info = {
        .decoder_ops = codecs[i].ops,
        .parser = codecs[i].parser,
        .free = NULL,
    };
    esp_audio_err_t reg_err =
        esp_audio_simple_dec_register(codecs[i].type, &reg_info);
    if (reg_err != ESP_AUDIO_ERR_OK) {
      ESP_LOGE(TAG, "%s 注册失败: %d", codecs[i].name, reg_err);
      return ESP_FAIL;
    }
    ESP_LOGI(TAG, "%s 解码器已注册", codecs[i].name);
  }

  s_ring.buf = heap_caps_malloc(RING_SIZE_BYTES, MALLOC_CAP_SPIRAM);
  if (!s_ring.buf) {
    ESP_LOGE(TAG, "PSRAM 分配失败 (%d 字节)", RING_SIZE_BYTES);
    return ESP_ERR_NO_MEM;
  }
  s_ring.size = RING_SIZE_BYTES;

  s_state = SD_PLAYER_IDLE;
  s_error[0] = '\0';

  ESP_LOGI(TAG, "SD 播放器就绪 (缓冲 %d KB, 放 PSRAM)", RING_SIZE_BYTES / 1024);
  return ESP_OK;
}

// 内部实现, 定义在下面。true = 顺带把 I2S 交还给 AirPlay。
static esp_err_t player_stop_internal(bool yield_to_airplay);

esp_err_t sd_player_stop(void) { return player_stop_internal(false); }

esp_err_t sd_player_stop_and_yield(void) { return player_stop_internal(true); }

static esp_err_t player_stop_internal(bool yield_to_airplay) {
  // ⚠️ 早退路径也要走"交还 I2S": "SD 的歌播完后"就是走这个分支 (play_task 已
  //    自己退出并把状态置成 IDLE), 而它恰恰是最需要交还 I2S 的时刻。
  //    stop_and_yield 是幂等的, 重复调用无害。
  //    (同 web_radio.c:1816, 那边因为这里直接 return 而出过"投 AirPlay 没声音"
  //    的 bug。)
  if (s_play_task == NULL && s_reader_task == NULL) {
    s_state = SD_PLAYER_IDLE;
    if (yield_to_airplay && s_hooks_ready && s_hooks.resume_airplay) {
      s_hooks.resume_airplay();
      ESP_LOGI(TAG, "播放器已空闲, I2S 交还 AirPlay");
    }
    return ESP_OK;
  }

  ESP_LOGI(TAG, "停止当前播放");
  s_stop_req = true;
  s_reader_abort = true; // 让 reader 尽快跳出 fread 循环

  for (int i = 0; i < (SD_PLAY_WAIT_MS / 50); i++) {
    if (s_play_task == NULL && s_reader_task == NULL) {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  if (s_play_task != NULL || s_reader_task != NULL) {
    // 任务没能按时退出。**不要**重置 s_stop_req —— 保持它让任务尽快自尽,
    // 否则残留任务会污染下一次播放。宁可让本次停止变慢, 也不能让状态错乱。
    // (同 web_radio.c:1852。)
    ESP_LOGE(TAG, "任务未退出 (play=%p reader=%p), 保持停止标志",
             (void *)s_play_task, (void *)s_reader_task);
    return ESP_ERR_TIMEOUT;
  }

  ring_reset();
  s_stop_req = false; // 两个任务都确认没了, 这时才安全
  s_skip_req = false;
  s_skip_delta = 0;
  s_reader_abort = false;
  s_reader_failed = false;
  s_reader_running = false;
  s_state = SD_PLAYER_IDLE;
  s_dec_sample_rate = 0;
  s_dec_channels = 0;
  s_error[0] = '\0';
  memset(&s_ti, 0, sizeof(s_ti));

  playlist_free();

  // ⚠️⚠️ 把 I2S **完整地**交还给 AirPlay —— 这一步不能省。
  //    播放期间按文件原生采样率改过 I2S, 并停掉了 AirPlay 的 playback_task。
  //    不还原的话, 播完 SD 歌再投 AirPlay 会"歌在播放但完全没声音", 且只有
  //    复位设备才能恢复 (只有重启才会重走 start_airplay_services ->
  //    audio_output_start)。见 sd_player.h 里 resume_airplay 的说明。
  //
  //    ⚠️ 只在 yield_to_airplay 为真时做。sd_player_play() 内部也会调本函数
  //       (先停旧的再播新的), 那条路径**不能**拉起 playback_task —— 它读不到
  //       数据就会灌静音, 和我们的 PCM 抢同一个 I2S DMA。所以两条路径分开:
  //         sd_player_stop()            — 内部停止, 不碰 AirPlay
  //         sd_player_stop_and_yield()  — 内部停止 + 交还 I2S
  if (yield_to_airplay && s_hooks_ready && s_hooks.resume_airplay) {
    s_hooks.resume_airplay();
    ESP_LOGI(TAG, "已停止, I2S 已交还 AirPlay (采样率已还原, playback_task 已重启)");
  } else {
    ESP_LOGI(TAG, "已停止");
  }
  return ESP_OK;
}

esp_err_t sd_player_play(const char *path) {
  if (!s_hooks_ready) {
    set_error("音频输出未就绪 (main 未注册钩子)");
    return ESP_ERR_INVALID_STATE;
  }
  if (!path || path[0] == '\0') {
    return ESP_ERR_INVALID_ARG;
  }
  if (sd_card_path_check(path) != ESP_OK) {
    set_error("路径非法");
    return ESP_ERR_INVALID_ARG;
  }
  if (!sd_card_is_mounted()) {
    set_error("SD 卡未挂载");
    return ESP_ERR_INVALID_STATE;
  }

  // 先停干净 (幂等)。必须确认真的停干净了 —— 否则旧任务还活着, 会把上一曲的
  // 数据灌进环形缓冲, 新会话读到就解码失败。
  esp_err_t serr = sd_player_stop();
  if (serr != ESP_OK) {
    set_error("上一个任务未能停止, 请稍后重试");
    return serr;
  }

  s_error[0] = '\0';

  // 判断是目录还是文件
  char dir[SD_CARD_PATH_MAX];
  const char *select = NULL;

  char vfs[SD_VFS_MAX];
  esp_err_t perr = rel_to_vfs(path, vfs, sizeof(vfs));
  if (perr != ESP_OK) {
    set_error("路径非法");
    return perr;
  }

  struct stat st;
  if (stat(vfs, &st) != 0) {
    set_error("文件或目录不存在");
    return ESP_ERR_NOT_FOUND;
  }

  if (S_ISDIR(st.st_mode)) {
    strlcpy(dir, path, sizeof(dir));
  } else {
    // 单曲: 播放列表取**所在目录**, 并把 index 定位到这一首。
    // 这样点单曲时「上一条/下一条」也是通的 —— 符合用户预期。
    strlcpy(dir, path, sizeof(dir));
    char *slash = strrchr(dir, '/');
    if (!slash) {
      return ESP_ERR_INVALID_ARG;
    }
    if (slash == dir) {
      dir[1] = '\0'; // 根目录下的文件 -> dir 变成 "/"
    } else {
      *slash = '\0';
    }
    select = path;
  }

  esp_err_t berr = playlist_build(dir, select);
  if (berr != ESP_OK) {
    set_error(berr == ESP_ERR_NOT_FOUND
                  ? "这个目录里没有可播放的音频文件"
                  : (berr == ESP_ERR_NO_MEM ? "内存不足" : "目录打不开"));
    playlist_free();
    return berr;
  }
  if (!select) {
    s_pl_idx = 0;
  }

  s_state = SD_PLAYER_PLAYING;
  s_stop_req = false;
  s_skip_req = false;
  s_skip_delta = 0;
  s_reader_abort = false;
  s_reader_failed = false;
  ring_reset();

  // 先把第 0 曲的对外信息准备好, 这样网页在 play_task 起来之前就能显示正确的
  // "第 1/N 曲"。play_task 里的 track_begin 会再算一次 (幂等)。
  track_begin(s_pl_idx);

  // 从 AirPlay 手里接管 I2S。output_stop 内部最多等 2 秒。
  ESP_LOGI(TAG, "接管 I2S (暂停 AirPlay playback task)");
  s_hooks.output_stop();

  int start = s_pl_idx;
  if (xTaskCreatePinnedToCore(play_task, "sd_play", SD_TASK_STACK,
                              (void *)(intptr_t)start, SD_TASK_PRIO,
                              &s_play_task, SD_TASK_CORE) != pdPASS) {
    s_stop_req = true;
    set_error("播放任务创建失败");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "开始播放: %s (%d/%d)", s_pl[start], start + 1, s_pl_count);
  return ESP_OK;
}

void sd_player_skip(int delta) {
  if (delta != 1 && delta != -1) {
    return;
  }
  if (!sd_player_is_active() || s_pl_count <= 1 || s_play_task == NULL) {
    return; // 没在播 / 单曲 / 还没起来, 忽略
  }

  s_skip_delta = delta;
  s_skip_req = true;

  // ⚠️ 必须**主动掐断**当前读取, 否则 play_task 要等这一曲放完才看得到标志 ——
  //    一首歌 3 分钟, 按钮就等于没反应。
  //    文件读没有 web_radio 那套 socket shutdown 的麻烦 (那边的说明见
  //    web_radio.c:2041): fread 一次最多几十毫秒, 置个标志就够了。
  s_reader_abort = true;

  ESP_LOGI(TAG, "切曲请求: %+d (当前第 %d/%d 曲)", delta, s_pl_idx + 1,
           s_pl_count);
}

bool sd_player_is_active(void) { return s_state == SD_PLAYER_PLAYING; }

sd_player_state_t sd_player_get_state(void) { return s_state; }

void sd_player_get_track_info(sd_player_track_info_t *out) {
  if (!out) {
    return;
  }
  // 整体拷贝。s_ti 只在 play_task 里写 (曲目边界 + 每秒更新已播时长), 这里的读
  // 可能撞上写, 但最坏情况是某几个字段差半拍 —— 网页 2 秒后轮询就纠正了,
  // 不值得为此上锁。(同 web_radio.c:2020。)
  *out = s_ti;
}

void sd_player_get_error(char *buf, size_t buf_len) {
  if (buf && buf_len > 0) {
    strlcpy(buf, s_error, buf_len);
  }
}

void sd_player_yield_to_airplay(void) {
  // ⚠️⚠️ **不能**加 `if (sd_player_is_active())` 守卫 —— 这正是 web_radio 原
  //    bug 的根源: 用户的场景是"歌**播完**之后投 AirPlay", 播完时 play_task 自己
  //    把状态置成 IDLE 就退出了, 根本没走 stop(), 所以 I2S 采样率和
  //    playback_task 都还停在"播放器占着"的状态, 而 is_active() 此时是 false。
  //    加守卫的话这个回调什么都不做, AirPlay 推流没人写 I2S, 表现就是"歌在播放
  //    但没声音", 只有重启设备才好。(见 web_radio.c:2068。)
  //
  //    stop_and_yield 是幂等的: 没有任务在跑时它只做"交还 I2S"这一步。
  ESP_LOGI(TAG, "AirPlay 会话开始, SD 播放器让出 I2S");
  sd_player_stop_and_yield();
}
