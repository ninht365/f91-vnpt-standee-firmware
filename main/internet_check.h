/**
 * @file internet_check.h
 * @brief Kiem tra ket noi Internet bang ICMP ping.
 *
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
