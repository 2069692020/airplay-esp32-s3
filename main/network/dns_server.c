#include "dns_server.h"
#include "spiram_task.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "dns_server";

#define DNS_PORT    53
#define DNS_MAX_LEN 512

// DNS header structure
typedef struct __attribute__((packed)) {
  uint16_t id;
  uint16_t flags;
  uint16_t qdcount;
  uint16_t ancount;
  uint16_t nscount;
  uint16_t arcount;
} dns_header_t;

static int s_dns_socket = -1;
static TaskHandle_t s_dns_task = NULL;
static uint32_t s_redirect_ip = 0;

static void dns_server_task(void *pvParameters) {
  uint8_t rx_buffer[DNS_MAX_LEN];
  uint8_t tx_buffer[DNS_MAX_LEN];
  struct sockaddr_in client_addr;
  socklen_t addr_len = sizeof(client_addr);

  ESP_LOGI(TAG, "DNS server task started");

  while (1) {
    int len = recvfrom(s_dns_socket, rx_buffer, sizeof(rx_buffer), 0,
                       (struct sockaddr *)&client_addr, &addr_len);
    if (len < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      ESP_LOGE(TAG, "recvfrom failed: %d", errno);
      break;
    }

    if (len < (int)sizeof(dns_header_t)) {
      continue;
    }

    dns_header_t *req_header = (dns_header_t *)rx_buffer;

    // Build response
    memcpy(tx_buffer, rx_buffer, len);
    dns_header_t *resp_header = (dns_header_t *)tx_buffer;

    // Set response flags: QR=1 (response), AA=1 (authoritative), RCODE=0 (no
    // error)
    resp_header->flags = htons(0x8400);
    // ancount 在答案真正编完之后才填 (见下面)

    int resp_len = len;

    // Add answer section for each question
    uint16_t qdcount = ntohs(req_header->qdcount);
    const uint8_t * const rx_end = rx_buffer + len;
    uint8_t *ptr = rx_buffer + sizeof(dns_header_t);
    int answers = 0;

    for (int i = 0; i < qdcount && resp_len < DNS_MAX_LEN - 16; i++) {
      // 跳过问题名: 一串 [长度字节][标签] ，以 0 结尾。
      // ⚠️ 每走一步都要先确认**还要读的那个字节**在包内 —— 长度字节本身来自
      //    网络，`ptr += *ptr + 1` 一步就能跳出缓冲区；原来写成
      //    `while (*ptr != 0 && ptr < rx_buffer + len)` 是**先解引用再判界**，
      //    判界形同虚设。指针跑出栈缓冲区就是硬崩。
      while (ptr < rx_end && *ptr != 0) {
        size_t step = (size_t)*ptr + 1u;
        if ((size_t)(rx_end - ptr) <= step) {
          ptr = (uint8_t *)rx_end; // 明确推到终点, 下面一条判定就会放弃这个包
          break;
        }
        ptr += step;
      }
      // 名字结束符 + QTYPE(2) + QCLASS(2)
      if ((size_t)(rx_end - ptr) < 5) {
        break; // 包被截断/格式不对: 剩下的问题不再编答案
      }
      ptr += 5;

      // Add answer: pointer to question name, type A, class IN, TTL, IP
      uint8_t *ans = tx_buffer + resp_len;
      ans[0] = 0xC0; // Pointer to offset 12 (question name)
      ans[1] = 0x0C;
      ans[2] = 0x00; // Type A
      ans[3] = 0x01;
      ans[4] = 0x00; // Class IN
      ans[5] = 0x01;
      ans[6] = 0x00; // TTL (60 seconds)
      ans[7] = 0x00;
      ans[8] = 0x00;
      ans[9] = 0x3C;
      ans[10] = 0x00; // RDLENGTH (4 bytes for IPv4)
      ans[11] = 0x04;
      // IP address (already in network byte order)
      memcpy(&ans[12], &s_redirect_ip, 4);
      resp_len += 16;
      answers++;
    }

    // 真正的答案数 (上面的循环可能提前 break)。以前这里直接把请求的 qdcount
    // 抄进 ancount 就发出去，一旦提前退出，响应就宣称了比实际更多的 RR。
    resp_header->ancount = htons((uint16_t)answers);

    sendto(s_dns_socket, tx_buffer, resp_len, 0,
           (struct sockaddr *)&client_addr, addr_len);
  }

  ESP_LOGI(TAG, "DNS server task exiting");
  vTaskDelete(NULL);
}

esp_err_t dns_server_start(uint32_t redirect_ip) {
  if (s_dns_socket >= 0) {
    ESP_LOGW(TAG, "DNS server already running");
    return ESP_OK;
  }

  s_redirect_ip = redirect_ip;

  s_dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s_dns_socket < 0) {
    ESP_LOGE(TAG, "Failed to create socket: %d", errno);
    return ESP_FAIL;
  }

  // Set socket timeout
  struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
  setsockopt(s_dns_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  // Allow address reuse
  int opt = 1;
  setsockopt(s_dns_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in server_addr = {
      .sin_family = AF_INET,
      .sin_port = htons(DNS_PORT),
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };

  if (bind(s_dns_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) <
      0) {
    ESP_LOGE(TAG, "Failed to bind socket: %d", errno);
    close(s_dns_socket);
    s_dns_socket = -1;
    return ESP_FAIL;
  }

  task_create_spiram(dns_server_task, "dns_server", 4096, NULL, 5, &s_dns_task,
                     NULL);

  ESP_LOGI(TAG, "DNS server started, redirecting to " IPSTR,
           IP2STR((esp_ip4_addr_t *)&redirect_ip));
  return ESP_OK;
}

void dns_server_stop(void) {
  if (s_dns_socket >= 0) {
    close(s_dns_socket);
    s_dns_socket = -1;
  }
  if (s_dns_task) {
    // Task will exit on socket close
    s_dns_task = NULL;
  }
  ESP_LOGI(TAG, "DNS server stopped");
}
