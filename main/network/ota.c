#include "ota.h"

#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include <inttypes.h>
#include <string.h>
#include <sys/param.h>

static const char *TAG = "ota";

// 一次收包最多允许多久没有进展。esp_http_server 默认所有 URI 都在**同一个
// httpd 任务**里跑，所以这里如果无限重试，客户端半路挂住就会把整个网页服务
// 冻死（不只是这个接口）。上游那版是 `if (timeout) continue;` 没有上限。
#define OTA_RECV_STALL_MS 5000

// 同一时刻只允许一个升级在跑（httpd 单任务下已经是串行的，这条防的是将来
// 换成多 worker 或从别的任务再调一次）。
static volatile bool s_ota_busy;

static bool ota_acquire(void) {
  if (s_ota_busy) {
    return false;
  }
  s_ota_busy = true;
  return true;
}

/**
 * 校验内存里的镜像头部（真正的全镜像校验由 esp_ota_end() 做）。
 * 检查：最小长度、magic、段数，以及镜像自带 hash 时的 SHA-256。
 */
static esp_err_t ota_validate_image(const uint8_t *image, size_t len) {
  if (len < sizeof(esp_image_header_t)) {
    ESP_LOGE(TAG, "镜像太小 (%zu 字节)", len);
    return ESP_ERR_INVALID_SIZE;
  }

  const esp_image_header_t *header = (const esp_image_header_t *)image;

  if (header->magic != ESP_IMAGE_HEADER_MAGIC) {
    ESP_LOGE(TAG, "镜像 magic 不对: 0x%02x (期望 0x%02x)", header->magic,
             ESP_IMAGE_HEADER_MAGIC);
    return ESP_ERR_INVALID_STATE;
  }

  if (header->segment_count == 0 ||
      header->segment_count > ESP_IMAGE_MAX_SEGMENTS) {
    ESP_LOGE(TAG, "段数不对: %u", header->segment_count);
    return ESP_ERR_INVALID_STATE;
  }

  if (header->hash_appended) {
    if (len < 32) {
      ESP_LOGE(TAG, "声明带 hash 但镜像不足 32 字节");
      return ESP_ERR_INVALID_SIZE;
    }
    // SHA-256 覆盖除最后 32 字节以外的全部内容
    size_t data_len = len - 32;
    const uint8_t *expected_hash = image + data_len;

    uint8_t computed_hash[32];
    mbedtls_sha256(image, data_len, computed_hash, 0);

    if (memcmp(computed_hash, expected_hash, 32) != 0) {
      ESP_LOGE(TAG, "SHA-256 不匹配 —— 固件已损坏");
      return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "SHA-256 校验通过");
  }

  return ESP_OK;
}

/**
 * 收满 fw_size 字节。返回 ESP_ERR_TIMEOUT 表示超过 OTA_RECV_STALL_MS 没有
 * 任何进展 —— 调用方必须放弃并释放缓冲。
 */
static esp_err_t ota_recv_all(httpd_req_t *req, uint8_t *buf, size_t fw_size,
                              size_t *received_out) {
  size_t received = 0;
  int64_t last_progress_us = esp_timer_get_time();

  while (received < fw_size) {
    int recv_len = httpd_req_recv(req, (char *)buf + received,
                                  fw_size - received);

    if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
      int64_t now_us = esp_timer_get_time();
      if ((now_us - last_progress_us) > (int64_t)OTA_RECV_STALL_MS * 1000) {
        ESP_LOGE(TAG, "接收停滞超过 %d ms（已收 %zu/%zu），放弃",
                 OTA_RECV_STALL_MS, received, fw_size);
        return ESP_ERR_TIMEOUT;
      }
      // 让出 CPU：这个任务还负责整个网页服务
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (recv_len <= 0) {
      ESP_LOGE(TAG, "接收错误: %d", recv_len);
      return ESP_FAIL;
    }

    received += (size_t)recv_len;
    last_progress_us = esp_timer_get_time();
  }

  *received_out = received;
  return ESP_OK;
}

/**
 * 先把整个镜像收进 PSRAM、验完再写 flash（失败不会碰活动槽）。
 * PSRAM 分配不到时返回 ESP_ERR_NO_MEM，由调用方退回流式写法。
 */
static esp_err_t ota_buffered(httpd_req_t *req,
                              const esp_partition_t *ota_partition) {
  size_t fw_size = req->content_len;

  uint8_t *fw_buf = heap_caps_malloc(fw_size, MALLOC_CAP_SPIRAM);
  if (!fw_buf) {
    ESP_LOGW(TAG, "PSRAM 里分配不出 %zu 字节", fw_size);
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(TAG, "接收固件到 PSRAM (%zu 字节)...", fw_size);
  size_t received = 0;
  esp_err_t err = ota_recv_all(req, fw_buf, fw_size, &received);
  if (err != ESP_OK) {
    heap_caps_free(fw_buf);
    return err;
  }

  // 落地前先验
  err = ota_validate_image(fw_buf, fw_size);
  if (err != ESP_OK) {
    heap_caps_free(fw_buf);
    return err;
  }

  esp_ota_handle_t ota_handle;
  err = esp_ota_begin(ota_partition, fw_size, &ota_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_begin 失败: %s", esp_err_to_name(err));
    heap_caps_free(fw_buf);
    return err;
  }

  // 4 KB 一片写，避免大镜像一次写完触发看门狗
  size_t offset = 0;
  while (offset < fw_size) {
    size_t chunk = MIN(fw_size - offset, 4096);
    if (esp_ota_write(ota_handle, fw_buf + offset, chunk) != ESP_OK) {
      ESP_LOGE(TAG, "flash 写入失败，偏移 %zu", offset);
      esp_ota_abort(ota_handle);
      heap_caps_free(fw_buf);
      return ESP_FAIL;
    }
    offset += chunk;
  }
  heap_caps_free(fw_buf);

  if (esp_ota_end(ota_handle) != ESP_OK) {
    ESP_LOGE(TAG, "镜像校验失败 (esp_ota_end)");
    return ESP_FAIL;
  }

  if (esp_ota_set_boot_partition(ota_partition) != ESP_OK) {
    ESP_LOGE(TAG, "设置启动槽失败");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "OTA 成功（buffered）→ 槽 %s", ota_partition->label);
  return ESP_OK;
}

/**
 * 直接流式写 flash（PSRAM 不够时的退路）。没有前置校验，
 * 坏镜像会在 esp_ota_end() 被拦下，不会成为启动槽。
 */
static esp_err_t ota_streaming(httpd_req_t *req,
                               const esp_partition_t *ota_partition) {
  esp_ota_handle_t ota_handle;
  esp_err_t err = esp_ota_begin(ota_partition, OTA_SIZE_UNKNOWN, &ota_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_begin 失败: %s", esp_err_to_name(err));
    return err;
  }

  uint8_t buf[1024];
  size_t remaining = req->content_len;
  int64_t last_progress_us = esp_timer_get_time();
  ESP_LOGI(TAG, "流式接收固件 (%zu 字节)...", remaining);

  while (remaining > 0) {
    int recv_len =
        httpd_req_recv(req, (char *)buf, MIN(remaining, sizeof(buf)));

    if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
      int64_t now_us = esp_timer_get_time();
      if ((now_us - last_progress_us) > (int64_t)OTA_RECV_STALL_MS * 1000) {
        ESP_LOGE(TAG, "接收停滞超时，放弃");
        esp_ota_abort(ota_handle);
        return ESP_ERR_TIMEOUT;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (recv_len <= 0) {
      ESP_LOGE(TAG, "接收错误: %d", recv_len);
      esp_ota_abort(ota_handle);
      return ESP_FAIL;
    }

    if (esp_ota_write(ota_handle, buf, recv_len) != ESP_OK) {
      ESP_LOGE(TAG, "flash 写入失败");
      esp_ota_abort(ota_handle);
      return ESP_FAIL;
    }

    remaining -= (size_t)recv_len;
    last_progress_us = esp_timer_get_time();
  }

  if (esp_ota_end(ota_handle) != ESP_OK) {
    ESP_LOGE(TAG, "镜像校验失败");
    return ESP_FAIL;
  }

  if (esp_ota_set_boot_partition(ota_partition) != ESP_OK) {
    ESP_LOGE(TAG, "设置启动槽失败");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "OTA 成功（streaming）→ 槽 %s", ota_partition->label);
  return ESP_OK;
}

esp_err_t ota_start_from_http(httpd_req_t *req) {
  if (!req || req->content_len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  if (!ota_acquire()) {
    ESP_LOGW(TAG, "已有升级在跑，拒绝这次请求");
    return ESP_ERR_INVALID_STATE;
  }

  esp_err_t err = ESP_FAIL;
  const char *why = "";

  // 先拿到目标槽，再用它的容量去卡 Content-Length —— 顺序反了就会先按一个
  // 攻击者可控的长度去 malloc。
  const esp_partition_t *ota_partition =
      esp_ota_get_next_update_partition(NULL);
  if (!ota_partition) {
    why = "分区表里没有可写的 OTA 槽（是不是还在用单 app 的无 OTA 表？）";
    ESP_LOGE(TAG, "%s", why);
    err = ESP_ERR_NOT_FOUND;
  } else if (req->content_len > ota_partition->size) {
    why = "固件比 OTA 槽还大";
    ESP_LOGE(TAG, "镜像 %zu 字节 > 槽 %s 的 %" PRIu32 " 字节",
             req->content_len, ota_partition->label, ota_partition->size);
    err = ESP_ERR_INVALID_SIZE;
  } else {
    // 优先 RAM 缓冲（写之前能验），PSRAM 不够再退回流式。
    err = ota_buffered(req, ota_partition);
    if (err == ESP_ERR_NO_MEM) {
      ESP_LOGW(TAG, "PSRAM 不够，改用流式写入（少了写前校验这一步）");
      err = ota_streaming(req, ota_partition);
    }
  }

  // 成功路径调用方马上 esp_restart()，这里清不清都无所谓；失败路径必须清，
  // 否则一次超时就把以后所有升级都锁死。
  s_ota_busy = false;

  if (err != ESP_OK && why[0]) {
    ESP_LOGW(TAG, "OTA 未执行: %s", why);
  }
  return err;
}
