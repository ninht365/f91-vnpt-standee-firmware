/**
 * @file nvs_storage.c
 * @brief Xem mô tả chi tiết trong nvs_storage.h
 */
#include "nvs_storage.h"

#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "nvs_storage";

/* Namespace + key dùng riêng cho module này, tách biệt khỏi namespace mặc
 * định "nvs" mà esp_wifi driver tự dùng để lưu cache nội bộ của nó. */
#define WIFI_NVS_NAMESPACE      "wifi_cfg"
#define KEY_SSID                "ssid"
#define KEY_PASSWORD            "password"

esp_err_t nvs_storage_init(void)
{
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Partition NVS bị đầy hoặc không tương thích (thường gặp sau khi
         * flash lại partition table khác, hoặc lần đầu dùng board mới) ->
         * xoá trắng và khởi tạo lại. Dữ liệu cũ (nếu có) sẽ mất, đây là
         * hành vi chuẩn được khuyến nghị bởi ESP-IDF. */
        ESP_LOGW(TAG, "NVS can loi/khong tuong thich, dang xoa va khoi tao lai...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "Khoi tao NVS thanh cong");
    return ESP_OK;
}

esp_err_t nvs_storage_save_wifi_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL || strlen(ssid) == 0 || strlen(ssid) > NVS_WIFI_SSID_MAX_LEN) {
        ESP_LOGE(TAG, "SSID khong hop le (rong hoac > %d ky tu)", NVS_WIFI_SSID_MAX_LEN);
        return ESP_ERR_INVALID_ARG;
    }
    if (password != NULL && strlen(password) > NVS_WIFI_PASS_MAX_LEN) {
        ESP_LOGE(TAG, "Password qua dai (> %d ky tu)", NVS_WIFI_PASS_MAX_LEN);
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open that bai: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, KEY_SSID, ssid);
    if (err == ESP_OK) {
        /* password co the la chuoi rong "" neu la mang mo, van hop le */
        err = nvs_set_str(handle, KEY_PASSWORD, password != NULL ? password : "");
    }

    if (err == ESP_OK) {
        err = nvs_commit(handle); /* Bat buoc commit() de du lieu thuc su ghi xuong flash */
    }

    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Da luu WiFi credentials vao NVS (SSID: %s)", ssid);
    } else {
        ESP_LOGE(TAG, "Loi khi luu WiFi credentials: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t nvs_storage_load_wifi_credentials(wifi_credentials_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        /* Truong hop pho bien nhat: chua tung provisioning lan nao nen
         * namespace "wifi_cfg" chua ton tai -> ESP_ERR_NVS_NOT_FOUND */
        return err;
    }

    size_t ssid_len = sizeof(out->ssid);
    err = nvs_get_str(handle, KEY_SSID, out->ssid, &ssid_len);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    size_t pass_len = sizeof(out->password);
    err = nvs_get_str(handle, KEY_PASSWORD, out->password, &pass_len);
    /* Neu khong co password (mang mo duoc luu truoc khi co field password)
     * thi coi nhu password rong, khong coi la loi. */
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        out->password[0] = '\0';
        err = ESP_OK;
    }

    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Da doc WiFi credentials tu NVS (SSID: %s)", out->ssid);
    }
    return err;
}

bool nvs_storage_has_wifi_credentials(void)
{
    wifi_credentials_t tmp;
    return nvs_storage_load_wifi_credentials(&tmp) == ESP_OK && strlen(tmp.ssid) > 0;
}

esp_err_t nvs_storage_erase_wifi_credentials(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    ESP_LOGI(TAG, "Da xoa WiFi credentials trong NVS: %s", esp_err_to_name(err));
    return err;
}