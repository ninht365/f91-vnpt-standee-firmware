/**
 * @file internet_check.h
 * @brief Kiem tra ket noi Internet thuc su bang ICMP ping.
 *
 * Sau khi ESP32 nhan duoc IP tu router, viec chi co IP khong dam bao
 * co Internet (router co the khong co WAN). Module nay ping 1.1.1.1
 * (Cloudflare) de xac nhan co ket noi Internet thuc su.
 *
 * Neu ping that bai, tu dong thu lai sau CONFIG_INTERNET_CHECK_RETRY_SEC giay.
 * Neu ping thanh cong, goi callback(true) va dung retry.
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Callback duoc goi khi kiem tra Internet hoan thanh */
typedef void (*internet_check_cb_t)(bool internet_ok);

/**
 * @brief Bat dau kiem tra Internet (ping khong dong bo).
 * @param cb Callback nhan ket qua true/false.
 * @return ESP_OK neu ping session duoc tao thanh cong.
 */
esp_err_t internet_check_start(internet_check_cb_t cb);

/**
 * @brief Dung kiem tra Internet (huy ping dang chay va retry timer).
 */
void internet_check_stop(void);

#ifdef __cplusplus
}
#endif
