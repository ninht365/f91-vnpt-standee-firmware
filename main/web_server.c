/**
 * @file web_server.c
 * @brief Xem mô tả các endpoint trong web_server.h
 */
#include "web_server.h"
#include "wifi_manager.h"
#include "nvs_storage.h"

#include <string.h>
#include <stdlib.h>
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "cJSON.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"

/* Fallback neu Kconfig chua dinh nghia */
#ifndef CONFIG_PROV_AP_IP
#define CONFIG_PROV_AP_IP "192.168.4.1"
#endif

#ifndef CONFIG_PROV_AP_SSID
#define CONFIG_PROV_AP_SSID "ESP32_WiFi_Setup"
#endif

#ifndef CONFIG_PROV_AP_PASSWORD
#define CONFIG_PROV_AP_PASSWORD "12345678"
#endif

#include "freertos/task.h"

static const char *TAG = "web_server";

/* Handle toi HTTP server hien tai, NULL neu server chua chay */
static httpd_handle_t s_server = NULL;

/* File main/web/index.html duoc nhung thang vao firmware luc build nho
 * khai bao EMBED_FILES trong main/CMakeLists.txt. Trinh linker se tu tao
 * 2 bien nay danh dau diem dau/cuoi cua noi dung file trong flash. */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

#define SAVE_REQ_MAX_LEN 512

/* ---------------------------------------------------------------------
 * GET /  ->  Tra ve trang HTML cau hinh WiFi (giao dien VNPT)
 * ------------------------------------------------------------------- */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    size_t html_len = index_html_end - index_html_start;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, (const char *)index_html_start, html_len);
}

/* ---------------------------------------------------------------------
 * GET /api/scan  ->  Quet cac mang WiFi xung quanh, tra ve JSON
 * Vi du response:
 *   [{"ssid":"VNPT_ABC","rssi":-55,"secure":true}, ...]
 * ------------------------------------------------------------------- */
static esp_err_t scan_get_handler(httpd_req_t *req)
{
    wifi_scan_config_t scan_config = {
        .show_hidden = false,
    };

    /* Quet dong bo (block=true): don gian, phu hop khi CPU chua lam gi
     * nang khac. Qua trinh nay co the lam STA tam gian doan ket noi vai
     * trăm ms neu dang connected - dieu nay binh thuong doi voi tinh nang
     * "quet lai WiFi" tren trang cau hinh. */
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_scan_start loi: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"scan_failed\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 20) {
        ap_count = 20; /* gioi han de khong chiem qua nhieu RAM tam thoi */
    }

    cJSON *root = cJSON_CreateArray();

    if (ap_count > 0) {
        wifi_ap_record_t *ap_records = calloc(ap_count, sizeof(wifi_ap_record_t));
        if (ap_records != NULL) {
            esp_wifi_scan_get_ap_records(&ap_count, ap_records);
            for (int i = 0; i < ap_count; i++) {
                if (strlen((const char *)ap_records[i].ssid) == 0) {
                    continue; /* bo qua mang an SSID */
                }
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "ssid", (const char *)ap_records[i].ssid);
                cJSON_AddNumberToObject(item, "rssi", ap_records[i].rssi);
                cJSON_AddBoolToObject(item, "secure", ap_records[i].authmode != WIFI_AUTH_OPEN);
                cJSON_AddItemToArray(root, item);
            }
            free(ap_records);
        }
    }

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ---------------------------------------------------------------------
 * GET /api/status  ->  Trang thai ket noi STA hien tai (de JS tren trang
 * web hien thi "Da ket noi" / "Dang cho..." / IP hien tai)
 * ------------------------------------------------------------------- */
static esp_err_t status_get_handler(httpd_req_t *req)
{
    char ip[16] = {0};
    char ssid[NVS_WIFI_SSID_MAX_LEN + 1] = {0};
    wifi_manager_get_sta_ip(ip, sizeof(ip));
    wifi_manager_get_connected_ssid(ssid, sizeof(ssid));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "connected", wifi_manager_is_connected());
    cJSON_AddStringToObject(root, "ssid", ssid);
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddStringToObject(root, "ap_ssid", CONFIG_PROV_AP_SSID);
    cJSON_AddStringToObject(root, "ap_password", CONFIG_PROV_AP_PASSWORD);

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ---------------------------------------------------------------------
 * GET /api/connect_status -> Tra ve tien trinh phien cau hinh (Provisioning)
 * Cho JS polling de hien thi popup theo tung buoc
 * ------------------------------------------------------------------- */
static esp_err_t connect_status_get_handler(httpd_req_t *req)
{
    prov_state_t st;
    wifi_manager_get_prov_state(&st);

    cJSON *root = cJSON_CreateObject();
    const char *status_str = "idle";
    int step = 0;

    switch (st.status) {
    case PROV_STATUS_CONNECTING:
        status_str = "connecting";
        step = 1;
        break;
    case PROV_STATUS_GOT_IP:
        status_str = "got_ip";
        step = 2;
        break;
    case PROV_STATUS_SUCCESS:
        status_str = "success";
        step = 3;
        break;
    case PROV_STATUS_NO_INTERNET:
        status_str = "no_internet";
        step = 3;
        break;
    case PROV_STATUS_FAIL_WRONG_PASS:
    case PROV_STATUS_FAIL_NO_AP:
    case PROV_STATUS_FAIL_ASSOC:
    case PROV_STATUS_FAIL_TIMEOUT:
    case PROV_STATUS_FAIL_GENERIC:
        status_str = "failed";
        step = 0;
        break;
    default:
        status_str = "idle";
        step = 0;
        break;
    }

    cJSON_AddStringToObject(root, "status", status_str);
    cJSON_AddNumberToObject(root, "step", step);
    cJSON_AddStringToObject(root, "error_msg", st.error_msg);
    cJSON_AddStringToObject(root, "ip", st.ip);

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ---------------------------------------------------------------------
 * Task chay ngam de goi wifi_manager_try_connect() SAU KHI HTTP response
 * da duoc gui xong cho trinh duyet.
 * ------------------------------------------------------------------- */
typedef struct {
    char ssid[NVS_WIFI_SSID_MAX_LEN + 1];
    char password[NVS_WIFI_PASS_MAX_LEN + 1];
} pending_connect_t;

static void delayed_connect_task(void *arg)
{
    pending_connect_t *info = (pending_connect_t *)arg;
    vTaskDelay(pdMS_TO_TICKS(150));
    wifi_manager_try_connect(info->ssid, info->password);
    free(info);
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------------
 * POST /api/save  ->  Body JSON: {"ssid":"...", "password":"..."}
 * Bat dau phien try_connect (NVS se tu luu SAU KHI co IP).
 * ------------------------------------------------------------------- */
static esp_err_t save_post_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len >= SAVE_REQ_MAX_LEN) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Du lieu khong hop le");
        return ESP_FAIL;
    }

    char buf[SAVE_REQ_MAX_LEN];
    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buf + received, req->content_len - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Loi doc du lieu");
            return ESP_FAIL;
        }
        received += ret;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON khong hop le");
        return ESP_FAIL;
    }

    cJSON *ssid_item = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass_item = cJSON_GetObjectItem(root, "password");

    if (!cJSON_IsString(ssid_item) || strlen(ssid_item->valuestring) == 0) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Thieu truong 'ssid'");
        return ESP_FAIL;
    }

    const char *ssid = ssid_item->valuestring;
    const char *password = cJSON_IsString(pass_item) ? pass_item->valuestring : "";

    /* Phan hoi cho trinh duyet ngay lap tuc */
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true,\"message\":\"Da nhan cau hinh. Dang bat dau thu ket noi...\"}",
                     HTTPD_RESP_USE_STRLEN);

    /* Lap lich goi wifi_manager_try_connect va khoi tao phien test */
    pending_connect_t *info = malloc(sizeof(pending_connect_t));
    if (info != NULL) {
        strncpy(info->ssid, ssid, sizeof(info->ssid) - 1);
        info->ssid[sizeof(info->ssid) - 1] = '\0';
        strncpy(info->password, password, sizeof(info->password) - 1);
        info->password[sizeof(info->password) - 1] = '\0';
        xTaskCreate(delayed_connect_task, "delayed_try_conn", 4096, info, 5, NULL);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

/* ---------------------------------------------------------------------
 * Handler cho Android: GET /generate_204
 * Android tra loi nay de xac nhan internet OK (khong popup)
 * ------------------------------------------------------------------- */
static esp_err_t android_generate_204_handler(httpd_req_t *req)
{
#ifdef CONFIG_ENABLE_CAPTIVE_DNS
    prov_state_t st;
    wifi_manager_get_prov_state(&st);
    if (st.status == PROV_STATUS_SUCCESS) {
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
#else
    /* Tra ve 204: Android nghi la co Internet -> KHONG popup */
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
#endif
    return ESP_OK;
}

/* ---------------------------------------------------------------------
 * Handler cho iOS: GET /hotspot-detect.html
 * iOS kiem tra noi dung nay de quyet dinh hien popup hay khong
 * ------------------------------------------------------------------- */
static esp_err_t ios_hotspot_detect_handler(httpd_req_t *req)
{
#ifdef CONFIG_ENABLE_CAPTIVE_DNS
    prov_state_t st;
    wifi_manager_get_prov_state(&st);
    if (st.status == PROV_STATUS_SUCCESS) {
        /* Tra ve trang Apple "Success" de iOS dong popup captive portal */
        const char *apple_success = "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>";
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, apple_success, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
#else
    /* Tra ve trang Apple "Success": iOS hieu la co Internet -> KHONG popup */
    const char *apple_success = "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, apple_success, HTTPD_RESP_USE_STRLEN);
#endif
    return ESP_OK;
}

/* ---------------------------------------------------------------------
 * Handler cho Windows NCSI: GET /ncsi.txt (Windows 7/8/10)
 *                           GET /connecttest.txt (Windows 10/11)
 * Windows kiem tra noi dung file nay de xac dinh co Internet hay khong.
 * Neu tra ve dung noi dung, Windows KHONG tu ngat WiFi va KHONG popup.
 * ------------------------------------------------------------------- */
static esp_err_t windows_ncsi_handler(httpd_req_t *req)
{
#ifdef CONFIG_ENABLE_CAPTIVE_DNS
    prov_state_t st;
    wifi_manager_get_prov_state(&st);
    if (st.status == PROV_STATUS_SUCCESS) {
        /* Khi provisioning xong, tra ve dung content de Windows dong popup */
        if (strstr(req->uri, "connecttest.txt")) {
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_send(req, "Microsoft Connect Test", HTTPD_RESP_USE_STRLEN);
        } else {
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_send(req, "Microsoft NCSI", HTTPD_RESP_USE_STRLEN);
        }
        return ESP_OK;
    }
    /* Chua xong -> redirect ve trang cau hinh */
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
#else
    /* Tra ve dung content: Windows nghi la co Internet -> on dinh, KHONG tu ngat WiFi */
    if (strstr(req->uri, "connecttest.txt")) {
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Microsoft Connect Test", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Microsoft NCSI", HTTPD_RESP_USE_STRLEN);
    }
#endif
    return ESP_OK;
}

/* ---------------------------------------------------------------------
 * Handler cho moi URL khong ton tai
 * ------------------------------------------------------------------- */
static esp_err_t http_404_redirect_handler(httpd_req_t *req, httpd_err_code_t err)
{
#ifdef CONFIG_ENABLE_CAPTIVE_DNS
    prov_state_t st;
    wifi_manager_get_prov_state(&st);
    if (st.status == PROV_STATUS_SUCCESS) {
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    /* Captive Portal BAT: redirect ve trang cau hinh -> dien thoai hien popup */
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
#else
    /* Captive Portal TAT: tra ve 204 cho tat ca URL con lai
     * de HDH khong phat hien ra captive portal va khong hien popup. */
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
#endif
    return ESP_OK;
}


/* ---------------------------------------------------------------------
 * Khoi dong HTTP server va dang ky toan bo route
 * ------------------------------------------------------------------- */
esp_err_t web_server_start(void)
{
    if (s_server != NULL) {
        ESP_LOGW(TAG, "HTTP server da dang chay");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 16;
    config.stack_size = 8192;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Khong the khoi dong HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t uri_root = {
        .uri = "/", .method = HTTP_GET, .handler = root_get_handler,
    };
    static const httpd_uri_t uri_scan = {
        .uri = "/api/scan", .method = HTTP_GET, .handler = scan_get_handler,
    };
    static const httpd_uri_t uri_status = {
        .uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler,
    };
    static const httpd_uri_t uri_conn_status = {
        .uri = "/api/connect_status", .method = HTTP_GET, .handler = connect_status_get_handler,
    };
    static const httpd_uri_t uri_save = {
        .uri = "/api/save", .method = HTTP_POST, .handler = save_post_handler,
    };
    /* Android probe */
    static const httpd_uri_t uri_gen204 = {
        .uri = "/generate_204", .method = HTTP_GET, .handler = android_generate_204_handler,
    };
    /* iOS probe */
    static const httpd_uri_t uri_hotspot = {
        .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = ios_hotspot_detect_handler,
    };
    /* Windows NCSI probes */
    static const httpd_uri_t uri_ncsi = {
        .uri = "/ncsi.txt", .method = HTTP_GET, .handler = windows_ncsi_handler,
    };
    static const httpd_uri_t uri_connecttest = {
        .uri = "/connecttest.txt", .method = HTTP_GET, .handler = windows_ncsi_handler,
    };

    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_scan);
    httpd_register_uri_handler(s_server, &uri_status);
    httpd_register_uri_handler(s_server, &uri_conn_status);
    httpd_register_uri_handler(s_server, &uri_save);
    httpd_register_uri_handler(s_server, &uri_gen204);
    httpd_register_uri_handler(s_server, &uri_hotspot);
    httpd_register_uri_handler(s_server, &uri_ncsi);
    httpd_register_uri_handler(s_server, &uri_connecttest);
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, http_404_redirect_handler);

    ESP_LOGI(TAG, "HTTP server da san sang tai http://%s/ (qua SoftAP)", CONFIG_PROV_AP_IP);
    return ESP_OK;
}

void web_server_stop(void)
{
    if (s_server == NULL) {
        return;
    }
    httpd_stop(s_server);
    s_server = NULL;
    ESP_LOGI(TAG, "HTTP server da dung");
}