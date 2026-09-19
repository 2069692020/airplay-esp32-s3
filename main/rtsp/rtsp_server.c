#include "rtsp_server.h"

#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "audio_receiver.h"
#include "audio_output.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "rtsp_conn.h"
#include "rtsp_crypto.h"
#include "rtsp_handlers.h"
#include "rtsp_message.h"

#include "ntp_clock.h"
#include "ptp_clock.h"
#include "rtsp_events.h"
#include "settings.h"
#include "dacp_client.h"

static const char *TAG = "rtsp_server";

#define RTSP_PORT           7000
#define RTSP_BUFFER_INITIAL 4096
#define RTSP_BUFFER_LARGE   ((size_t)256 * 1024)

#define CLIENT_STACK_SIZE 8192
#define SERVER_STACK_SIZE 4096

static int server_socket = -1;
static TaskHandle_t server_task_handle = NULL;
static bool server_running = false;

// RTSP tasks are restartable. Use dynamic TCB allocation so
// reconnect/start-stop paths cannot reuse static task memory before FreeRTOS
// idle finishes deletion.

// Client slot for tracking connections
typedef struct {
  rtsp_conn_t *conn;
  TaskHandle_t task;
  int socket;
  volatile bool should_stop;
  volatile bool is_old; // Marked as old client being killed
  // ⚠️ "本 slot 有活跃会话" 只能这样问，**不能** 去读 conn：conn 由本 slot 的
  //    client_task 持有并在断开时 free，而读方是优先级 9 的 playback_task
  //    (每帧一次) 和 httpd/按键任务。clients[] 是静态数组，所以读这个标量本身
  //    不可能踩到已释放内存。写方只有 client_task 自己。
  volatile bool has_session;
} client_slot_t;

static client_slot_t clients[2] = {0}; // Current and old
static int current_slot = 0;

// Flag set by the play/pause button to tell the grace period loop
// to send a DACP resume command and keep waiting for reconnect.
static volatile bool s_resume_requested = false;

// 音量相关的公共 API 集中在下面 airplay_volume_db_to_q15() 之后
// (换算曲线必须先定义，两边才能共用同一份实现)。

// dB ↔ Q15 换算。放在这里是因为它必须和 airplay_apply_volume_db() 里那套曲线
// **完全一致** —— 网页音量滑块也走这个换算, 两边算法一旦分叉, 同一个音量值
// 经 AirPlay 和经网页会得到不同的响度。
//
// 曲线: -30dB..0dB 线性映射到 0..1, 再平方 (感知上更接近线性)。
int32_t airplay_volume_db_to_q15(float volume_db) {
  if (volume_db <= -30.0f) {
    return 0;
  }
  if (volume_db >= 0.0f) {
    return 32768;
  }
  float normalized = (volume_db + 30.0f) / 30.0f;
  float curved = normalized * normalized;
  return (int32_t)(curved * 32768.0f);
}

float airplay_q15_to_volume_db(int32_t q15) {
  if (q15 <= 0) {
    return -30.0f;
  }
  if (q15 >= 32768) {
    return 0.0f;
  }
  float curved = (float)q15 / 32768.0f;
  float normalized = sqrtf(curved);
  return normalized * 30.0f - 30.0f;
}

// 音量的**权威副本在 settings 的 RAM 缓存里** (g_volume_db)，这里只是它换算好的
// Q15 形式，专供音频热路径读。
//
// ⚠️ 音量以前是存在 rtsp_conn_t 里、由这两个访问器隔着任务解引用 conn 取的，
//    而 conn 属于那条 RTSP 连接、会在断开时被 free —— airplay_get_volume_q15()
//    却要被 audio_output.c 的 apply_volume() **每帧**调用一次（优先级 9），
//    于是"正常断开 AirPlay 的那一瞬间"就是一个 use-after-free 窗口。
//    现在跨任务读的只是一个对齐的 32 位标量（ESP32 上存取本身是原子的）。
//
// 初值 = airplay_volume_db_to_q15(-15.0f) = 8192：明显的保守音量。用字面量而不
// 是调函数，是因为 playback_task 可能在 server_task 跑到开机刷新之前就先读一次。
static volatile int32_t s_volume_q15 = 8192;

// 网页设过音量后置位。AirPlay 连接建立时据此把网页设的值同步给对方。
static volatile bool s_volume_synced_to_airplay = true;
static volatile float s_last_set_volume_db = 0.0f;

// 应用一个音量值：换算 + 缓存 + 落 settings（settings_set_volume 顺带推给 DAC）。
// 全项目**只有这一个**写音量的入口。
void airplay_apply_volume_db(float volume_db) {
  s_volume_q15 = airplay_volume_db_to_q15(volume_db);
  s_last_set_volume_db = volume_db;
  settings_set_volume(volume_db);
}

// Public API for volume control (硬件按键走这里)
void airplay_set_volume(float volume_db) {
  client_slot_t *c = &clients[current_slot];
  if (c->has_session && !c->is_old) {
    airplay_apply_volume_db(volume_db);
  }
}

void airplay_set_volume_q15(int32_t q15) {
  float db = airplay_q15_to_volume_db(q15);

  client_slot_t *c = &clients[current_slot];
  if (c->has_session && !c->is_old) {
    // 有活跃 AirPlay 连接就同步过去 (顺带更新 q15 缓存)
    airplay_apply_volume_db(db);
  } else {
    // 没有连接: 只更新 DAC + 缓存, 并记录待同步的值
    s_last_set_volume_db = db;
    s_volume_q15 = airplay_volume_db_to_q15(db);
    settings_set_volume(db);
    s_volume_synced_to_airplay = false;
  }
}

bool airplay_take_volume_pending_sync(void) {
  if (s_volume_synced_to_airplay) {
    return false;
  }
  s_volume_synced_to_airplay = true;
  return true;
}

float airplay_get_pending_volume_db(void) { return s_last_set_volume_db; }

// ⚠️ 这是**每帧**都会走的路径 (audio_output.c 的 apply_volume)，所以只读标量。
//
// 以前这里分两支：有活跃 conn 就读 conn->volume_q15，没有就回读
// settings_get_volume()。两支的值其实**恒等** —— 每次写音量都同时落在
// settings 的 RAM 副本和这份缓存里，所以下面合并成一次标量读取。
//
// 冷启动默认 -15dB → Q15≈8192，是明显压低过的音量，不会一开机就满音量炸出来。
int32_t airplay_get_volume_q15(void) { return s_volume_q15; }

// 当前音量的 dB 形式。权威副本在 settings 的 RAM 缓存里 (g_volume_db)。
// NVS 里还没有值时沿用 -15dB 的保守默认 —— 与 rtsp_conn_create 以前为 conn
// 播种音量的行为一致。
float airplay_get_volume_db(void) {
  float db = -15.0f;
  if (settings_get_volume(&db) != ESP_OK) {
    db = -15.0f; // NVS 里还没有值, 用保守默认
  }
  return db;
}

void rtsp_server_request_resume(void) {
  s_resume_requested = true;
}

// Helper to grow buffer
//
// Allocates new_size + 1: `new_size` is the *usable* capacity every caller
// tracks as buf_capacity, and process_rtsp_buffer() writes a one-byte NUL
// terminator at buffer[total_len] where total_len may legitimately reach
// buf_capacity.  The extra byte keeps that write inside the block.
static uint8_t *grow_buffer(uint8_t *old_buf, size_t old_size, size_t new_size,
                            size_t data_len) {
  (void)old_size;
  uint8_t *new_buf =
      heap_caps_malloc(new_size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!new_buf) {
    new_buf = malloc(new_size + 1);
  }
  if (!new_buf) {
    return NULL;
  }
  if (old_buf && data_len > 0) {
    memcpy(new_buf, old_buf, data_len);
  }
  free(old_buf);
  return new_buf;
}

// Process buffered RTSP requests
static void process_rtsp_buffer(client_slot_t *slot, uint8_t *buffer,
                                size_t *buf_len) {
  while (*buf_len > 0 && !slot->should_stop) {
    const uint8_t *header_end = rtsp_find_header_end(buffer, *buf_len);
    if (!header_end) {
      break;
    }

    size_t header_len = (size_t)(header_end - buffer) + 4;
    char *header_str = malloc(header_len + 1);
    if (!header_str) {
      *buf_len = 0;
      break;
    }
    memcpy(header_str, buffer, header_len);
    header_str[header_len] = '\0';

    int content_len = rtsp_parse_content_length(header_str);
    if (content_len < 0) {
      content_len = 0;
    }

    size_t total_len = header_len + (size_t)content_len;
    if (total_len > RTSP_BUFFER_LARGE || *buf_len < total_len) {
      free(header_str);
      if (total_len > RTSP_BUFFER_LARGE) {
        *buf_len = 0;
      }
      break;
    }

    // Null-terminate so strcasestr in parse_raw_header won't read past
    // the message boundary.  total_len is bounded by buf_capacity (recv never
    // writes past it), and the block is allocated one byte larger than that.
    uint8_t saved = buffer[total_len];
    buffer[total_len] = '\0';
    rtsp_dispatch(slot->socket, slot->conn, buffer, total_len);
    buffer[total_len] = saved;
    free(header_str);

    if (*buf_len > total_len) {
      memmove(buffer, buffer + total_len, *buf_len - total_len);
    }
    *buf_len -= total_len;
  }
}

// Client task
static void client_task(void *pvParameters) {
  int slot_idx = (int)(intptr_t)pvParameters;
  client_slot_t *slot = &clients[slot_idx];

  // Create connection state
  rtsp_conn_t *conn = rtsp_conn_create();
  if (!conn) {
    ESP_LOGE(TAG, "Failed to create connection state");
    close(slot->socket);
    slot->socket = -1;
    slot->task = NULL;
    vTaskDelete(NULL);
    return;
  }
  // 若网页设过音量, 而这次 AirPlay 连接建立前没有活跃会话, 就把网页那个值同步
  // 过去。否则手机会用**它自己记的**音量覆盖掉网页的设置 —— 用户会觉得"网页上
  // 拖了半天, 一连 AirPlay 就变回去了"。
  if (airplay_take_volume_pending_sync()) {
    float pending_db = airplay_get_pending_volume_db();
    airplay_apply_volume_db(pending_db);
    ESP_LOGI(TAG, "已把网页音量 %.1f dB 同步到新 AirPlay 连接", pending_db);
  }

  // Get client IP address for timing requests
  struct sockaddr_in peer_addr;
  socklen_t peer_len = sizeof(peer_addr);
  if (getpeername(slot->socket, (struct sockaddr *)&peer_addr, &peer_len) ==
      0) {
    conn->client_ip = peer_addr.sin_addr.s_addr;
    ESP_LOGI(TAG, "Client IP: %u.%u.%u.%u",
             (unsigned int)(conn->client_ip & 0xFF),
             (unsigned int)((conn->client_ip >> 8) & 0xFF),
             (unsigned int)((conn->client_ip >> 16) & 0xFF),
             (unsigned int)((conn->client_ip >> 24) & 0xFF));
  }

  // ⚠️ 发布必须在 conn 完全初始化之后：slot->conn / has_session 一旦被别的任务
  //    (web_server、按键、playback_task) 看到，conn 就成了跨任务共享对象。
  slot->conn = conn;
  slot->has_session = true;

  // Allocate buffer — one byte past buf_capacity for the framing terminator
  // (see grow_buffer).
  size_t buf_capacity = RTSP_BUFFER_INITIAL;
  uint8_t *buffer = malloc(buf_capacity + 1);
  if (!buffer) {
    ESP_LOGE(TAG, "Failed to allocate buffer");
    // 先收回可见性，再 free (顺序反了就是 UAF)
    slot->has_session = false;
    slot->conn = NULL;
    rtsp_conn_free(conn);
    close(slot->socket);
    slot->socket = -1;
    slot->task = NULL;
    vTaskDelete(NULL);
    return;
  }

  size_t buf_len = 0;

  // Socket timeout for stop signal responsiveness
  struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
  setsockopt(slot->socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  // Disable Nagle: RTSP control commands (volume, pause) are tiny and must
  // not wait for coalescing/delayed-ACK, which adds tens to hundreds of ms of
  // latency to every command on this connection.
  int nodelay = 1;
  setsockopt(slot->socket, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

  while (server_running && !slot->should_stop) {
    if (conn->encrypted_mode) {
      // Encrypted mode
      while (server_running && conn->encrypted_mode && !slot->should_stop) {
        if (buf_len >= buf_capacity - 1024) {
          size_t new_cap = buf_capacity < RTSP_BUFFER_LARGE ? RTSP_BUFFER_LARGE
                                                            : buf_capacity * 2;
          if (new_cap > RTSP_BUFFER_LARGE) {
            goto cleanup;
          }
          uint8_t *new_buf =
              grow_buffer(buffer, buf_capacity, new_cap, buf_len);
          if (!new_buf) {
            goto cleanup;
          }
          buffer = new_buf;
          buf_capacity = new_cap;
        }

        int block_len = rtsp_crypto_read_block(
            slot->socket, conn, buffer + buf_len, buf_capacity - buf_len);
        if (block_len <= 0) {
          if (slot->should_stop || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            goto cleanup;
          }
          continue;
        }

        buf_len += (size_t)block_len;
        process_rtsp_buffer(slot, buffer, &buf_len);
      }
      goto cleanup;
    }

    // Plain-text mode
    if (buf_len >= buf_capacity - 1024) {
      size_t new_cap = buf_capacity < RTSP_BUFFER_LARGE ? RTSP_BUFFER_LARGE
                                                        : buf_capacity * 2;
      if (new_cap > RTSP_BUFFER_LARGE) {
        break;
      }
      uint8_t *new_buf = grow_buffer(buffer, buf_capacity, new_cap, buf_len);
      if (!new_buf) {
        break;
      }
      buffer = new_buf;
      buf_capacity = new_cap;
    }

    ssize_t recv_len =
        recv(slot->socket, buffer + buf_len, buf_capacity - buf_len, 0);
    if (recv_len <= 0) {
      if (recv_len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        continue;
      }
      break;
    }
    buf_len += (size_t)recv_len;
    process_rtsp_buffer(slot, buffer, &buf_len);
  }

cleanup:
  ESP_LOGI(TAG, "Client slot %d disconnected", slot_idx);
  free(buffer);
  close(slot->socket);
  slot->socket = -1;

  // Immediate: stop audio and NTP
  audio_receiver_stop();
  audio_output_flush();
  ntp_clock_stop();

  bool has_dacp_remote = conn && conn->protocol_version == 1 &&
                         conn->dacp_id[0] != '\0' &&
                         conn->active_remote[0] != '\0';

  // iOS v1 pause handling needs DACP to distinguish pause from disconnect.
  // Third-party RAOP clients often have no DACP remote; disconnect them
  // immediately instead of delaying slot cleanup with an iOS-only grace path.
  if (has_dacp_remote) {
    if (!slot->should_stop) {
      s_resume_requested = false;
      rtsp_events_emit(RTSP_EVENT_PAUSED, NULL);

      // Phase 1: let mDNS settle (3 s), but exit early on resume or reconnect
      for (int i = 0; i < 6 && !slot->should_stop; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (s_resume_requested) {
          ESP_LOGI(TAG,
                   "Resume requested during Phase 1 — skipping to Phase 2");
          break;
        }
      }

      // Phase 2: wait for reconnect as long as DACP service is advertised.
      // Re-probe every ~5 s. The phone unadvertises the service when the
      // user switches away, so disappearance = genuine disconnect.
      if (!slot->should_stop) {
        bool stay = dacp_probe_service() || s_resume_requested;
        if (s_resume_requested) {
          s_resume_requested = false;
          ESP_LOGI(TAG, "Resume requested via button — waiting for reconnect");
          stay = true;
        }
        if (stay) {
          ESP_LOGI(TAG, "DACP still advertised — waiting for reconnect");
        }
        while (stay && !slot->should_stop) {
          // Wait 5 s between probes (10 × 500 ms), checking flags each tick
          for (int i = 0; i < 10 && !slot->should_stop; i++) {
            vTaskDelay(pdMS_TO_TICKS(500));
            if (s_resume_requested) {
              s_resume_requested = false;
              ESP_LOGI(TAG,
                       "Resume requested via button — extending grace period");
            }
          }
          if (slot->should_stop) {
            break;
          }
          // Re-probe: still advertised?
          stay = dacp_probe_service();
          if (stay) {
            ESP_LOGD(TAG, "DACP still advertised — continuing wait");
          } else {
            ESP_LOGI(TAG, "DACP service gone — genuine disconnect");
          }
        }
      }

      if (slot->is_old) {
        // New client connected during grace period — treat as reconnect
        ESP_LOGI(TAG, "Client reconnected during grace period");
      } else {
        ESP_LOGI(TAG, "Grace period expired — full disconnect");
        dacp_clear_session();
        rtsp_events_emit(RTSP_EVENT_DISCONNECTED, NULL);
      }
    } else {
      // Forcefully stopped (server shutdown or replaced by new client)
      dacp_clear_session();
      rtsp_events_emit(RTSP_EVENT_DISCONNECTED, NULL);
    }
  } else {
    // v2 / unknown — no grace period, clear immediately.
    dacp_clear_session();
    rtsp_events_emit(RTSP_EVENT_DISCONNECTED, NULL);
  }

  // When being replaced by a new client (is_old), skip global state changes —
  // the new session's SETUP already manages PTP and the event port task.
  if (!slot->is_old) {
    ptp_clock_init(); // Restart PTP (stopped during v1 SETUP to free sockets)
    rtsp_stop_event_port_task();
  } else if (rtsp_event_port_listen_socket() >= 0 &&
             rtsp_event_port_listen_socket() == conn->event_socket) {
    // Old task still using our socket — stop it before closing
    rtsp_stop_event_port_task();
  }

  if (conn->event_socket >= 0) {
    close(conn->event_socket);
    conn->event_socket = -1;
  }

  rtsp_conn_cleanup(conn);

  // ⚠️ 顺序很重要：**先把 conn 从别的任务眼前收回来，再 free**。
  //    原来是 rtsp_conn_free() 在前、slot->conn = NULL 在后，中间那几行里
  //    web_server / 按键 / playback_task 读到的就是已释放的堆 —— 音量写路径
  //    原本还会顺着这个指针落到 conn 上并写 NVS。
  slot->has_session = false;
  slot->conn = NULL;
  rtsp_conn_free(conn);

  slot->socket = -1;
  slot->task = NULL;
  slot->should_stop = false;
  slot->is_old = false;

  vTaskDelete(NULL);
}

// Signal old client to stop (non-blocking)
static void signal_old_client_stop(int old_slot) {
  client_slot_t *old = &clients[old_slot];
  if (old->task == NULL) {
    return;
  }

  ESP_LOGI(TAG, "Signaling old client to stop");
  old->is_old = true;
  old->should_stop = true;

  // Shutdown socket to unblock recv
  if (old->socket >= 0) {
    shutdown(old->socket, SHUT_RDWR);
  }
  // Task will clean itself up
}

static void server_task(void *pvParameters) {
  (void)pvParameters;

  struct sockaddr_in server_addr, client_addr;
  socklen_t client_addr_len = sizeof(client_addr);

  // Initialize slots
  for (int i = 0; i < 2; i++) {
    clients[i].socket = -1;
    clients[i].conn = NULL;
    clients[i].task = NULL;
    clients[i].should_stop = false;
    clients[i].is_old = false;
    clients[i].has_session = false;
  }

  // 冷启动时把网页上次设的音量当成"待同步", 这样第一台连上来的设备
  // 不会用自己记的音量覆盖掉用户在网页上调好的值。
  float boot_db = -15.0f;
  settings_get_volume(&boot_db);
  s_last_set_volume_db = boot_db;
  s_volume_q15 = airplay_volume_db_to_q15(boot_db);
  s_volume_synced_to_airplay = false;

  server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server_socket < 0) {
    ESP_LOGE(TAG, "Failed to create socket: %d", errno);
    server_task_handle = NULL;
    vTaskDelete(NULL);
    return;
  }

  int opt = 1;
  setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_port = htons(RTSP_PORT);

  if (bind(server_socket, (struct sockaddr *)&server_addr,
           sizeof(server_addr)) < 0) {
    ESP_LOGE(TAG, "Failed to bind: %d", errno);
    close(server_socket);
    server_socket = -1;
    server_task_handle = NULL;
    vTaskDelete(NULL);
    return;
  }

  if (listen(server_socket, 5) < 0) {
    ESP_LOGE(TAG, "Failed to listen: %d", errno);
    close(server_socket);
    server_socket = -1;
    server_task_handle = NULL;
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGI(TAG, "RTSP server listening on port %d", RTSP_PORT);
  server_running = true;

  while (server_running) {
    int new_socket = accept(server_socket, (struct sockaddr *)&client_addr,
                            &client_addr_len);
    if (new_socket < 0) {
      if (server_running) {
        ESP_LOGE(TAG, "Failed to accept: %d", errno);
      }
      continue;
    }

    ESP_LOGI(TAG, "New client connected");

    // Find slot for new client (alternate between 0 and 1)
    int new_slot = 1 - current_slot;

    // If new slot still has a running task, wait for it to fully exit.
    // With static TCBs we MUST NOT reuse until the old task is deleted.
    if (clients[new_slot].task != NULL) {
      clients[new_slot].should_stop = true;
      if (clients[new_slot].socket >= 0) {
        shutdown(clients[new_slot].socket, SHUT_RDWR);
      }
      int timeout = 30; // 3 seconds max
      while (clients[new_slot].task != NULL && timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        timeout--;
      }
      if (clients[new_slot].task != NULL) {
        ESP_LOGE(TAG, "Slot %d task did not exit in time", new_slot);
        close(new_socket);
        continue;
      }
    }
    // Signal old client to stop (in background)
    signal_old_client_stop(current_slot);

    // Setup new slot
    clients[new_slot].socket = new_socket;
    clients[new_slot].should_stop = false;
    clients[new_slot].is_old = false;

    // Start new client task immediately.
    clients[new_slot].task = NULL;
    BaseType_t task_ret =
        xTaskCreate(client_task, "rtsp_client", CLIENT_STACK_SIZE,
                    (void *)(intptr_t)new_slot, 5, &clients[new_slot].task);
    if (task_ret != pdPASS || clients[new_slot].task == NULL) {
      ESP_LOGE(TAG, "Failed to create client task");
      close(new_socket);
      clients[new_slot].socket = -1;
    } else {
      current_slot = new_slot;
    }
  }

  // Stop all clients
  for (int i = 0; i < 2; i++) {
    if (clients[i].task != NULL) {
      clients[i].should_stop = true;
      if (clients[i].socket >= 0) {
        shutdown(clients[i].socket, SHUT_RDWR);
      }
    }
  }

  vTaskDelay(pdMS_TO_TICKS(500));

  if (server_socket >= 0) {
    close(server_socket);
    server_socket = -1;
  }

  server_task_handle = NULL;
  vTaskDelete(NULL);
}

static bool rtsp_server_wait_for_task_stopped(int timeout_ticks) {
  while (server_task_handle != NULL && timeout_ticks-- > 0) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return server_task_handle == NULL;
}

esp_err_t rtsp_server_start(void) {
  if (server_task_handle != NULL) {
    if (server_running) {
      return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGW(TAG, "RTSP server task still stopping, waiting");
    if (!rtsp_server_wait_for_task_stopped(40)) {
      ESP_LOGE(TAG, "Previous RTSP server task did not stop");
      return ESP_ERR_INVALID_STATE;
    }
  }

  BaseType_t task_ret =
      xTaskCreate(server_task, "rtsp_server", SERVER_STACK_SIZE, NULL, 5,
                  &server_task_handle);
  if (task_ret != pdPASS || server_task_handle == NULL) {
    return ESP_FAIL;
  }

  return ESP_OK;
}

void rtsp_server_stop(void) {
  server_running = false;

  if (server_socket >= 0) {
    shutdown(server_socket, SHUT_RDWR);
    close(server_socket);
    server_socket = -1;
  }

  if (server_task_handle != NULL) {
    if (!rtsp_server_wait_for_task_stopped(40)) {
      ESP_LOGW(TAG, "RTSP server task did not exit within timeout");
    }
  }
}
