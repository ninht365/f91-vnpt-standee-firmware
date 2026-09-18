/**
 * @file web_server.h
 * @brief Module dung HTTP server phuc vu trang cau hinh WiFi va REST API
 *        de trang web giao tiep voi firmware.
 *
 * Cac endpoint cung cap:
 *   GET  /                 -> Trang HTML cau hinh
 *   GET  /api/scan         -> Quet WiFi xung quanh, tra ve JSON danh sach SSID
 *   GET  /api/status       -> Trang thai ket noi hien tai (JSON)
 *   POST /api/save         -> Nhan {ssid, password} tu form, luu NVS + ket noi
 *   *    (404 khac)        -> Redirect ve "/" (ho tro captive portal)
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Khoi dong HTTP server. Goi sau khi wifi_manager da bat AP Mode
 *        (SoftAP da len) de client co the ket noi vao AP va truy cap ngay.
 */
esp_err_t web_server_start(void);

/**
 * @brief Dung HTTP server (giai phong tai nguyen). Goi khi thoat AP Mode.
 */
void web_server_stop(void);

#ifdef __cplusplus
}
#endif