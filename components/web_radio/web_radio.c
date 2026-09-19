/*
 * Web 电台 — HTTP 音乐流播放 (m3u 歌单 / 单流 URL)
 *
 * 数据流:
 *   HTTP(TCP) ──event cb──> PSRAM 环形缓冲 ──play task──> MP3 解码
 *                                                        ──> audio_output_write ──> I2S
 *
 * 设计要点 (都是踩过的坑, 说明写在这里免得以后忘):
 *
 * 1. 环形缓冲放 **PSRAM**。参考 memory「ESP-IDF 音频流三大坑」之一:
 *    给 WiFi/HTTP 配大缓冲会把内部 RAM 撑爆, 导致 httpd 启动失败。
 *    S3 有 8MB PSRAM, 缓冲放那边, 内部 RAM 只留给 httpd 和 lwIP。
 *
 * 2. I2S 接管用现成的配对接口:
 *      audio_output_stop()   — 停掉 AirPlay 的 playback_task, 让出 DMA
 *      audio_output_write()  — 往 DMA 直写 PCM
 *    ⚠️ **不要**调 audio_output_start()。它启动的是 AirPlay 的 playback_task,
 *    那个任务从 audio_receiver_read() 取数据 (web radio 不走这条路), 读不到
 *    就每 ~8ms 灌一帧静音, 会和我们的 PCM 抢同一个 I2S DMA, 结果是爆音。
 *    A2DP (同样是外部音源, main/audio/a2dp_sink.c) 的模式就是: 只 write,
 *    不 start —— 前提是 playback_task 已被 audio_output_stop() 停掉。
 *
 * 3. 用 esp_audio_simple_dec 而不是裸解码器。它的注释明确写了
 *    "support input data of any size", 内部自带帧同步/容器解析。自己从
 *    TCP/HTTP 字节流里切帧边界是最容易出错的地方, 交给它省一个坑。
 *
 * 4. ⚠️⚠️ **输入游标必须手动推进** (raw.len -= raw.consumed)。
 *    这是本项目最隐蔽的一个 bug, 详细说明在 play_task 里 —— 症状是
 *    "所有计数器都健康, 但 PCM 是间断的", 听感就是持续的糊/爆。
 *    一句话: 一次 process() 消费不完你喂进去的字节, 没消费的尾巴必须
 *    原样再喂, 且**不能**在它没消费完时覆盖那块内存。
 *
 * 5. ⚠️ simple dec 的解码器必须**手动注册**。esp_audio_simple_dec_open
 *    不会自动找实现 —— 没注册就返回 -7 (NOT_SUPPORT)。
 *      · esp_audio_simple_dec_register_default() 只覆盖 Kconfig 里有开关的
 *        四个 (WAV/M4A/TS/OGG), **不含 MP3/FLAC**。
 *      · MP3 / FLAC 必须自己调 esp_audio_simple_dec_register(), parser 用
 *        库里的 esp_mp3_dec_parse_frame / esp_flac_dec_parse_frame。
 *      · 这两个 parser 函数在 .a 里是弱符号, 且 mp3 没导出头文件, 所以
 *        头文件里自己补了声明 (见下面)。
 *    (这就是 memory 里"预编译库必须手动注册解码器"那条坑, 换组件又撞一次。)
 */

#include "web_radio.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "esp_heap_caps.h"

#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_reg.h"

#include "decoder/impl/esp_mp3_dec.h"
#include "decoder/impl/esp_flac_dec.h"

// esp_mp3_dec_parse_frame / esp_flac_dec_parse_frame 在 .a 里 (弱符号),
// 但这个版本的组件**没有导出对应的头文件** (只有 m4a/ogg/ts/wav 有
// esp_xxx_parse.h, MP3/FLAC 都没有)。所以这里自己声明。
// 签名必须和 esp_es_parse_func_t 一致:
//   esp_es_parse_err_t (*)(esp_es_parse_raw_t *, esp_es_parse_frame_info_t *)
// 如果哪天上游导出了对应的 parse 头文件, 把这段删掉换成 #include。
esp_es_parse_err_t esp_mp3_dec_parse_frame(esp_es_parse_raw_t *data,
                                           esp_es_parse_frame_info_t *info);
esp_es_parse_err_t esp_flac_dec_parse_frame(esp_es_parse_raw_t *data,
                                            esp_es_parse_frame_info_t *info);

static const char *TAG = "web_radio";

// 音频输出通过 main 注册进来的函数指针调用 —— 见 web_radio.h 的
// "依赖方向" 说明。不直接 include audio_output.h。
static web_radio_output_hooks_t s_hooks = {0};
static bool s_hooks_ready = false;

void web_radio_set_output_hooks(const web_radio_output_hooks_t *hooks) {
  if (!hooks || !hooks->output_stop || !hooks->output_write ||
      !hooks->output_set_rate || !hooks->output_set_source_rate ||
      !hooks->output_is_active || !hooks->get_volume_q15 ||
      !hooks->abort_http || !hooks->resume_airplay) {
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
// 可调参数
// ---------------------------------------------------------------------------

// 环形缓冲大小。放 PSRAM, 大点无所谓: 256KB ≈ 128kbps 流缓冲 16 秒,
// 足够扛住 WiFi 抖动 (参考项目里 AirPlay 用 2 秒深缓冲就能压住抖动)。
#define RING_SIZE_BYTES (256 * 1024)

// HTTP 读缓冲。这个决定单次 TCP 读取量, 也影响 TCB 窗口。
#define HTTP_BUF_SIZE 4096

// 解码输出缓冲初始大小。FLAC 的块最大 4096 样本, 立体声 16bit 就是 16KB,
// MP3 一帧 1152 样本只要 4.6KB。给 64KB 一次到位, 避免运行时扩容。
// (仍保留按 needed_size 扩容的路径, 见 play_task 里的 BUFF_NOT_ENOUGH 分支。)
#define PCM_OUT_BYTES (64 * 1024)

// 解码输入缓冲。⚠️ 这个缓冲的存在理由见 play_task 里的大段说明:
// simple dec 要求"喂进去但没消费完的字节不能被覆盖", 所以必须留一块
// 属于解码器输入侧的缓冲, 在里面按 consumed 推进游标。
// 放 PSRAM (它只被 memcpy 和 parser 读, 不进 DMA)。
// 64KB 是按最坏情况定的: FLAC 一个 block 压缩后约 20KB, 留 3 倍余量。
#define DEC_IN_BYTES (64 * 1024)

// 播放任务栈。解码 + 音量运算, 给足。
#define RADIO_TASK_STACK 6144

// ⚠️ 任务放置 (核 + 优先级) —— 这里必须小心, 否则会干扰 AirPlay。
//
// audio_output.c 的注释明确写了: playback_task 优先级 9 必须高于所有音源任务
// (realtime UDP rx = 8, control rx = 7, buffered reader = 5), 因为源任务
// 抢占它会让 DMA 环形缓冲跑空 (issue #122 的根因)。
//
// 同理, 我的任务**绝不能被放到优先级 9 及以上**。而 AirPlay 的 UDP 接收任务
// (audio_stream_realtime.c: "audio_recv") 跑在 **核 0 / 优先级 8**, 所以:
//   - 我的任务优先级取 6 → 低于 playback_task(9), 也低于 UDP rx(8)
//   - 但为了完全不给 AirPlay 添乱, HTTP+解码这两个吃 CPU 的任务放 **核 1**,
//     与 AirPlay 的接收链 (核 0) 分开。
#define RADIO_TASK_PRIO 6 // < AUDIO_PLAYBACK_TASK_PRIORITY(9)
#define HTTP_TASK_CORE  1 // 避开 AirPlay 接收任务的核 0
#define PLAY_TASK_CORE  1

// 网速掉到多少字节/秒以下算"卡了"(触发自愈重连)
#define STALL_BYTES_PER_SEC 128

// 播放任务栈/优先级
#define HTTP_TASK_STACK 6144

// ---------------------------------------------------------------------------
// 内部状态
// ---------------------------------------------------------------------------

typedef struct {
  uint8_t *buf;    // PSRAM
  size_t size;     // 总容量
  volatile size_t head; // 写入位置 (HTTP 任务)
  volatile size_t tail; // 读取位置 (播放任务)
  SemaphoreHandle_t mutex;
  EventGroupHandle_t ev;
} ring_t;

#define EV_STOP BIT0
#define EV_HTTP_DONE BIT1

static ring_t s_ring;

static TaskHandle_t s_play_task = NULL;
static TaskHandle_t s_http_task = NULL;

static volatile web_radio_state_t s_state = WEB_RADIO_IDLE;
static char s_url[256];
static char s_error[160];

static volatile bool s_stop_req = false; // 请求停止
static volatile bool s_http_running = false;

// 当前 HTTP client 句柄。播放任务在检测到下载停滞时用它主动关闭会话,
// 让 http_task 的 perform() 返回、自然结束, 而不是傻等。
// 只在 http_task 里创建/销毁, 用 volatile 指针保证可见性。
static esp_http_client_handle_t volatile s_http_client = NULL;

// 播放统计, 用于卡顿自愈
static volatile uint32_t s_bytes_downloaded = 0;
static volatile uint32_t s_underrun_count = 0;
static volatile size_t s_ring_high_water = 0; // 缓冲历史最高占用

// 解码器输出的实际格式 (从流里解出来, 不靠猜)
static uint32_t s_dec_sample_rate = 0;
static uint8_t s_dec_channels = 0;
static uint8_t s_dec_bits = 0;

// ---------------------------------------------------------------------------
// 环形缓冲
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

// 写。返回实际写入字节数。满了就少写。
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
  size_t used_now = ring_used();
  if (used_now > s_ring_high_water) {
    s_ring_high_water = used_now;
  }
  return len;
}

// 读。返回实际读出字节数。
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

// 判断 URL 是否有指定后缀 (如 ".flac")。忽略 query string (?...) 和
// fragment (#...), 因为 NAS 直链常带 ?token=... 之类。
static bool url_has_ext(const char *url, const char *ext) {
  if (!url || !ext) {
    return false;
  }
  size_t url_len = strlen(url);
  size_t ext_len = strlen(ext);
  if (ext_len == 0 || url_len < ext_len) {
    return false;
  }

  // 先把 query/fragment 截掉
  for (size_t i = 0; i < url_len; i++) {
    if (url[i] == '?' || url[i] == '#') {
      url_len = i;
      break;
    }
  }
  if (url_len < ext_len) {
    return false;
  }
  return strncasecmp(url + url_len - ext_len, ext, ext_len) == 0;
}

// 按扩展名挑容器类型。认不出来就按 MP3 试 —— 电台流大多没有后缀。
//
// ⚠️ 只按后缀判断是**不够**的: 服务器上的实际文件可能和路径后缀不符
// (或者干脆没后缀)。后缀猜错时解码器会返回 NOT_SUPPORT 并进 ERROR 状态,
// **不会静默放错音**, 所以这个简化是可接受的。
static esp_audio_simple_dec_type_t dec_type_from_url(const char *url) {
  if (url_has_ext(url, ".flac")) {
    return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
  }
  if (url_has_ext(url, ".wav")) {
    return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
  }
  if (url_has_ext(url, ".m4a") || url_has_ext(url, ".aac")) {
    return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
  }
  if (url_has_ext(url, ".ogg") || url_has_ext(url, ".oga")) {
    return ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
  }
  return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
}

static void ring_reset(void) {
  s_ring.head = 0;
  s_ring.tail = 0;
}

// ---------------------------------------------------------------------------
// 歌单 (m3u) + 曲目标签 (ID3v2 / Vorbis comment)
// ---------------------------------------------------------------------------

// 歌单条目上限。32 条 × (256 URL + 128 标题) ≈ 12KB, 放 PSRAM 无压力。
// 超出就截断, 并在状态里带出 truncated 标志 —— 不静默丢弃。
#define MAX_PLAYLIST 32
#define PLAYLIST_BYTES (512 * 1024) // 歌单文本读取上限
#define PLAYLIST_TIMEOUT_MS 8000

// 曲目标签预读: 读满这么多字节、或超过时间预算就主动中止。
// 实测素材: MP3 的整个 ID3v2.4 标签只有 107 字节; FLAC 因夹了 8KB padding,
// 标签在偏移 676、音频从 8916 才开始。8KB 对两者都够, 且能覆盖绝大多数
// 不带封面的文件 (带 300KB 专辑封面的文件会读不全, 那就回退到 EXTINF 标题)。
#define TAG_PROBE_BYTES (8 * 1024)
// ⚠️ 这是**墙钟预算**, 不是单次读超时。
//    实测 dcxx.cc:10086 上 16KB 要 ~10 秒才能读完, 期间每次读都在超时
//    之内, 所以只靠 esp_http_client 的 timeout_ms 根本拦不住 —— 每首歌
//    开头会静音十秒。这里在事件回调里掐总时间。
#define TAG_PROBE_BUDGET_MS 2500

typedef struct {
  char(*urls)[256];   // PSRAM, MAX_PLAYLIST 条
  char(*titles)[128]; // PSRAM, MAX_PLAYLIST 条
  int count;
  int index; // 当前播到第几曲 (0-based)
  bool truncated;
} playlist_t;

static playlist_t s_pl = {0};
static char s_cur_url[256];  // 当前曲真实地址 (歌单模式下 ≠ s_url)
static char s_pl_url[256];   // 歌单原始地址 (用户输入的那个)
static web_radio_track_info_t s_ti = {0};

// 曲间控制。web_radio_skip() 置位, play_task 在曲目边界消费。
static volatile bool s_skip_req = false;
static volatile int s_skip_delta = 0;
// 本曲是否失败。失败时**不自动跳下一曲** —— 否则网络断了会疯狂跳歌刷屏。
static volatile bool s_track_failed = false;
// 本曲是否被**主动掐断** (停止 / 切曲)。
//
// ⚠️ 必须和 s_track_failed 分开。掐断时 http_task 的 perform() 一定会返回
//    "INCOMPLETE_DATA" 之类的错误 (连接被我们 shutdown 了), 那是**预期**
//    结果, 不是网络故障。不区分的话每次点「下一曲」都会顺手把状态置成
//    出错, 表现为"歌切过去了但网页一直显示出错"。
static volatile bool s_abort_http = false;

// 下载任务的世代号。http_task 退出得慢 (perform 的退出延迟无法回避),
// 可能拖到下一次播放才死透 —— 靠这个区分"我还属不属于当前会话"。
static volatile uint32_t s_http_gen = 0;      // 当前生效的世代
static volatile uint32_t s_http_gen_next = 0; // 下一个要分配的

// ---- 一次性 HTTP GET 的小工具 -------------------------------------------
//
// 歌单拉取和标签预读都只需要"连上去读一小段然后主动断开", 与 http_task 的
// 长流语义完全不同 (那个写环形缓冲, 这个写普通内存并随时准备中止)。
// 所以这里写一个独立的小 helper, 自带 event handler, 复用它两次。

typedef struct {
  uint8_t *buf;
  size_t cap;
  size_t len;
  bool overflow; // 写满了
  bool abort;    // 够了, 主动中止
  // 墙钟预算 (0 = 不限)。超过就中止 —— 单靠 esp_http_client 的 timeout_ms
  // 拦不住"慢慢滴"的服务器: 每次读都在超时内, 但总耗时能到十几秒。
  int64_t deadline_us; // esp_timer_get_time() 的绝对值; 0 = 不检查
  uint32_t budget_ms;  // 墙钟预算, 0 = 不限
} fetch_ctx_t;

static esp_err_t fetch_event_handler(esp_http_client_event_t *evt) {
  fetch_ctx_t *c = (fetch_ctx_t *)evt->user_data;
  if (!c) {
    return ESP_OK;
  }

  switch (evt->event_id) {
  case HTTP_EVENT_ON_CONNECTED:
    // 连接建立的那一刻起算预算 (不含 DNS/连接握手, 那部分由 timeout_ms 管)
    if (c->deadline_us == 0 && c->budget_ms > 0) {
      c->deadline_us = esp_timer_get_time() + (int64_t)c->budget_ms * 1000;
    }
    return ESP_OK;

  case HTTP_EVENT_ON_DATA:
    if (!evt->data || evt->data_len <= 0) {
      return ESP_OK;
    }
    if (c->deadline_us && esp_timer_get_time() > c->deadline_us) {
      c->abort = true; // 预算用完, 不管读到多少都用这些
      return ESP_FAIL;
    }
    if (c->len + (size_t)evt->data_len > c->cap) {
      c->overflow = true;
      c->abort = true;
      return ESP_FAIL; // 非 OK 会让 perform() 立刻返回
    }
    memcpy(c->buf + c->len, evt->data, (size_t)evt->data_len);
    c->len += (size_t)evt->data_len;
    if (c->len >= c->cap) {
      c->abort = true;
      return ESP_FAIL;
    }
    return ESP_OK;

  default:
    return ESP_OK;
  }
}

// 拉一小段。成功 (或"读够了/超预算主动中止") 返回 ESP_OK, buf/len 有效。
// auto_redirect 打开 —— 歌单和音频都可能 302 跳转。
//
// @param budget_ms 墙钟预算 (毫秒)。从**连接建立**起算, 超了就中止并返回
//                  已读到的部分。0 = 不限 (只受 timeout_ms 约束)。
static esp_err_t fetch_small(const char *url, uint8_t *buf, size_t cap,
                             size_t *out_len, uint32_t timeout_ms,
                             uint32_t budget_ms) {
  if (!url || !buf || cap == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  fetch_ctx_t ctx = {.buf = buf, .cap = cap, .len = 0, .overflow = false,
                     .abort = false, .deadline_us = 0,
                     .budget_ms = budget_ms};

  esp_http_client_config_t cfg = {
      .url = url,
      .event_handler = fetch_event_handler,
      .user_data = &ctx,
      .buffer_size = HTTP_BUF_SIZE,
      .timeout_ms = (int)timeout_ms,
      .keep_alive_enable = false,
      .disable_auto_redirect = false,
      // ⚠️ https:// 是明确支持的 (见 play_url 的协议白名单)，可这里原先一个 CA
      //    都没配 —— 没有信任锚，握手必定失败，报出来的错还很难看懂。用 IDF 自带
      //    的根证书包 (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE 已开)。
      //    **别**改成 ESP_TLS_INSECURE / skip_server_cert_verify：那会从"连不上"
      //    变成"静默可被中间人"。
      .crt_bundle_attach = esp_crt_bundle_attach,
  };

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) {
    return ESP_FAIL;
  }
  esp_http_client_set_header(client, "User-Agent", "ESP32-WebRadio/1.0");

  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);

  // 主动 abort 会让 perform 返回非 OK, 那是**预期**的, 不算失败。
  if (ctx.abort) {
    err = ESP_OK;
  }
  if (err == ESP_OK && status != 0 && status != 200) {
    err = ESP_ERR_INVALID_RESPONSE;
  }

  esp_http_client_cleanup(client);

  if (err != ESP_OK) {
    return err;
  }

  // NUL 结尾: 歌单解析按字符串走, 多留一个字节。
  // (调用方保证 cap 有余量; 满了就写到最后一位)
  if (ctx.len < cap) {
    buf[ctx.len] = '\0';
  } else {
    buf[cap - 1] = '\0';
  }
  if (out_len) {
    *out_len = ctx.len;
  }
  return ESP_OK;
}

// ---- 标签解析 -----------------------------------------------------------

// 拷贝并截断, 顺便去掉首尾空白和 UTF-8 BOM。
static void copy_trim(char *dst, size_t dst_len, const char *src,
                      size_t src_len) {
  if (!dst || dst_len == 0) {
    return;
  }
  dst[0] = '\0';
  if (!src || src_len == 0) {
    return;
  }

  // UTF-8 BOM (EF BB BF) —— 有些 Windows 工具写的 m3u 会带
  if (src_len >= 3 && (uint8_t)src[0] == 0xEF && (uint8_t)src[1] == 0xBB &&
      (uint8_t)src[2] == 0xBF) {
    src += 3;
    src_len -= 3;
  }

  while (src_len > 0 && (src[0] == ' ' || src[0] == '\t')) {
    src++;
    src_len--;
  }
  while (src_len > 0 && (src[src_len - 1] == ' ' || src[src_len - 1] == '\t' ||
                         src[src_len - 1] == '\r' || src[src_len - 1] == '\n')) {
    src_len--;
  }

  if (src_len >= dst_len) {
    src_len = dst_len - 1;
  }
  memcpy(dst, src, src_len);
  dst[src_len] = '\0';
}

// ID3v2 的帧长是 **syncsafe** 整数: 每字节只用低 7 位 (v2.4 规范)。
// 按普通大端解会把长度算错, 后面全乱。
static uint32_t syncsafe32(const uint8_t *p) {
  return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
         ((uint32_t)(p[2] & 0x7F) << 7) | (uint32_t)(p[3] & 0x7F);
}

// 解析 ID3v2 文本帧。只认 UTF-8 (编码字节 0x03); UTF-16 会被跳过 ——
// 国内老文件 v2.3 用 UTF-16 的不少, 那种情况回退到 #EXTINF 标题。
//
// 实测素材: ID3v2.4.0, 整个标签 107 字节, TIT2/TPE1/TALB 都是 UTF-8。
static void tag_parse_id3v2(const uint8_t *buf, size_t len, char *title,
                            size_t title_len, char *artist, size_t artist_len,
                            char *album, size_t album_len) {
  if (len < 10 || memcmp(buf, "ID3", 3) != 0) {
    return;
  }
  // buf[3] = 主版本, buf[4] = 次版本; buf[5] = flags
  if (buf[5] & 0x40) {
    return; // 有扩展头, 布局复杂, 放弃 (回退到 EXTINF)
  }

  uint32_t tag_size = syncsafe32(buf + 6);
  size_t end = 10 + tag_size;
  if (end > len) {
    end = len; // 只读到了部分标签, 尽力而为
  }

  size_t pos = 10;
  while (pos + 10 <= end) {
    const uint8_t *hdr = buf + pos;
    // 填到 padding 区了 (全 0)
    if (hdr[0] == 0) {
      break;
    }

    char id[5] = {(char)hdr[0], (char)hdr[1], (char)hdr[2], (char)hdr[3], 0};
    uint32_t fsize = syncsafe32(hdr + 4);
    if (fsize == 0 || pos + 10 + fsize > end) {
      break;
    }

    const uint8_t *body = buf + pos + 10;
    // 正文第一个字节是编码: 0x00=Latin1, 0x01=UTF-16+BOM, 0x02=UTF-16BE,
    // 0x03=UTF-8。只处理 UTF-8。
    if (fsize >= 2 && body[0] == 0x03 &&
        (strcmp(id, "TIT2") == 0 || strcmp(id, "TPE1") == 0 ||
         strcmp(id, "TALB") == 0)) {
      const char *text = (const char *)body + 1;
      size_t text_len = fsize - 1;
      if (strcmp(id, "TIT2") == 0) {
        copy_trim(title, title_len, text, text_len);
      } else if (strcmp(id, "TPE1") == 0) {
        copy_trim(artist, artist_len, text, text_len);
      } else {
        copy_trim(album, album_len, text, text_len);
      }
    }

    pos += 10 + fsize;
  }
}

// 解析 FLAC 的 VORBIS_COMMENT 块 (块类型 4)。
// ⚠️ 标签**不在文件开头**: 前面有 STREAMINFO(34B) 和可能的 SEEKTABLE/PADDING。
//    实测素材 VORBIS_COMMENT 在偏移 676, 音频要 8916 才开始 —— 所以必须
//    按元数据块链一路走, 不能假设"标签就在最前面"。
static void tag_parse_vorbis(const uint8_t *buf, size_t len, char *title,
                             size_t title_len, char *artist, size_t artist_len,
                             char *album, size_t album_len) {
  if (len < 4 || memcmp(buf, "fLaC", 4) != 0) {
    return;
  }

  size_t pos = 4;
  while (pos + 4 <= len) {
    uint8_t hdr = buf[pos];
    bool last = (hdr & 0x80) != 0;
    uint8_t btype = hdr & 0x7F;
    uint32_t blen = ((uint32_t)buf[pos + 1] << 16) |
                    ((uint32_t)buf[pos + 2] << 8) | (uint32_t)buf[pos + 3];
    pos += 4;

    if (btype == 4) { // VORBIS_COMMENT
      if (pos + blen > len) {
        return; // 还没读全 (预读窗口不够)
      }
      const uint8_t *p = buf + pos;
      size_t remain = blen;

      // 先跳过 vendor string (小端 4 字节长度 + 内容)
      if (remain < 4) {
        return;
      }
      uint32_t vlen = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                      ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
      p += 4;
      remain -= 4;
      if (vlen > remain) {
        return;
      }
      p += vlen;
      remain -= vlen;

      // 再逐条读 comment: 小端 4 字节长度 + "KEY=VALUE"
      if (remain < 4) {
        return;
      }
      uint32_t ncomments = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
      p += 4;
      remain -= 4;

      for (uint32_t i = 0; i < ncomments && remain >= 4; i++) {
        uint32_t clen = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        p += 4;
        remain -= 4;
        if (clen > remain) {
          break;
        }

        const char *kv = (const char *)p;
        const char *eq = memchr(kv, '=', clen);
        if (eq) {
          size_t klen = (size_t)(eq - kv);
          const char *val = eq + 1;
          size_t vlen2 = clen - klen - 1;

          if (klen == 5 && strncasecmp(kv, "TITLE", 5) == 0) {
            copy_trim(title, title_len, val, vlen2);
          } else if (klen == 6 && strncasecmp(kv, "ARTIST", 6) == 0) {
            copy_trim(artist, artist_len, val, vlen2);
          } else if (klen == 5 && strncasecmp(kv, "ALBUM", 5) == 0) {
            copy_trim(album, album_len, val, vlen2);
          }
        }

        p += clen;
        remain -= clen;
      }
      return; // 找到 VORBIS_COMMENT 就够, 后面的块不用看
    }

    if (last) {
      return; // 元数据块链到头了, 没找到 VORBIS_COMMENT
    }
    if (pos + blen > len) {
      return; // 超出预读窗口
    }
    pos += blen;
  }
}

// 回退链最后一环: 从 URL 猜标题。
// 取最后一段路径 → 去掉 query/fragment → URL 解码 (%XX → 字节) → 去扩展名。
//
// ⚠️ 这里的解码只用于**显示**, 解出来的字符串绝不拿去做请求 ——
//    实测素材的 URL 是 %2520 双重编码, 解码后会变成 %20 而 404。
//    请求一律用原样的 URL。
static void title_from_url(const char *url, char *out, size_t out_len) {
  if (!out || out_len == 0) {
    return;
  }
  out[0] = '\0';
  if (!url) {
    return;
  }

  // 取最后一段路径 (跳开 scheme:// 里的斜杠)
  const char *p = url;
  const char *scheme = strstr(p, "://");
  if (scheme) {
    p = scheme + 3;
  }
  const char *last = strrchr(p, '/');
  if (last) {
    p = last + 1;
  }

  // 截掉 query / fragment
  char seg[192];
  copy_trim(seg, sizeof(seg), p, strlen(p));

  // 去扩展名
  char *dot = strrchr(seg, '.');
  if (dot && dot != seg) {
    *dot = '\0';
  }
  if (seg[0] == '\0') {
    strlcpy(out, "未知曲目", out_len);
    return;
  }

  // URL 解码 (显示专用)。做**两轮**:
  // 有些服务器把文件名里的空格编码成 %2520 (即文件名本身含 "%20"),
  // 一轮解码后还剩一个 %20 看着很难受。两轮之后绝大多数情况就正常了。
  // ⚠️ 这只影响显示。请求用的 URL 在别处, 逐字照用、绝不解码
  //    (解了会把 %25 变成 % 直接 404)。
  for (int pass = 0; pass < 2; pass++) {
    char in[192];
    strlcpy(in, out[0] ? out : seg, sizeof(in));
    // 第一轮从 seg 解, 第二轮从上一轮结果解
    const char *src = (pass == 0) ? seg : in;
    size_t oi = 0;
    for (size_t i = 0; src[i] && oi + 1 < out_len; i++) {
      if (src[i] == '%' && isxdigit((unsigned char)src[i + 1]) &&
          isxdigit((unsigned char)src[i + 2])) {
        char hex[3] = {src[i + 1], src[i + 2], 0};
        out[oi++] = (char)strtol(hex, NULL, 16);
        i += 2;
      } else if (src[i] == '+') {
        out[oi++] = ' ';
      } else {
        out[oi++] = src[i];
      }
    }
    out[oi] = '\0';
    if (strchr(out, '%') == NULL) {
      break; // 没有残留的 % , 不用再解一轮
    }
  }
}

// 拉取并解析歌单。
//
// ⚠️ 识别方式: 只按 **URL 后缀** (.m3u/.m3u8, 比较前剥掉 ?query), 不做内容嗅探。
//    嗅探会给每一次普通播放都加一次探测成本, 且要在 http_task 中途切模式,
//    风险高得多。代价是"无后缀但返回歌单"的地址识别不了。
static esp_err_t playlist_load(const char *url) {
  uint8_t *buf = heap_caps_malloc(PLAYLIST_BYTES, MALLOC_CAP_SPIRAM);
  if (!buf) {
    return ESP_ERR_NO_MEM;
  }

  size_t len = 0;
  esp_err_t err = fetch_small(url, buf, PLAYLIST_BYTES - 1, &len,
                              PLAYLIST_TIMEOUT_MS, 0);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "歌单拉取失败: %s", esp_err_to_name(err));
    heap_caps_free(buf);
    return err;
  }

  // 相对路径要用歌单地址的 scheme://host[:port] 拼。
  // 302 跳转过的话, 原始 url 可能已经不是最终地址了 —— 但 fetch_small
  // 内部没把最终地址带出来, 这里用调用方给的 url 兜底 (常见情况够用)。
  char base[256];
  base[0] = '\0';
  {
    const char *sp = strstr(url, "://");
    if (sp) {
      const char *slash = strchr(sp + 3, '/');
      size_t n = slash ? (size_t)(slash - url) : strlen(url);
      if (n < sizeof(base)) {
        memcpy(base, url, n);
        base[n] = '\0';
      }
    }
  }

  s_pl.count = 0;
  s_pl.truncated = false;

  char pending_title[128];
  pending_title[0] = '\0';

  char *line = (char *)buf;
  size_t remain = len;
  bool overflow = false;

  while (remain > 0) {
    // 切出一行
    char *nl = memchr(line, '\n', remain);
    size_t line_len = nl ? (size_t)(nl - line) : remain;
    remain -= line_len + (nl ? 1 : 0);

    char cur[384];
    copy_trim(cur, sizeof(cur), line, line_len);
    line = nl ? nl + 1 : line + line_len;

    if (cur[0] == '\0') {
      continue;
    }

    if (strncmp(cur, "#EXTINF:", 8) == 0) {
      // "#EXTINF:-1,标题" —— 取**第一个逗号之后**的内容
      char *comma = strchr(cur + 8, ',');
      if (comma) {
        copy_trim(pending_title, sizeof(pending_title), comma + 1,
                  strlen(comma + 1));
      }
      continue;
    }
    if (cur[0] == '#') {
      continue; // #EXTM3U 等其他指令
    }

    // 非 # 开头的非空行 = 一个条目地址
    if (s_pl.count >= MAX_PLAYLIST) {
      s_pl.truncated = true;
      overflow = true;
      break;
    }

    const char *entry = cur;
    char abs[256];
    if (strncmp(entry, "http://", 7) != 0 &&
        strncmp(entry, "https://", 8) != 0) {
      // 相对路径 → 拼上歌单的 scheme://host[:port]
      // ⚠️ 用 "base + [/] + entry" 拼接。entry 最长 383 字节, 拼起来可能
      //    超过 256 —— snprintf 会安全截断, 截断后的 URL 请求会失败并在
      //    下面被跳过, 不会越界。
      if (base[0] == '\0') {
        pending_title[0] = '\0';
        continue; // 拼不出来, 跳过这条
      }
      int n = snprintf(abs, sizeof(abs), "%s%s%s", base,
                       entry[0] == '/' ? "" : "/", entry);
      if (n <= 0 || (size_t)n >= sizeof(abs)) {
        ESP_LOGW(TAG, "条目拼接后过长, 跳过");
        pending_title[0] = '\0';
        continue;
      }
      entry = abs;
    }

    // ⚠️ URL **逐字照用**, 不做解码 —— 见 title_from_url 的说明。
    strlcpy(s_pl.urls[s_pl.count], entry, 256);
    strlcpy(s_pl.titles[s_pl.count], pending_title, 128);
    s_pl.count++;
    pending_title[0] = '\0';
  }

  heap_caps_free(buf);

  if (s_pl.count == 0) {
    ESP_LOGW(TAG, "歌单里没找到任何条目");
    return ESP_ERR_NOT_FOUND;
  }
  if (overflow) {
    ESP_LOGW(TAG, "歌单超过 %d 条, 只取前 %d 条", MAX_PLAYLIST, MAX_PLAYLIST);
  }

  ESP_LOGI(TAG, "拉取歌单: %d 条%s", s_pl.count,
           s_pl.truncated ? " (已截断)" : "");
  for (int i = 0; i < s_pl.count; i++) {
    ESP_LOGI(TAG, "  第%d条: %s", i + 1,
             s_pl.titles[i][0] ? s_pl.titles[i] : "(无标题)");
  }
  return ESP_OK;
}
// 预读当前曲目的文件头, 填 s_ti 的 title/artist/album。
// 失败**不影响播放** —— 静默回退, 只是少显示点信息。
static void track_probe_tags(int idx) {
  // 先按回退链铺好底: EXTINF 标题 > URL 文件名 > "未知曲目"
  const char *u = s_pl.urls[idx];
  if (s_pl.titles[idx][0]) {
    strlcpy(s_ti.title, s_pl.titles[idx], sizeof(s_ti.title));
  } else {
    title_from_url(u, s_ti.title, sizeof(s_ti.title));
  }
  s_ti.artist[0] = '\0';
  s_ti.album[0] = '\0';

  uint8_t *buf = heap_caps_malloc(TAG_PROBE_BYTES, MALLOC_CAP_SPIRAM);
  if (!buf) {
    return;
  }

  size_t len = 0;
  esp_err_t err = fetch_small(u, buf, TAG_PROBE_BYTES, &len,
                              TAG_PROBE_BUDGET_MS + 1500,
                              TAG_PROBE_BUDGET_MS);
  // ⚠️ 超预算中止时 fetch_small 返回的是 ESP_OK (len = 已读到的部分) ——
  //    所以**不能**用 err 判断"读到了没有", 要用 len。
  //    ID3 只有 107 字节、Vorbis comment 在 676, 部分数据通常也够解析。
  if (len < 4) {
    ESP_LOGI(TAG, "无内嵌标签 (预读没读到东西), 用回退标题: %s", s_ti.title);
    heap_caps_free(buf);
    return;
  }
  if (err != ESP_OK) {
    ESP_LOGI(TAG, "标签预读失败 (%s), 用回退标题: %s", esp_err_to_name(err),
             s_ti.title);
    heap_caps_free(buf);
    return;
  }
  ESP_LOGD(TAG, "标签预读 %u 字节", (unsigned)len);

  char t[128] = {0}, a[64] = {0}, al[64] = {0};

  if (memcmp(buf, "ID3", 3) == 0) {
    tag_parse_id3v2(buf, len, t, sizeof(t), a, sizeof(a), al, sizeof(al));
  } else if (memcmp(buf, "fLaC", 4) == 0) {
    tag_parse_vorbis(buf, len, t, sizeof(t), a, sizeof(a), al, sizeof(al));
  }
  // 其余情况 (裸 MPEG 同步字开头等) = 没有标签, 保持回退值

  if (t[0]) {
    strlcpy(s_ti.title, t, sizeof(s_ti.title));
  }
  if (a[0]) {
    strlcpy(s_ti.artist, a, sizeof(s_ti.artist));
  }
  if (al[0]) {
    strlcpy(s_ti.album, al, sizeof(s_ti.album));
  }

  ESP_LOGI(TAG, "标签: 标题=%s 作者=%s 专辑=%s", s_ti.title,
           s_ti.artist[0] ? s_ti.artist : "-",
           s_ti.album[0] ? s_ti.album : "-");

  heap_caps_free(buf);
}

// ---- 曲目上下文 ---------------------------------------------------------
//
// 单曲模式和歌单模式统一走下面两个函数, 这样 play_task 里不用到处判分支。
// 单曲模式就是"长度为 1 的歌单": s_single_url 那条。

// 前置声明 —— 这两个的定义在文件更靠后的位置 (HTTP 接收 / 状态那两节)。
static esp_err_t http_event_handler(esp_http_client_event_t *evt);
static void http_task(void *arg);
static void set_error(const char *msg);

static char s_single_url[256];

static int pl_total(void) {
  if (s_pl.count > 0) {
    return s_pl.count;
  }
  return s_single_url[0] ? 1 : 0;
}

// 取第 idx 曲的地址。单曲模式下 idx 恒为 0。
static const char *pl_url_at(int idx) {
  if (s_pl.count > 0) {
    if (idx < 0 || idx >= s_pl.count) {
      return NULL;
    }
    return s_pl.urls[idx];
  }
  return idx == 0 ? s_single_url : NULL;
}

// 进入第 idx 曲: 更新对外可见的曲目信息 + 重置本曲的播放统计。
//
// ⚠️ 跨曲必须重置这些全局量 —— 它们都是**单份**的, 不清就会带着上一曲
//    的值跑 (诊断行数字飘、采样率不更新、停滞检测一上来就误判)。
static void track_begin(int idx) {
  s_ti.index = (s_pl.count > 0) ? (idx + 1) : 0;
  s_ti.count = (s_pl.count > 0) ? s_pl.count : ((s_single_url[0]) ? 1 : 0);
  s_ti.truncated = s_pl.truncated;

  const char *u = pl_url_at(idx);
  if (u) {
    strlcpy(s_cur_url, u, sizeof(s_cur_url));
    strlcpy(s_ti.cur_url, u, sizeof(s_ti.cur_url));
  }

  // 标题先用回退链铺底, 预读成功再覆盖 (单曲模式没有 EXTINF, 直接按 URL 猜)
  if (s_pl.count > 0 && s_pl.titles[idx][0]) {
    strlcpy(s_ti.title, s_pl.titles[idx], sizeof(s_ti.title));
  } else {
    title_from_url(s_cur_url, s_ti.title, sizeof(s_ti.title));
  }
  s_ti.artist[0] = '\0';
  s_ti.album[0] = '\0';

  // 本曲统计归零
  s_pl.index = idx;
  s_track_failed = false;
  s_abort_http = false; // 上一曲的掐断标记, 别带到这一曲
  s_bytes_downloaded = 0;
  s_dec_sample_rate = 0;
  s_dec_channels = 0;
  s_dec_bits = 0;
  s_underrun_count = 0;
  s_ring_high_water = 0;
}

// 掐断当前的 HTTP 下载。
//
// ⚠️⚠️ 必须用 abort, **不能**只用 close。
//    实测 (2026-09-17): 点「下一条」后 close() 已调用, 但 http_task 的
//    perform() 并没有立刻返回 —— 它卡在阻塞读上, 一直等到读超时 (~2 秒)
//    才因 "Incomlete data received, ret=-1" 退出。超过了我等它的窗口,
//    于是走到"为避免串曲就整个停掉"的分支, 表现就是**点了没反应**。
//    esp_http_client_abort() 会把内部状态标成已中止, perform() 立即返回。
//
// close 仍然要留着: 它负责把 socket 真正还回去。两个一起调才干净。
static void abort_current_http(void) {
  s_abort_http = true; // 让 http_task 知道"这是被掐的, 别报错"
  if (s_http_client && s_hooks_ready && s_hooks.abort_http) {
    s_hooks.abort_http((void *)s_http_client);
  }
  if (s_http_client) {
    esp_http_client_close(s_http_client);
  }
}

typedef struct {
  char *url;
  uint32_t gen;
} http_task_arg_t;

static void http_task(void *arg);

// 消费切曲请求并算出新的曲目序号。没有请求就原样返回 cur。
//
// ⚠️⚠️ 必须是**唯一**处理 s_skip_req 的地方。这个标志在两个位置会被看到
//    (for 循环顶部 / 本曲解码循环顶部), 谁先看到谁就调用它 —— 如果只清标志
//    而不算 cur, 后来那个位置就再也算不出来了。
//    实测踩过: 解码循环里清了标志去丢弃缓冲, 回到 for 顶部时标志已空,
//    cur 没变 —— 于是「下一曲」变成重播当前曲, 网页永远停在 1/2。
static int apply_skip(int cur) {
  if (!s_skip_req) {
    return cur;
  }
  int d = s_skip_delta;
  s_skip_req = false;
  s_skip_delta = 0;

  if (pl_total() <= 1) {
    return cur; // 单曲模式, 没有"下一曲"可言
  }
  // 手动「下一条」在最后一曲 → 环绕回第一曲;
  // 手动「上一条」在第一曲 → 回第一曲重播 (不绕到末尾)。
  int n = (d > 0) ? (cur + 1) % pl_total() : (cur > 0 ? cur - 1 : 0);
  ESP_LOGI(TAG, "切到第 %d 曲 (共 %d)", n + 1, pl_total());
  return n;
}

// 启动一次下载 (曲目边界调用)。单曲模式由 play_url 先启动过一次 —— 见那
// 里的说明: 先启动再进 play_task, 这样 http 和 play 两个任务和现在一样是
// 并发的, 不会给单曲模式引入额外延迟。
static esp_err_t start_http_task(const char *url) {
  http_task_arg_t *a = malloc(sizeof(*a));
  if (!a) {
    set_error("内存不足");
    return ESP_ERR_NO_MEM;
  }
  a->url = strdup(url);
  if (!a->url) {
    free(a);
    set_error("内存不足");
    return ESP_ERR_NO_MEM;
  }
  // 世代号在任务创建**之前**就递增: 任务一跑起来就可能登记 s_http_gen,
  // 晚改的话会被它自己的登记覆盖掉。
  a->gen = ++s_http_gen_next;

  s_abort_http = false; // 新会话, 清掉上一轮的掐断标记
  s_http_running = true;
  BaseType_t ok =
      xTaskCreatePinnedToCore(http_task, "webradio_http", HTTP_TASK_STACK,
                              a, RADIO_TASK_PRIO, &s_http_task, HTTP_TASK_CORE);
  if (ok != pdPASS) {
    free(a->url);
    free(a);
    s_http_running = false;
    set_error("HTTP 任务创建失败");
    return ESP_FAIL;
  }
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// 解码输出 -> 16bit PCM
// ---------------------------------------------------------------------------

// ⚠️⚠️ esp_audio_simple_dec 的输出样本宽度**不是**恒定的 16bit —— 它按源文件的
//     位深原样输出, 位深由 esp_audio_simple_dec_get_info() 的 bits_per_sample
//     给出 (头文件把它归在"can be used for play"那一类, 也就是**输出**格式)。
//
//     实测 (2026-09-17, 24bit/48kHz FLAC): 24bit 是**每样本 3 字节、小端、紧凑
//     排列** —— 前 24 字节按 3 字节一组切得到 0,0,-1,-1,+1,0,0,0 (音频), 按
//     4 字节一组切是 0,0xFFFF0000,0x01FFFFFF (跳变); 且 decoded_size/3/2 = 4096
//     正好是 FLAC 的标准块长。
//
//     之前这里恒按 int16 处理, 于是 24bit 素材被"每 2 字节重新切一刀": 有声音、
//     速度也对, 但内容全是垃圾 —— 用户听到的是"全是嘈杂的杂音"。
//     16bit 的 MP3/FLAC 一直正常, 所以这个 bug 只在 24bit 素材上暴露。
//     (components/sd_player/sd_player.c 里有同一段逻辑, 同一处坑, 一起改了。)

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

// 把 Q15 音量 (32768 = 0dB) 应用到 16-bit PCM。原地改。
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
// 状态
// ---------------------------------------------------------------------------

static void set_error(const char *msg) {
  strlcpy(s_error, msg ? msg : "", sizeof(s_error));
  s_state = WEB_RADIO_ERROR;
  ESP_LOGE(TAG, "%s", s_error);
}

// ---------------------------------------------------------------------------
// HTTP 接收
// ---------------------------------------------------------------------------

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
  switch (evt->event_id) {
  case HTTP_EVENT_ON_DATA: {
    if (s_stop_req || !evt->data || evt->data_len <= 0) {
      return ESP_OK;
    }

    const uint8_t *p = (const uint8_t *)evt->data;
    size_t remain = (size_t)evt->data_len;

    // 环形缓冲可能暂时满 (播放任务还没消费完)。**只能等** —— 等待让 TCP 窗口
    // 收缩形成背压, 这是我们要的流控。
    //
    // ⚠️ 这里**绝对不能**"等超时就丢一点"。
    //    最初写过一个 2 秒超时后丢弃残留的版本, 理由听着挺合理 ("MP3 每帧
    //    自同步, 丢几个字节能自己找回来")。实测下来是错的: 填充 256KB 环形
    //    缓冲只需要 **1.5 秒**, 所以正常播放时这个超时**必然**触发 —— 结果
    //    是每播几秒就永久跳过一段音频 (诊断行里 丢弃 一路涨)。
    //    正常背压和"卡死"在这个回调里长得一模一样, 分不出来就别分。
    //
    //    真正卡死的兜底在播放任务那边: 下载停滞检测 3 秒后会主动
    //    esp_http_client_close(), 那个回调自然会解开。这里只要继续等,
    //    顺带让出 CPU 给 play_task 消费缓冲 (同核 1, 同优先级)。
    //
    // ⚠️ 这里**必须把这次回调收到的字节全部写完**才能 return。
    //    回调返回后 esp_http_client 会把 data_process 前进整整 data_len ——
    //    你要是只写了一半就走人, 剩下那半就**永久丢了**, 表现为周期性缺口,
    //    听感就是持续的糊/爆。(§17 记的就是这一类坑, 别再犯。)
    //    缓冲满时**只能等**, 靠 TCP 窗口收缩形成背压 —— 这正是我们要的流控。
    //
    // ⚠️ 唯一的例外是**已被主动掐断** (停止/切曲): 这时马上要弃掉整条流,
    //    丢几个字节无所谓, 而尽快返回能让 perform() 立刻查 state、
    //    立刻退出 —— 这是把切曲延迟从 3 秒压到 0.3 秒的关键。
    //    这个出口**只在 s_abort_http 为真时**才生效, 正常播放走不到。
    while (remain > 0 && !s_stop_req && !s_abort_http) {
      size_t n = ring_write(p, remain);
      if (n == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
        continue;
      }
      p += n;
      remain -= n;
      s_bytes_downloaded += n;
      if (s_state == WEB_RADIO_CONNECTING) {
        s_state = WEB_RADIO_PLAYING;
      }
    }
    if (remain > 0 && s_abort_http) {
      ESP_LOGD(TAG, "已掐断, 放弃剩余 %u 字节以尽快退出", (unsigned)remain);
    }
    return ESP_OK;
  }

  case HTTP_EVENT_ON_CONNECTED:
    ESP_LOGI(TAG, "HTTP connected");
    return ESP_OK;

  case HTTP_EVENT_REDIRECT:
    // 电台 URL 基本都会 302 跳到真实流地址。自动跟随并记下来。
    if (evt->header_key && evt->header_value &&
        strcasecmp(evt->header_key, "Location") == 0) {
      ESP_LOGI(TAG, "Redirect -> %s", evt->header_value);
      strlcpy(s_url, evt->header_value, sizeof(s_url));
    }
    return ESP_OK;

  case HTTP_EVENT_ON_FINISH:
    ESP_LOGI(TAG, "HTTP stream finished");
    return ESP_OK;

  case HTTP_EVENT_DISCONNECTED:
    ESP_LOGI(TAG, "HTTP disconnected");
    return ESP_OK;

  case HTTP_EVENT_ERROR:
    ESP_LOGW(TAG, "HTTP error event");
    return ESP_OK;

  default:
    return ESP_OK;
  }
}

// HTTP 下载任务。
//
// ⚠️⚠️ 这个任务**必须尽快退出**, 而且它的慢是**无法回避**的 ——
//    esp_http_client_perform() 是阻塞式实现 (IDF 5.5.5), 内部
//    `while (data_process < content_length) { get_data(); }` 里的
//    `data_process` **不是 volatile**, 别的任务改 client->state 它看不见,
//    所以"关连接"要过很久才被它察觉 (实测 1.2~3 秒)。
//
//    这带来两个必须处理的问题, 这个结构就是为它们设计的:
//
//    1. **abort 打得准**: 每个任务实例把自己的 client 句柄存进
//       s_http_client, 由它自己负责清空 (退出前)。如果像早先那样在
//       退出路径上先置 NULL, 主循环的 abort 就找不到目标了 —— 实测就是
//       因为这个, 点「下一曲」完全不生效。
//
//    2. **不许误判串曲**: 本任务可能比 play_task 预期的活得久一点
//       (3 秒窗口)。但那**不等于**它在下一次播放里还活着 —— 用
//       s_http_gen (世代号) 区分: 每次 start_http_task 递增, 任务退出时
//       发现自己的世代已经过期, 就什么都不动, 免得把新会话的状态搅乱。
static void http_task(void *arg) {
  http_task_arg_t *a = (http_task_arg_t *)arg;
  char *url = a->url;
  uint32_t my_gen = a->gen;
  free(a);

  ESP_LOGI(TAG, "HTTP task start: %s", url);

  esp_http_client_config_t cfg = {
      .url = url,
      .event_handler = http_event_handler,
      .buffer_size = HTTP_BUF_SIZE,
      .timeout_ms = 10000,
      .keep_alive_enable = false,
      // 电台流不设 Content-Length, 必须允许分块/无限流
      .disable_auto_redirect = false,
      .crt_bundle_attach = esp_crt_bundle_attach, // 理由见 fetch_small 那处
  };

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) {
    set_error("HTTP client init failed");
    goto done;
  }

  // 有些电台服务器要求 UA, 不然返回 403
  esp_http_client_set_header(client, "User-Agent", "ESP32-WebRadio/1.0");
  esp_http_client_set_header(client, "Icy-MetaData", "0");

  // 登记给主循环 —— abort_current_http() 靠这个句柄掐断本次下载。
  // ⚠️ 必须在这里登记 (不是更早), 而且要由本任务自己清空 (见 done:)。
  s_http_client = client;
  s_http_gen = my_gen;

  esp_err_t err = esp_http_client_perform(client);

  // ⚠️ 被主动掐断 (停止/切曲) 时 perform() 必然返回 INCOMPLETE_DATA 之类的
  //    错误 —— 连接是我们自己 shutdown 掉的, 这是**预期**结果, 不是故障。
  //    不排除这种情况的话, 每次点「下一曲」都会顺手把状态置成 ERROR,
  //    网页就会一直显示"出错" (而歌其实已经切过去了)。
  if (!s_abort_http && err != ESP_OK && !s_stop_req) {
    char msg[160];
    snprintf(msg, sizeof(msg), "HTTP 失败: %s", esp_err_to_name(err));
    set_error(msg);
  }

  int status = esp_http_client_get_status_code(client);
  ESP_LOGI(TAG, "HTTP done, status=%d, downloaded=%u bytes",
           status, (unsigned)s_bytes_downloaded);

  if (!s_abort_http && status != 200 && status != 0 && !s_stop_req &&
      s_state != WEB_RADIO_ERROR) {
    char msg[160];
    snprintf(msg, sizeof(msg), "服务器返回 HTTP %d", status);
    set_error(msg);
  }

  esp_http_client_cleanup(client);

done:
  // ⚠️ 只有**还属于本世代**才动全局状态。
  //    本任务可能比预期多活一会儿 (perform 的退出延迟), 那期间会话可能已经
  //    换了一轮 —— 这时把 s_http_running 置 false 会让 play_task 误以为
  //    新会话已经结束, 曲末判断全乱。世代号把这种情况挡掉。
  if (s_http_gen == my_gen) {
    s_http_client = NULL;
    s_http_running = false;
    s_http_task = NULL;
    xEventGroupSetBits(s_ring.ev, EV_HTTP_DONE);
  } else {    ESP_LOGD(TAG, "HTTP task (gen %u) 退出时已是 gen %u, 不动作",
             (unsigned)my_gen, (unsigned)s_http_gen);
  }

  free(url); // http_task 拥有这个副本
  vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// 播放任务: 环形缓冲 -> 解码 -> I2S
// ---------------------------------------------------------------------------

static void play_task(void *arg) {
  (void)arg;

  int cur = (int)(intptr_t)arg; // 起始曲目序号 (单曲模式恒为 0)

  // ⚠️⚠️ 解码输入缓冲 (PSRAM, 64KB) —— 这是整个文件最容易搞错的地方。
  //
  // esp_audio_simple_dec 的输入契约 (见 espressif 自己的
  // test_apps/.../simple_decoder_test.c, 以及头文件里 raw.consumed 的定义):
  //
  //   1. 一次 process() **不代表**把喂进去的字节用完了。它可能只消费了
  //      前面若干字节 (raw.consumed), 剩下的要你**原样再喂一遍**。
  //   2. 返回 OK 且 decoded_size==0 也是**正常**的, 意思是"这些字节我收下
  //      了/还没凑够一帧", 不是错误。
  //   3. 没消费完的字节, 下次调用时内容**必须保持不变**:
  //        "ATTENTION: when input raw data unconsumed (`raw.len > 0`) do not
  //         overwrite its content. Or-else unexpected error may happen for
  //         data corrupt."
  //
  // 所以正确做法是 (照抄参考实现的形状):
  //     raw.len -= raw.consumed;
  //     raw.buffer += raw.consumed;
  // 把游标在**同一块缓冲内部**往前推, 直到 raw.len == 0 才读下一块。
  //
  // 之前这里是"每轮从环形缓冲读一块新数据, process 一次就丢", 两处都错:
  //
  //   · 没有推进游标 → 若 consumed < len, 那批**没被消费的尾巴被直接丢掉**。
  //     PCM 输出从头到尾都是周期性缺口, 听起来就是持续的"糊/爆"。
  //     (旧代码只处理了 decoded_size==0, 完全忽略了 consumed<len 的情况。)
  //
  //   · 用"两个 4KB 缓冲交替"来保证"上一轮内容不被覆盖"是**不够的**:
  //     len 最大 4KB, 一帧要 640B (MP3) / 20KB (FLAC 4096 样本), 所以
  //     解码器要跨越**5 次以上** process 才能凑齐一帧 —— 而双缓冲只保住了
  //     最近 2 块, 中间那些早被覆盖了。数据必然损坏。
  //
  // 现在: 解码器直接把 ring_read 到 enc_buf 的数据**原地消费掉** (游标在
  // enc_buf 内推进), 只有 ring_read 拿到新数据时才会写 enc_buf, 而那时
  // 上一批必然已经消费完 —— 冲突从根上消失了, 不再需要任何"交替"技巧。
  uint8_t *enc_buf = heap_caps_malloc(DEC_IN_BYTES, MALLOC_CAP_SPIRAM);
  size_t pcm_buf_size = PCM_OUT_BYTES;
  int16_t *pcm_buf = heap_caps_malloc(pcm_buf_size, MALLOC_CAP_SPIRAM);
  if (!enc_buf || !pcm_buf) {
    set_error("内存不足");
    goto cleanup;
  }

  // =========================================================================
  // ⚠️ 曲目循环 —— 歌单顺序播放就靠这一层。
  //
  // 设计要点 (改动时别破坏):
  //
  //  1. play_task **跨曲存活**。一首放完不退出任务, 而是接着开下一曲 ——
  //     任务退出/重建意味着重新抢 I2S、走一遍 web_radio_stop() 的最长 5 秒
  //     阻塞等待, 曲间会有很明显的停顿。
  //
  //  2. I2S **全程握在电台手里**, 曲间不调 output_stop()/output_start()。
  //     output_stop 只在 web_radio_play_url 进来时调一次。
  //
  //  3. 曲间切换**绝不能**调 web_radio_play_url()/web_radio_stop() ——
  //     那两个函数都会等 s_play_task == NULL, 而这里就是 s_play_task 本身,
  //     自等自必然超时(5秒)。所以 next/prev 走 s_skip_req 标志。
  //
  //  4. 单曲模式 (s_pl.count == 0) 循环只跑一圈就 break, 行为与改造前一致。
  //     注意单曲模式的 http_task 是 play_url 在进本任务**之前**就启动好的
  //     (见那里的说明), 所以这里第一圈不用再 start 一次。
  // =========================================================================
  // ⚠️⚠️ 这个循环体里**只允许有一个 `continue`**, 就是最末尾"自动播下一曲"
  //      那一个 —— 它前面紧跟着 `cur++`, 是两个绑在一起的语句。
  //
  //      在循环体**前面**再加一个 `continue` 会直接跳到 `cur++` 上, 结果是:
  //        · 跳过 next_track 整段 (关解码器 / 等 http_task 退出 / 失败检查)
  //        · 刚设好的 cur 又被 +1, 直接多跳一首
  //      曲间切换一律走 `goto next_track` (它跳过 cur++), 别用 continue。
  // =========================================================================
  bool first_iter = true;
  for (;;) {
    // 处理切曲请求 (在上一曲的解码循环里也可能已经被消费掉了,
    // 那种情况下这里什么也不做)。见 apply_skip 的说明。
    cur = apply_skip(cur);

    if (cur >= pl_total()) {
      break; // 越界 (理论上不该发生), 收工
    }

    track_begin(cur);

    // 预读文件头拿标签。失败不影响播放 —— 内部已回退到 EXTINF/文件名标题。
    // ⚠️ 只有歌单模式才预读。单曲/电台流模式下多开一次连接是白费功夫,
    //    而且电台流 (无 Content-Length 的无限流) 预读还得靠超时断开。
    //    track_begin 已经按 URL 文件名铺好标题了。
    if (s_pl.count > 0) {
      track_probe_tags(cur);
    }

    // 从第 2 曲开始 (以及 skip 之后) 才在这里启动下载。
    // 第 1 曲的下载已由 web_radio_play_url 启动 —— 让它和本任务并发跑,
    // 避免给单曲模式引入额外的启动延迟。
    if (!first_iter) {
      const char *u = pl_url_at(cur);
      if (!u || start_http_task(u) != ESP_OK) {
        break;
      }
    }
    first_iter = false;

    // ---- 本曲开始 ----
    ring_reset(); // 曲间必须清干净, 否则新曲会先解出上一曲的尾巴

    esp_audio_simple_dec_handle_t dec = NULL;
    // 容器类型按**当前曲**的后缀选 —— 歌单里 MP3 和 FLAC 混排时每曲都不同。
    esp_audio_simple_dec_type_t dec_type = dec_type_from_url(s_cur_url);

    esp_audio_simple_dec_cfg_t dec_cfg = {
        .dec_type = dec_type,
        .dec_cfg = NULL,
        .cfg_size = 0,
        .use_frame_dec = false, // 让内部 parser 处理任意长度的输入
    };

    esp_audio_err_t aerr = esp_audio_simple_dec_open(&dec_cfg, &dec);
    if (aerr != ESP_AUDIO_ERR_OK) {
      char msg[160];
      snprintf(msg, sizeof(msg), "解码器打开失败 (第%d曲, type=%d): %d",
               cur + 1, (int)dec_type, aerr);
      set_error(msg);
      s_track_failed = true;
      goto next_track;
    }
    ESP_LOGI(TAG, "第%d曲 解码器已打开 (type=%d)", cur + 1, (int)dec_type);

    bool got_format = false;
    int64_t last_progress_us = esp_timer_get_time();
    uint32_t last_bytes = 0;
    uint32_t dbg_loops = 0;
    uint32_t dbg_pcm_bytes = 0;
    uint32_t dbg_decode_errs = 0;
    uint32_t dbg_pcm_frames = 0;
    int64_t dbg_last_us = last_progress_us;

  #define WEB_RADIO_DEC_ERR_BAIL 30

    while (!s_stop_req) {
      // 切曲: 丢掉上一曲在缓冲里的残留 (最多 256KB ≈ 16 秒)。
      // 不丢的话要**听它播完**才切得过去 —— 实测切曲延迟 16 秒。
      //
      // ⚠️ 这里用 apply_skip() 而不是只清标志 —— 它会顺手把目标曲目算好
      //    存进 cur。只清不算的话, 回到 for 顶部标志已空, cur 没变,
      //    「下一曲」就退化成重播当前曲 (实测踩过, 网页永远停在 1/2)。
      if (s_skip_req) {
        cur = apply_skip(cur);
        ring_reset();
        ESP_LOGI(TAG, "丢弃缓冲残留, 立即切曲");
        break;
      }
      size_t avail = ring_used();

      if (avail == 0) {
        // 缓冲空: 本曲放完了 (或者下载断了)
        if (!s_http_running) {
          ESP_LOGI(TAG, "第%d曲 流结束且缓冲已放完", cur + 1);
          break;
        }
        // 还在下载, 等一会儿
        s_underrun_count++;
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
      // 见函数开头对 raw.consumed 契约的说明。
      while (raw.len > 0 && !s_stop_req) {
        // 单声道流要把解码结果**原地翻倍**扩成立体声 (见下面
        // s_dec_channels == 1)，所以解码器最多只准用一半缓冲区。
        // 声道数要到首帧解出来之后才从解码器拿到，首帧那一趟靠下面的
        // 扩容兜底。(同 sd_player.c:743。)
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
        // (参考实现冲的是整个文件, 出错直接 break; 我们冲的是网络流,
        //  必须吃掉这些字节往前走, 让 parser 重新同步。)
        uint32_t consumed = raw.consumed;

        if (r == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
          // 输出缓冲不够。按 needed_size 真正扩容, 然后重试同一帧
          // (这次不推进游标 —— 帧还没解出来)。
          //
          // ⚠️ 单声道要再乘 2：上面把 out.len 收成了 pcm_buf_size/2，若只扩到
          //    needed_size，下一趟 out.len 仍然小于 needed_size，这个分支就会
          //    原地打转 —— 必须保证 pcm_buf_size >= 2 * needed_size。
          size_t need = out.needed_size ? out.needed_size : (pcm_buf_size * 2);
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
          // 前几次错误打出来 —— 这是排查"解不出声"最直接的线索。
          if (dbg_decode_errs <= 6) {
            ESP_LOGW(TAG, "解码错误 %d (已消费 %u/%u)", r, (unsigned)consumed,
                     (unsigned)raw.len);
          }
          // 连续错太多次说明这批数据没救了: 丢掉剩余部分, 从下一个
          // 环形缓冲块重新开始 (parser 会重新找帧头)。
          if (dbg_decode_errs % WEB_RADIO_DEC_ERR_BAIL == 0) {
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

        // 前几次成功解码打日志, 方便确认解码器真的在出 PCM
        if (dbg_pcm_frames < 3) {
          ESP_LOGI(TAG, "解码成功: %u 字节 PCM (本块剩 %u, 消费 %u)",
                   (unsigned)out.decoded_size, (unsigned)raw.len,
                   (unsigned)consumed);
          dbg_pcm_frames++;
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
            ESP_LOGI(TAG, "流格式: %u Hz, %u ch, %u bit",
                     (unsigned)info.sample_rate, (unsigned)info.channel,
                     (unsigned)info.bits_per_sample);

            // I2S 输出率跟随流的原生采样率。
            //
            // 背景: 项目默认 CONFIG_OUTPUT_SAMPLE_RATE_HZ=44100, AirPlay 用它的
            // playback_task 把 44.1k 源重采样到 OUTPUT_RATE。但我们**不启用**
            // 那个任务 (见下面"绝对不要调 output_start"), 所以没有 SRC。
            // 48kHz 的流若直接喂给 44.1k 的 I2S 会快 8.8% (音调偏高)。
            //
            // ⚠️ 每曲都会重跑这一段 (got_format 是曲内局部变量)。歌单里
            //    44.1k 和 48k 混排时, 每曲都要把 I2S 率切回来, 否则第二曲
            //    音调会跑。
            //
            // ⚠️ 守卫: 只有 AirPlay 的 playback_task **没在跑**时才改 I2S 采样率。
            //   一旦它在跑, set_sample_rate 会改变 playback_task 重采样器的目标
            //   率, 把 AirPlay 的 44.1k 源拖进 44.1→48 的转换, 属于对 AirPlay 的
            //   回归。改用 output_set_source_rate 更安全 —— 它只影响重采样判断,
            //   若 playback_task 未运行则完全无害。
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
        // audio_receiver_read() 取数据 —— 而 web radio 的数据根本不走那条路。
        // 一旦启动, 它每 ~8ms 读不到数据就往同一个 I2S DMA 灌一帧静音,
        // 和我们直写的 PCM 互相覆盖, 结果是持续的爆音/杂音。
        //
        // 正确做法看 A2DP (同样是外部音源): 只调 audio_output_write() 直写
        // I2S, 前提是 playback_task 已停 —— play_url 里的 output_stop() 负责这个。
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

        // I2S 是 16bit 立体声 slot。单声道流必须复制成两声道, 否则 I2S 会
        // 按"L/R 交替"去读单声道样本 —— 左右耳各听到一半采样, 等于把采样率
        // 减半, 声音又闷又怪。FLAC 单声道专辑 (少见但存在) 会踩到。
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
        dbg_pcm_bytes += out.decoded_size;
      }

      // ---- 每秒一次的诊断行, 确认链路在动 ----
      dbg_loops++;
      {
        int64_t t = esp_timer_get_time();
        if (t - dbg_last_us > 1000000) {
          // 比一下下载和 PCM 的**增量**就知道有没有在丢数据:
          // 健康时 (PCM增量)/(下载增量) ≈ 176400 / (码率/8)。
          // FLAC 约 900KB/s 下载对 176KB/s PCM → 比值约 0.2;
          // MP3 约 40KB/s 下载 → 比值接近 4.4。数字长期不变就是对的。
          ESP_LOGI(TAG,
                   "诊断: 第%d曲 循环%u 下载%u PCM%u 缓冲%u 峰值%u 欠载%u 解码错%u",
                   cur + 1, (unsigned)dbg_loops, (unsigned)s_bytes_downloaded,
                   (unsigned)dbg_pcm_bytes, (unsigned)ring_used(),
                   (unsigned)s_ring_high_water, (unsigned)s_underrun_count,
                   (unsigned)dbg_decode_errs);
          dbg_last_us = t;
        }
      }

      // ---- 卡顿自愈 ----
      // 每 3 秒看一次下载速率。掉到阈值以下说明流断了/服务器没了。
      // 只关掉当前 HTTP 会话 (perform 返回后 http_task 自然结束), 不置
      // s_stop_req —— 那会终止整个播放会话。关掉后本曲会走"缓冲放空 +
      // !s_http_running"的收尾路径, 然后由下面的 s_track_failed 决定停还是跳。
      int64_t now = esp_timer_get_time();
      if (now - last_progress_us > 3000000) {
        uint32_t delta = s_bytes_downloaded - last_bytes;
        if (delta < STALL_BYTES_PER_SEC * 3 && s_http_running &&
            s_http_client) {
          ESP_LOGW(TAG, "下载停滞 (%u 字节/3秒), 中止本次 HTTP 会话",
                   (unsigned)delta);
          s_underrun_count++;
          s_track_failed = true;
          abort_current_http();
        }
        last_bytes = s_bytes_downloaded;
        last_progress_us = now;
      }
  } // while 本曲解码循环

  next_track:
    if (dec) {
      esp_audio_simple_dec_close(dec);
      dec = NULL;
    }

    // ⚠️ 必须等 http_task 真正退出再进下一曲。
    //    否则它还在往环形缓冲里灌**上一曲的字节**, 而下一曲的 ring_reset()
    //    会把 tail 归零 —— 旧数据立刻变成"新曲的数据"被解码, 表现为
    //    切曲后马上解码失败或放出一段杂音。这是 web_radio_stop 里同一个坑
    //    的曲间版本。
    //    用 abort_current_http() (不是裸 close) —— 见它的说明: 只 close
    //    的话 perform() 要等读超时才会返回, 2 秒内等不到。
    for (int i = 0; i < 150 && s_http_task != NULL; i++) {
      abort_current_http();
      vTaskDelay(pdMS_TO_TICKS(20)); // 最长 3 秒
    }
    if (s_http_task != NULL) {
      ESP_LOGW(TAG, "HTTP 任务未退出, 停止播放以免串曲");
      s_stop_req = true;
      break;
    }

    if (s_stop_req) {
      break; // 用户点了停止
    }

    // ⚠️ 被掐断 (切曲) 时 http_task 会在 1 毫秒内退出, 于是 s_http_running
    //    已经变 false —— 看起来和"流自然放完"一模一样。要靠 s_abort_http
    //    区分: 它是真就说明这是我们主动掐的, 不是放完、也不是故障。
    //    这一步必须在下面"s_track_failed 判失败"和"自动播下一曲"之前。
    bool aborted = s_abort_http;

    // 下载失败 (不是自然放完、也不是被掐断) → 停住报错, **不自动跳下一曲**。
    // 否则网络一断就会一首接一首地跳, 日志刷屏且毫无意义。
    if (s_track_failed && !aborted) {
      if (s_state != WEB_RADIO_ERROR) {
        set_error("下载中断");
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
    if (cur + 1 < pl_total()) {
      cur++;
      ESP_LOGI(TAG, "自动播下一曲 (第 %d/%d)", cur + 1, pl_total());
      continue;
    }

    // 最后一曲放完 → 停止 (不环绕; 手动点「下一条」才环绕)
    ESP_LOGI(TAG, "歌单播放完毕 (%d 曲)", pl_total());
    s_state = WEB_RADIO_IDLE;
    break;
  } // for 曲目循环

  // 注意: 这里不需要 audio_output_stop()。
  // 我们从没启动过 AirPlay 的 playback_task (见上面"绝对不要调 output_start"
  // 的说明), 所以没有需要停的东西。I2S 就是从 AirPlay 手里接管过来的,
  // 交还由 main.c 在 AirPlay 恢复时 (start_airplay_services -> output_start)
  // 自己完成。

cleanup:
  heap_caps_free(enc_buf);
  heap_caps_free(pcm_buf);

  s_play_task = NULL;
  xEventGroupSetBits(s_ring.ev, EV_HTTP_DONE); // 唤醒 stop 等待
  vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------

esp_err_t web_radio_init(void) {
  memset(&s_ring, 0, sizeof(s_ring));

  // ⚠️ MP3 / FLAC 必须**手动注册**给 simple dec。
  //   esp_audio_simple_dec_register_default() 在 Kconfig 里只有 WAV/M4A/TS/OGG
  //   四个开关, **不含 MP3/FLAC** —— 不注册的话 open 会返回 -7 (NOT_SUPPORT)。
  //   (这和 memory 里记的 "esp_audio_codec 预编译库必须手动注册解码器"
  //    是同一类坑, 换了个组件又撞一次。)
  //   parser 用库里的 esp_xxx_dec_parse_frame, 它们按帧头/容器同步,
  //   正好补上"从 HTTP 字节流里切帧"这个最容易出错的环节。
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

  s_ring.mutex = xSemaphoreCreateMutex();
  s_ring.ev = xEventGroupCreate();
  if (!s_ring.mutex || !s_ring.ev) {
    return ESP_ERR_NO_MEM;
  }

  s_state = WEB_RADIO_IDLE;
  s_error[0] = '\0';
  s_url[0] = '\0';

  ESP_LOGI(TAG, "Web radio ready (缓冲 %d KB, 放 PSRAM)", RING_SIZE_BYTES / 1024);
  return ESP_OK;
}

// 内部实现, 定义在下面。true = 顺带把 I2S 交还给 AirPlay。
static esp_err_t radio_stop_internal(bool yield_to_airplay);

esp_err_t web_radio_stop(void) { return radio_stop_internal(false); }

// 用户主动停止 / AirPlay 要接管时用 —— 在内部停止的基础上, 再把 I2S
// **完整地**交还给 AirPlay (还原采样率 + 重新拉起 playback_task)。
esp_err_t web_radio_stop_and_yield(void) { return radio_stop_internal(true); }

static esp_err_t radio_stop_internal(bool yield_to_airplay) {
  // ⚠️ 早退路径也要走"交还 I2S" —— 见 web_radio_yield_to_airplay 的说明:
  //    "网络歌曲播完后"就是这个分支 (play_task 已自己退出), 而它恰恰是最
  //    需要交还 I2S 的时刻。以前这里直接 return, 导致 AirPlay 来了没声音。
  if (s_play_task == NULL && s_http_task == NULL) {
    s_state = WEB_RADIO_IDLE;
    if (yield_to_airplay && s_hooks_ready && s_hooks.resume_airplay) {
      s_hooks.resume_airplay();
      ESP_LOGI(TAG, "电台已空闲, I2S 交还 AirPlay");
    }
    return ESP_OK;
  }

  ESP_LOGI(TAG, "停止当前播放");
  s_stop_req = true;

  // ⚠️ 必须**主动掐断** HTTP 客户端。
  //   esp_http_client_perform() 在慢流上最长会阻塞 timeout_ms (这里 10 秒),
  //   光置 s_stop_req 它是看不见的。不掐的话 http_task 会一直活着,
  //   而我们下面一旦把 s_stop_req 重置, 它就会"复活"并继续把**旧 URL 的
  //   数据**灌进环形缓冲 —— 新会话读到旧数据, 表现为换歌后立刻解码失败。
  //   用 abort_current_http (不是裸 close): close 之后 perform 还要等读超时。
  abort_current_http();

  // 等两个任务真正退出。每轮都再掐一次, 防止竞态。
  // ⚠️ 这里也改成 abort_current_http() —— 原来只有 close, 而 close 之后
  //    perform() 还能再阻塞到读超时 (~2 秒), 5 秒的窗口有时不够, 会误报
  //    "任务未退出" 并保持 s_stop_req, 下一次播放就起不来了。
  for (int i = 0; i < 100; i++) {
    if (s_play_task == NULL && s_http_task == NULL) {
      break;
    }
    abort_current_http();
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  if (s_play_task != NULL || s_http_task != NULL) {
    // 任务没能按时退出。**不要**重置 s_stop_req —— 保持它让任务尽快自尽,
    // 否则残留任务会污染下一次播放。这是宁可让本次停止变慢, 也不能让
    // 状态错乱。
    ESP_LOGE(TAG, "任务未退出 (play=%p http=%p), 保持停止标志",
             (void *)s_play_task, (void *)s_http_task);
    return ESP_ERR_TIMEOUT;
  }

  ring_reset();
  s_stop_req = false; // 两个任务都确认没了, 这时才安全
  s_skip_req = false;
  s_skip_delta = 0;
  s_track_failed = false;
  s_abort_http = false;
  s_state = WEB_RADIO_IDLE;
  s_dec_sample_rate = 0;
  s_dec_channels = 0;
  s_dec_bits = 0;
  s_underrun_count = 0;
  s_bytes_downloaded = 0;
  s_ring_high_water = 0;
  s_error[0] = '\0';

  // 歌单 / 曲目信息一并清掉
  s_pl.count = 0;
  s_pl.index = 0;
  s_pl.truncated = false;
  s_single_url[0] = '\0';
  s_cur_url[0] = '\0';
  memset(&s_ti, 0, sizeof(s_ti));

  // ⚠️⚠️ 把 I2S **完整地**交还给 AirPlay —— 这一步不能省。
  //
  //    电台播放期间做了两件"有副作用"的事, 停播时必须逐条还原:
  //      1. 按流的原生采样率改过 I2S (48kHz 的歌就把 I2S 改成 48k)
  //      2. 停掉了 AirPlay 的 playback_task 来独占 DMA
  //
  //    这两件在本次改动之前**都没人还原**, 后果是:
  //    播完网络歌曲后用 iPhone 投 AirPlay —— 歌在"播放"但完全没声音,
  //    而且 I2S 还停在 48k, 就算拉起任务也会变调。用户实测**必须复位设备**
  //    才能恢复 (只有重启才会重走 start_airplay_services -> output_start)。
  //
  //    ⚠️ 只在 yield_to_airplay 为真时做。web_radio_play_url() 内部也会调
  //       本函数 (先停旧的再播新的), 那条路径**不能**拉起 playback_task ——
  //       它读不到数据就会灌静音, 和我们的 PCM 抢同一个 I2S DMA。
  //       见文件头第 2 条。所以两条路径必须分开:
  //         web_radio_stop()            — 内部停止, 不碰 AirPlay
  //         web_radio_stop_and_yield()  — 内部停止 + 交还 I2S
  if (yield_to_airplay && s_hooks_ready && s_hooks.resume_airplay) {
    s_hooks.resume_airplay();
    ESP_LOGI(TAG, "已停止, I2S 已交还 AirPlay (采样率已还原, playback_task 已重启)");
  } else {
    ESP_LOGI(TAG, "已停止");
  }
  return ESP_OK;
}

esp_err_t web_radio_play_url(const char *url) {
  if (!s_hooks_ready) {
    set_error("音频输出未就绪 (main 未注册钩子)");
    return ESP_ERR_INVALID_STATE;
  }
  if (!url || strlen(url) == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
    set_error("URL 必须以 http:// 或 https:// 开头");
    return ESP_ERR_INVALID_ARG;
  }
  if (strlen(url) >= sizeof(s_url)) {
    set_error("URL 太长");
    return ESP_ERR_INVALID_ARG;
  }

  // 先停干净 (幂等)。必须确认真的停干净了 —— 否则旧 HTTP 任务还活着,
  // 会把上一个 URL 的数据灌进环形缓冲, 新会话读到就解码失败。
  esp_err_t serr = web_radio_stop();
  if (serr != ESP_OK) {
    set_error("上一个任务未能停止, 请稍后重试");
    return serr;
  }

  strlcpy(s_url, url, sizeof(s_url));
  s_error[0] = '\0';

  const char *first_url = url; // 实际要播的第一个地址 (歌单模式是第 1 条的地址)

  // ---- m3u 歌单分支 ----------------------------------------------------
  // ⚠️ 只按 URL **后缀**识别, 不做内容嗅探 (理由见 playlist_load 的说明)。
  //    url_has_ext 已经会剥掉 ?query/#frag 再比, 所以 a.m3u?token=x 也能认。
  if (url_has_ext(url, ".m3u") || url_has_ext(url, ".m3u8")) {
    ESP_LOGI(TAG, "识别为 m3u 歌单, 先拉取: %s", url);
    s_state = WEB_RADIO_CONNECTING; // 拉歌单期间网页显示"正在连接…"

    // 歌单结构 (s_pl.urls / s_pl.titles) 首次使用时分配一次, 常驻 PSRAM。
    if (!s_pl.urls) {
      s_pl.urls = heap_caps_calloc(MAX_PLAYLIST, 256, MALLOC_CAP_SPIRAM);
      s_pl.titles = heap_caps_calloc(MAX_PLAYLIST, 128, MALLOC_CAP_SPIRAM);
      if (!s_pl.urls || !s_pl.titles) {
        heap_caps_free(s_pl.urls);
        heap_caps_free(s_pl.titles);
        s_pl.urls = NULL;
        s_pl.titles = NULL;
        set_error("歌单缓冲分配失败");
        return ESP_ERR_NO_MEM;
      }
    }

    esp_err_t perr = playlist_load(url);
    if (perr != ESP_OK) {
      set_error(perr == ESP_ERR_NOT_FOUND ? "歌单里没有可用条目"
                                          : "歌单拉取失败");
      return perr;
    }

    strlcpy(s_pl_url, url, sizeof(s_pl_url));
    first_url = s_pl.urls[0];
    ESP_LOGI(TAG, "歌单共 %d 条, 开始播第 1 条", s_pl.count);
  } else {
    // 单曲模式
    s_pl.count = 0;
    s_pl.truncated = false;
    s_pl_url[0] = '\0';
    strlcpy(s_single_url, url, sizeof(s_single_url));
  }

  s_bytes_downloaded = 0;
  s_dec_sample_rate = 0;
  s_dec_channels = 0;
  s_dec_bits = 0;
  s_state = WEB_RADIO_CONNECTING;
  s_stop_req = false;
  s_skip_req = false;
  s_skip_delta = 0;
  s_track_failed = false;
  s_abort_http = false;
  ring_reset();

  // 先把第 0 曲的对外信息准备好, 这样网页在 play_task 起来之前
  // (预读那几百毫秒) 就能显示出正确的"第 1/N 曲"。
  track_begin(0);

  // 从 AirPlay 手里接管 I2S。output_stop 内部最多等 2 秒。
  ESP_LOGI(TAG, "接管 I2S (暂停 AirPlay playback task)");
  s_hooks.output_stop();

  // ⚠️ http_task 在这里就启动, 而不是等 play_task 进去再启动 ——
  //    这样单曲模式和改造前完全一样 (两个任务并发跑)。
  //    play_task 里对 cur==0 的那一圈**不再**重复启动。
  //    标签预读因此发生在下载已经开始之后, 不占用启动时间。
  esp_err_t herr = start_http_task(first_url);
  if (herr != ESP_OK) {
    return herr;
  }

  // 把起始曲目序号当任务参数传进去 —— 必须是整型, 不能传指针
  // (栈上的局部变量地址在任务真正跑起来前就失效了)。
  if (xTaskCreatePinnedToCore(play_task, "webradio_play", RADIO_TASK_STACK,
                              (void *)(intptr_t)0, RADIO_TASK_PRIO,
                              &s_play_task, PLAY_TASK_CORE) != pdPASS) {
    s_stop_req = true;
    set_error("播放任务创建失败");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "开始播放: %s", first_url);
  return ESP_OK;
}

void web_radio_get_track_info(web_radio_track_info_t *out) {
  if (!out) {
    return;
  }
  // 整体拷贝。s_ti 只在 play_task 里写 (曲目边界), 这里的读可能撞上写,
  // 但最坏情况是某几个字段差半拍 (标题是这一曲、序号是下一曲), 网页
  // 3 秒后轮询就纠正了 —— 不值得为此上锁。
  *out = s_ti;
}

void web_radio_skip(int delta) {
  if (delta != 1 && delta != -1) {
    return;
  }
  if (!web_radio_is_active() || s_pl.count == 0 || s_http_task == NULL) {
    return; // 没在播 / 不是歌单 / 还没起来, 忽略
  }

  s_skip_delta = delta;
  s_skip_req = true;

  // ⚠️ 必须**主动掐断**当前下载, 否则 play_task 要等这一曲的 HTTP 流
  //    自然结束才看得到标志 —— 一首歌 3 分钟, 按钮就等于没反应。
  //    用 abort (不是裸 close): close 之后 perform() 还要等读超时才返回,
  //    实测要 1.2~3 秒。详见 abort_current_http()。
  abort_current_http();
  ESP_LOGI(TAG, "切曲请求: %+d (当前第 %d/%d 曲)", delta, s_pl.index + 1,
           pl_total());
}

bool web_radio_is_active(void) {
  return s_state == WEB_RADIO_CONNECTING || s_state == WEB_RADIO_PLAYING;
}

web_radio_state_t web_radio_get_state(void) { return s_state; }

void web_radio_get_url(char *buf, size_t buf_len) {
  if (buf && buf_len > 0) {
    strlcpy(buf, s_url, buf_len);
  }
}

void web_radio_get_error(char *buf, size_t buf_len) {
  if (buf && buf_len > 0) {
    strlcpy(buf, s_error, buf_len);
  }
}

void web_radio_yield_to_airplay(void) {
  // ⚠️⚠️ **不能**加 `if (web_radio_is_active())` 守卫 —— 这正是原 bug 的根源。
  //
  //    用户的场景是"网络歌曲**播完**之后投 AirPlay": 播完时 play_task 自己
  //    把状态置成 IDLE 就退出了, **根本没走 stop()**, 所以 I2S 采样率和
  //    playback_task 都还停在"电台占着"的状态。
  //    而此时 web_radio_is_active() 是 **false** —— 加守卫的话这个回调
  //    什么都不做, AirPlay 推流没人写 I2S, 表现就是"歌在播放但没声音",
  //    只有重启设备才好 (重启才会重走 start_airplay_services)。
  //
  //    stop_and_yield 是幂等的: 没有任务在跑时它只做"交还 I2S"这一步,
  //    重复调用无害。所以这里无条件调用。
  ESP_LOGI(TAG, "AirPlay 会话开始, 电台让出 I2S");
  web_radio_stop_and_yield();
}
