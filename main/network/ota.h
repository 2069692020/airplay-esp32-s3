#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * 把 HTTP POST 请求体里的原始固件镜像写进下一个 OTA 槽。
 *
 * 不会重启 —— 调用方先发响应，再自己 esp_restart()。
 * 口令校验、"本地播放中不许升级"这些**策略**也归调用方
 * (web_server.c: ota_update_handler)，这里只管收、验、写。
 *
 * @param req 带 Content-Length 的 POST 请求
 * @return ESP_OK 成功；ESP_ERR_HTTP_SERVER_... / ESP_FAIL 失败；
 *         ESP_ERR_INVALID_STATE 已有另一个升级在跑
 */
esp_err_t ota_start_from_http(httpd_req_t *req);
