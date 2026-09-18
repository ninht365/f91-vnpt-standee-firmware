/**
 * @file nvs_storage.h
 * @brief Module phụ trách LƯU / ĐỌC / XOÁ thông tin WiFi (SSID, password)
 *        trong bộ nhớ NVS (Non-Volatile Storage) - vùng flash không mất dữ
 *        liệu khi mất điện / restart.
 *
 * Đây là phần trả lời cho yêu cầu "Thực hiện lưu thông tin WiFi vào NVS" và
 * là nền tảng để làm "tự động kết nối lại sau khi khởi động lại thiết bị".
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NVS_WIFI_SSID_MAX_LEN     32   /* Chuẩn IEEE802.11 SSID toi da 32 byte */
#define NVS_WIFI_PASS_MAX_LEN     64   /* Chuan WPA2 password toi da 64 byte */

/** Cấu trúc chứa thông tin đăng nhập WiFi đọc/ghi từ NVS */
typedef struct {
    char ssid[NVS_WIFI_SSID_MAX_LEN + 1];
    char password[NVS_WIFI_PASS_MAX_LEN + 1];
} wifi_credentials_t;

/**
 * @brief Khởi tạo NVS flash. Bắt buộc gọi 1 lần trong app_main() trước khi
 *        gọi bất kỳ hàm nào khác trong module này hoặc dùng esp_wifi.
 *        Tự động format lại NVS nếu phát hiện phiên bản NVS cũ/không tương
 *        thích (ví dụ sau khi đổi partition table hoặc đổi phiên bản IDF).
 */
esp_err_t nvs_storage_init(void);

/**
 * @brief Lưu SSID + password vào NVS (namespace riêng "wifi_cfg").
 * @param ssid      Chuỗi SSID, tối đa NVS_WIFI_SSID_MAX_LEN ký tự.
 * @param password  Chuỗi password, tối đa NVS_WIFI_PASS_MAX_LEN ký tự.
 * @return ESP_OK nếu thành công.
 */
esp_err_t nvs_storage_save_wifi_credentials(const char *ssid, const char *password);

/**
 * @brief Đọc SSID + password đã lưu (nếu có) từ NVS.
 * @param out  Con trỏ tới struct sẽ được điền dữ liệu.
 * @return ESP_OK nếu đọc thành công và có dữ liệu hợp lệ.
 *         ESP_ERR_NVS_NOT_FOUND nếu chưa từng lưu (chưa provisioning lần nào).
 */
esp_err_t nvs_storage_load_wifi_credentials(wifi_credentials_t *out);

/**
 * @brief Kiểm tra nhanh xem đã có thông tin WiFi được lưu trong NVS chưa,
 *        không cần đọc ra toàn bộ nội dung.
 */
bool nvs_storage_has_wifi_credentials(void);

/**
 * @brief Xoá thông tin WiFi đã lưu (dùng cho tính năng "Reset cấu hình" /
 *        nút nhấn factory-reset nếu sau này muốn bổ sung).
 */
esp_err_t nvs_storage_erase_wifi_credentials(void);

#ifdef __cplusplus
}
#endif