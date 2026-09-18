#include "wifi_manager.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "ping/ping_sock.h"
#include "lwip/inet.h"

static const char *TAG = "WIFI_MGR";

#define NVS_NAMESPACE "wifi_store"
#define NVS_KEY_SSID  "ssid"
#define NVS_KEY_PASS  "pass"

#define MAX_RETRY_COUNT 5

static esp_netif_t *sta_netif = NULL;
static esp_netif_t *ap_netif = NULL;

static volatile wifi_mgr_status_t current_wifi_status = WIFI_MGR_STATUS_IDLE;
static volatile internet_status_t current_internet_status = INTERNET_UNKNOWN;
static char current_internet_info[128] = "Chưa kiểm tra";
static char current_sta_ip[32] = "0.0.0.0";
static char current_sta_ssid[64] = {0};
static int retry_num = 0;
static bool wifi_initialized = false;

// Task to check internet connectivity asynchronously after getting IP
static void internet_check_bg_task(void *pvParameters) {
    vTaskDelay(pdMS_TO_TICKS(1000)); // Wait 1 second for network routes to settle
    wifi_manager_check_internet(NULL, 0);
    vTaskDelete(NULL);
}

// Event Handler for Wi-Fi and IP events
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi STA started, connecting...");
        esp_wifi_connect();
        current_wifi_status = WIFI_MGR_STATUS_CONNECTING;
        current_internet_status = INTERNET_UNKNOWN;
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t*) event_data;
        ESP_LOGW(TAG, "Wi-Fi STA disconnected, reason: %d", disc->reason);
        current_internet_status = INTERNET_OFFLINE;
        if (retry_num < MAX_RETRY_COUNT) {
            esp_wifi_connect();
            retry_num++;
            current_wifi_status = WIFI_MGR_STATUS_CONNECTING;
            ESP_LOGI(TAG, "Retrying connection (%d/%d)...", retry_num, MAX_RETRY_COUNT);
        } else {
            current_wifi_status = WIFI_MGR_STATUS_FAILED;
            ESP_LOGE(TAG, "Failed to connect to AP after %d attempts", MAX_RETRY_COUNT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        esp_ip4addr_ntoa(&event->ip_info.ip, current_sta_ip, sizeof(current_sta_ip));
        ESP_LOGI(TAG, "Wi-Fi Connected! Obtained IP: %s", current_sta_ip);
        retry_num = 0;
        current_wifi_status = WIFI_MGR_STATUS_CONNECTED;

        // Trigger background Internet Ping test (Primary: 1.1.1.1, Backup: 8.8.8.8)
        xTaskCreate(internet_check_bg_task, "net_ping_task", 4096, NULL, 5, NULL);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "Station "MACSTR" joined SoftAP, AID=%d", MAC2STR(event->mac), event->aid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "Station "MACSTR" left SoftAP, AID=%d", MAC2STR(event->mac), event->aid);
    }
}

esp_err_t wifi_manager_init(void) {
    if (wifi_initialized) return ESP_OK;

    // 1. Initialize NVS Flash
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Initialize TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    sta_netif = esp_netif_create_default_wifi_sta();
    ap_netif = esp_netif_create_default_wifi_ap();

    // 3. Initialize Wi-Fi driver
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 4. Register Event Handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_initialized = true;
    ESP_LOGI(TAG, "Wi-Fi Manager Initialized successfully!");
    return ESP_OK;
}

bool wifi_manager_load_credentials(char *ssid, size_t ssid_len, char *password, size_t pass_len) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) return false;

    size_t required_ssid_len = ssid_len;
    err = nvs_get_str(my_handle, NVS_KEY_SSID, ssid, &required_ssid_len);
    if (err != ESP_OK || strlen(ssid) == 0) {
        nvs_close(my_handle);
        return false;
    }

    size_t required_pass_len = pass_len;
    err = nvs_get_str(my_handle, NVS_KEY_PASS, password, &required_pass_len);
    if (err != ESP_OK) {
        password[0] = '\0';
    }

    nvs_close(my_handle);
    ESP_LOGI(TAG, "Loaded saved Wi-Fi from NVS: \"%s\"", ssid);
    return true;
}

esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password) {
    if (!ssid || strlen(ssid) == 0) return ESP_ERR_INVALID_ARG;

    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(my_handle, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(my_handle, NVS_KEY_PASS, password ? password : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(my_handle);
    }
    nvs_close(my_handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Successfully saved Wi-Fi credentials for \"%s\" to NVS", ssid);
    }
    return err;
}

esp_err_t wifi_manager_erase_credentials(void) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return err;

    nvs_erase_all(my_handle);
    nvs_commit(my_handle);
    nvs_close(my_handle);

    ESP_LOGI(TAG, "Erased all Wi-Fi credentials from NVS");
    return ESP_OK;
}

esp_err_t wifi_manager_connect_sta(const char *ssid, const char *password) {
    if (!ssid || strlen(ssid) == 0) return ESP_ERR_INVALID_ARG;

    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password) {
        strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    }
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    strncpy(current_sta_ssid, ssid, sizeof(current_sta_ssid) - 1);
    retry_num = 0;
    current_wifi_status = WIFI_MGR_STATUS_CONNECTING;
    current_internet_status = INTERNET_UNKNOWN;

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    } else if (mode != WIFI_MODE_APSTA) {
        esp_wifi_set_mode(WIFI_MODE_STA);
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_LOGI(TAG, "Connecting to AP SSID: \"%s\"...", ssid);
    return esp_wifi_connect();
}

esp_err_t wifi_manager_start_softap(char *out_ap_ssid, size_t max_len) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "VNPT_F91_%02X%02X", mac[4], mac[5]);

    if (out_ap_ssid && max_len > 0) {
        strncpy(out_ap_ssid, ap_ssid, max_len - 1);
        out_ap_ssid[max_len - 1] = '\0';
    }

    wifi_config_t ap_config = {
        .ap = {
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
            .pmf_cfg = {
                .required = false,
            },
        },
    };
    strncpy((char*)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen(ap_ssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    current_wifi_status = WIFI_MGR_STATUS_AP_ACTIVE;
    ESP_LOGI(TAG, "SoftAP started! SSID: \"%s\", Channel: 1 (Open)", ap_ssid);
    return ESP_OK;
}

esp_err_t wifi_manager_stop_softap(void) {
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_APSTA || mode == WIFI_MODE_AP) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_LOGI(TAG, "SoftAP stopped. Switched back to pure STA mode.");
    }
    return ESP_OK;
}

esp_err_t wifi_manager_scan_networks(wifi_ap_record_t *ap_records, uint16_t *ap_count) {
    if (!ap_records || !ap_count || *ap_count == 0) return ESP_ERR_INVALID_ARG;

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    ESP_LOGI(TAG, "Starting Wi-Fi Scan...");
    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Scan failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(ap_count, ap_records));
    ESP_LOGI(TAG, "Scan completed. Found %d Access Points.", *ap_count);
    return ESP_OK;
}

// Ping Result Context
typedef struct {
    SemaphoreHandle_t sem;
    uint32_t resp_time_ms;
    uint32_t success_count;
} ping_context_t;

static void ping_on_success(esp_ping_handle_t hdl, void *args) {
    ping_context_t *ctx = (ping_context_t *)args;
    uint32_t elapsed_time;
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_time, sizeof(elapsed_time));
    ctx->resp_time_ms = elapsed_time;
    ctx->success_count++;
}

static void ping_on_timeout(esp_ping_handle_t hdl, void *args) {
    // Ping timeout callback
}

static void ping_on_end(esp_ping_handle_t hdl, void *args) {
    ping_context_t *ctx = (ping_context_t *)args;
    if (ctx && ctx->sem) {
        xSemaphoreGive(ctx->sem);
    }
}

esp_err_t wifi_manager_ping(const char *target_ip, uint32_t *latency_ms) {
    if (!target_ip) return ESP_ERR_INVALID_ARG;

    ip_addr_t target_addr;
    memset(&target_addr, 0, sizeof(target_addr));
    if (ipaddr_aton(target_ip, &target_addr) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ping_context_t ctx = {
        .sem = xSemaphoreCreateBinary(),
        .resp_time_ms = 0,
        .success_count = 0
    };

    if (!ctx.sem) return ESP_ERR_NO_MEM;

    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ping_config.target_addr = target_addr;
    ping_config.count = 2;
    ping_config.interval_ms = 400;
    ping_config.timeout_ms = 1000;

    esp_ping_callbacks_t cbs = {
        .cb_args = &ctx,
        .on_ping_success = ping_on_success,
        .on_ping_timeout = ping_on_timeout,
        .on_ping_end = ping_on_end
    };

    esp_ping_handle_t ping_handle;
    esp_err_t err = esp_ping_new_session(&ping_config, &cbs, &ping_handle);
    if (err != ESP_OK) {
        vSemaphoreDelete(ctx.sem);
        return err;
    }

    esp_ping_start(ping_handle);
    xSemaphoreTake(ctx.sem, pdMS_TO_TICKS(2500));
    esp_ping_stop(ping_handle);
    esp_ping_delete_session(ping_handle);
    vSemaphoreDelete(ctx.sem);

    if (ctx.success_count > 0) {
        if (latency_ms) *latency_ms = ctx.resp_time_ms;
        return ESP_OK;
    }
    return ESP_FAIL;
}

bool wifi_manager_check_internet(char *out_info, size_t max_len) {
    current_internet_status = INTERNET_CHECKING;
    uint32_t latency = 0;

    ESP_LOGI(TAG, "Testing Internet via Primary: %s...", PRIMARY_PING_TARGET);
    esp_err_t err = wifi_manager_ping(PRIMARY_PING_TARGET, &latency);
    if (err == ESP_OK) {
        current_internet_status = INTERNET_ONLINE;
        snprintf(current_internet_info, sizeof(current_internet_info),
                 "ONLINE (Primary 1.1.1.1: %u ms)", (unsigned int)latency);
        ESP_LOGI(TAG, "Internet OK! %s", current_internet_info);
        if (out_info && max_len > 0) {
            strncpy(out_info, current_internet_info, max_len - 1);
            out_info[max_len - 1] = '\0';
        }
        return true;
    }

    ESP_LOGW(TAG, "Primary 1.1.1.1 failed. Testing Backup: %s...", BACKUP_PING_TARGET);
    err = wifi_manager_ping(BACKUP_PING_TARGET, &latency);
    if (err == ESP_OK) {
        current_internet_status = INTERNET_ONLINE;
        snprintf(current_internet_info, sizeof(current_internet_info),
                 "ONLINE (Backup 8.8.8.8: %u ms)", (unsigned int)latency);
        ESP_LOGI(TAG, "Internet OK! %s", current_internet_info);
        if (out_info && max_len > 0) {
            strncpy(out_info, current_internet_info, max_len - 1);
            out_info[max_len - 1] = '\0';
        }
        return true;
    }

    current_internet_status = INTERNET_OFFLINE;
    snprintf(current_internet_info, sizeof(current_internet_info), "OFFLINE (No Internet)");
    ESP_LOGE(TAG, "Internet Check FAILED: Both 1.1.1.1 and 8.8.8.8 unreachable!");
    if (out_info && max_len > 0) {
        strncpy(out_info, current_internet_info, max_len - 1);
        out_info[max_len - 1] = '\0';
    }
    return false;
}

internet_status_t wifi_manager_get_internet_status(void) {
    return current_internet_status;
}

void wifi_manager_get_internet_info(char *info_str, size_t max_len) {
    if (info_str && max_len > 0) {
        strncpy(info_str, current_internet_info, max_len - 1);
        info_str[max_len - 1] = '\0';
    }
}

wifi_mgr_status_t wifi_manager_get_status(void) {
    return current_wifi_status;
}

void wifi_manager_get_ip(char *ip_str, size_t max_len) {
    if (ip_str && max_len > 0) {
        strncpy(ip_str, current_sta_ip, max_len - 1);
        ip_str[max_len - 1] = '\0';
    }
}

void wifi_manager_get_current_ssid(char *ssid_str, size_t max_len) {
    if (ssid_str && max_len > 0) {
        strncpy(ssid_str, current_sta_ssid, max_len - 1);
        ssid_str[max_len - 1] = '\0';
    }
}

int8_t wifi_manager_get_rssi(void) {
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return -127;
}
