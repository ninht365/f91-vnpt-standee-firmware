/**
 * @file wifi_manager.c
 * @brief Wi-Fi Manager for F91 Standee (ESP32-S2), based on wifi_demo architecture.
 */
#include "wifi_manager.h"
#include "nvs_storage.h"
#include "captive_dns.h"
#include "web_server.h"
#include "internet_check.h"

#include <string.h>
#include <stdio.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "lwip/ip4_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "ping/ping_sock.h"
#include "lwip/inet.h"

static const char *TAG = "wifi_manager";

#define AP_CHANNEL        1
#define AP_MAX_CONN       4
#define STA_MAXIMUM_RETRY 5
#define PROV_AP_IP        "192.168.4.1"
#define RETRY_SLOW_INTERVAL_MS 15000

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t s_wifi_event_group  = NULL;
static esp_timer_handle_t s_retry_timer       = NULL;
static esp_timer_handle_t s_ap_exit_timer     = NULL;

static wifi_app_state_t s_app_state    = WIFI_APP_STATE_IDLE;
static int              s_retry_count  = 0;
static bool             s_sta_connected = false;
static char             s_current_ssid[NVS_WIFI_SSID_MAX_LEN + 1] = {0};
static char             s_current_ip[16] = {0};
static char             s_current_ap_ssid[32] = "VNPT_F91_WIFI";
static char             s_current_internet_info[128] = "Chua kiem tra";
static internet_status_t s_internet_status = INTERNET_UNKNOWN;

static prov_state_t       s_prov_state       = { PROV_STATUS_IDLE, "", "" };
static bool s_in_provisioning = false;
static bool s_prov_aborting = false;
static char s_pending_ssid[NVS_WIFI_SSID_MAX_LEN + 1] = {0};
static char s_pending_pass[NVS_WIFI_PASS_MAX_LEN + 1] = {0};
static esp_timer_handle_t s_prov_timeout_timer = NULL;
static esp_timer_handle_t s_no_internet_exit_timer = NULL;

static wifi_ap_exit_callback_t s_ap_exit_cb = NULL;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif  = NULL;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);
static void retry_timer_callback(void *arg);
static void ap_exit_timer_callback(void *arg);
static void prov_timeout_callback(void *arg);
static void no_internet_exit_callback(void *arg);
static void on_internet_result(bool ok);
static void exit_ap_mode(void);

void wifi_manager_set_ap_exit_callback(wifi_ap_exit_callback_t cb)
{
    s_ap_exit_cb = cb;
}

wifi_app_state_t wifi_manager_get_state(void)
{
    return s_app_state;
}

wifi_app_state_t wifi_manager_get_status(void)
{
    return s_app_state;
}

internet_status_t wifi_manager_get_internet_status(void)
{
    return s_internet_status;
}

int8_t wifi_manager_get_rssi(void)
{
    if (!s_sta_connected) return 0;
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}

void wifi_manager_get_internet_info(char *buf, size_t len)
{
    if (!buf || len == 0) return;
    strncpy(buf, s_current_internet_info, len - 1);
    buf[len - 1] = '\0';
}

static void exit_ap_mode(void)
{
    ESP_LOGI(TAG, "Exiting AP Mode -> Pure STA mode...");

    if (s_ap_exit_timer && esp_timer_is_active(s_ap_exit_timer)) {
        esp_timer_stop(s_ap_exit_timer);
    }
    if (s_no_internet_exit_timer && esp_timer_is_active(s_no_internet_exit_timer)) {
        esp_timer_stop(s_no_internet_exit_timer);
    }

    web_server_stop();
    captive_dns_stop();

    esp_wifi_set_mode(WIFI_MODE_STA);

    if (s_sta_connected) {
        s_app_state = (s_internet_status == INTERNET_ONLINE) ? WIFI_APP_STATE_ONLINE : WIFI_APP_STATE_STA_CONNECTING;
    } else {
        s_app_state = WIFI_APP_STATE_IDLE;
    }

    if (s_ap_exit_cb) {
        s_ap_exit_cb();
    }
}

static void ap_exit_timer_callback(void *arg)
{
    ESP_LOGI(TAG, "5 seconds elapsed after Internet OK -> Stopping SoftAP and switching to STA");
    exit_ap_mode();
}

static void prov_timeout_callback(void *arg)
{
    if (!s_in_provisioning) return;
    ESP_LOGW(TAG, "[Prov] 15s timeout reached -> TIMEOUT. Keeping AP open.");

    s_in_provisioning  = false;
    s_prov_state.status = PROV_STATUS_FAIL_TIMEOUT;
    strncpy(s_prov_state.error_msg, "Het thoi gian cho (15 giay). Vui long thu lai.",
            sizeof(s_prov_state.error_msg) - 1);

    memset(s_pending_ssid, 0, sizeof(s_pending_ssid));
    memset(s_pending_pass, 0, sizeof(s_pending_pass));

    esp_wifi_disconnect();
}

static void no_internet_exit_callback(void *arg)
{
    ESP_LOGI(TAG, "[Prov] 30s elapsed after No-Internet -> Exiting AP.");
    exit_ap_mode();
}

static void on_internet_result(bool ok)
{
    if (ok) {
        s_internet_status = INTERNET_ONLINE;
        s_app_state = WIFI_APP_STATE_ONLINE;
        snprintf(s_current_internet_info, sizeof(s_current_internet_info), "Internet OK (1.1.1.1/8.8.8.8)");
        ESP_LOGI(TAG, "Internet confirmed OK!");

        if (s_in_provisioning) {
            s_in_provisioning = false;
            s_prov_state.status = PROV_STATUS_SUCCESS;
            ESP_LOGI(TAG, "[Prov] SUCCESS: Wi-Fi + Internet OK. Stopping SoftAP in 5s...");

            if (s_prov_timeout_timer && esp_timer_is_active(s_prov_timeout_timer)) {
                esp_timer_stop(s_prov_timeout_timer);
            }
        }

        wifi_mode_t mode;
        if (esp_wifi_get_mode(&mode) == ESP_OK && (mode & WIFI_MODE_AP)) {
            if (s_ap_exit_timer) {
                if (esp_timer_is_active(s_ap_exit_timer)) {
                    esp_timer_stop(s_ap_exit_timer);
                }
                esp_timer_start_once(s_ap_exit_timer, 5000000ULL); // 5 seconds
                ESP_LOGI(TAG, "SoftAP will automatically shutdown in 5 seconds...");
            }
        }
    } else {
        s_internet_status = INTERNET_OFFLINE;
        snprintf(s_current_internet_info, sizeof(s_current_internet_info), "No Internet (Ping Failed)");
        if (s_in_provisioning) {
            s_in_provisioning = false;
            s_prov_state.status = PROV_STATUS_NO_INTERNET;
            strncpy(s_prov_state.error_msg,
                    "Wi-Fi da ket noi nhung khong co Internet",
                    sizeof(s_prov_state.error_msg) - 1);
            ESP_LOGW(TAG, "[Prov] No Internet. AP will exit in 30s.");

            if (s_prov_timeout_timer && esp_timer_is_active(s_prov_timeout_timer)) {
                esp_timer_stop(s_prov_timeout_timer);
            }

            if (s_no_internet_exit_timer) {
                if (esp_timer_is_active(s_no_internet_exit_timer)) {
                    esp_timer_stop(s_no_internet_exit_timer);
                }
                esp_timer_start_once(s_no_internet_exit_timer, 30000000ULL); // 30 seconds
            }
        } else {
            s_app_state = WIFI_APP_STATE_STA_FAIL;
            ESP_LOGW(TAG, "Ping Internet failed. internet_check will retry in background.");
        }
    }
}

static void retry_timer_callback(void *arg)
{
    if (s_sta_connected || strlen(s_current_ssid) == 0) return;
    if (s_app_state == WIFI_APP_STATE_AP_MODE) return;

    ESP_LOGI(TAG, "[Auto-reconnect] Retrying connection to \"%s\" ...", s_current_ssid);
    esp_wifi_connect();
}

esp_err_t wifi_manager_init(void)
{
    if (s_wifi_event_group) return ESP_OK;

    s_wifi_event_group = xEventGroupCreate();

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    const esp_timer_create_args_t retry_args = {
        .callback = &retry_timer_callback,
        .name     = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&retry_args, &s_retry_timer));

    const esp_timer_create_args_t ap_exit_args = {
        .callback = &ap_exit_timer_callback,
        .name     = "ap_exit",
    };
    ESP_ERROR_CHECK(esp_timer_create(&ap_exit_args, &s_ap_exit_timer));

    const esp_timer_create_args_t prov_timeout_args = {
        .callback = &prov_timeout_callback,
        .name     = "prov_timeout",
    };
    ESP_ERROR_CHECK(esp_timer_create(&prov_timeout_args, &s_prov_timeout_timer));

    const esp_timer_create_args_t no_internet_exit_args = {
        .callback = &no_internet_exit_callback,
        .name     = "no_internet_exit",
    };
    ESP_ERROR_CHECK(esp_timer_create(&no_internet_exit_args, &s_no_internet_exit_timer));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_max_tx_power(78);

    wifi_credentials_t saved;
    if (nvs_storage_load_wifi_credentials(&saved) == ESP_OK && strlen(saved.ssid) > 0) {
        ESP_LOGI(TAG, "Found stored Wi-Fi (SSID: %s) -> Connecting...", saved.ssid);
        s_app_state = WIFI_APP_STATE_STA_CONNECTING;
        wifi_manager_connect(saved.ssid, saved.password);
    } else {
        ESP_LOGI(TAG, "No stored Wi-Fi. Waiting for AT+WIFICFG or SoftAP start.");
        s_app_state = WIFI_APP_STATE_IDLE;
    }

    return ESP_OK;
}

esp_err_t wifi_manager_start_softap(char *out_ap_ssid, size_t max_len)
{
    ESP_LOGI(TAG, "Entering SoftAP Mode...");
    s_app_state   = WIFI_APP_STATE_AP_MODE;
    s_retry_count = 0;

    if (s_retry_timer && esp_timer_is_active(s_retry_timer)) {
        esp_timer_stop(s_retry_timer);
    }

    esp_wifi_disconnect();

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_current_ap_ssid, sizeof(s_current_ap_ssid), "VNPT_F91_%02X%02X", mac[4], mac[5]);
    if (out_ap_ssid && max_len > 0) {
        strncpy(out_ap_ssid, s_current_ap_ssid, max_len - 1);
        out_ap_ssid[max_len - 1] = '\0';
    }

    wifi_config_t ap_config = {
        .ap = {
            .channel        = AP_CHANNEL,
            .max_connection = AP_MAX_CONN,
            .authmode       = WIFI_AUTH_OPEN,
            .pmf_cfg = {
                .capable  = true,
                .required = false,
            },
        },
    };
    strncpy((char *)ap_config.ap.ssid, s_current_ap_ssid, sizeof(ap_config.ap.ssid) - 1);
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);

    if (s_ap_netif) {
        esp_netif_ip_info_t ip_info;
        IP4_ADDR(&ip_info.ip,      192, 168, 4, 1);
        IP4_ADDR(&ip_info.gw,      192, 168, 4, 1);
        IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
        esp_netif_dhcps_stop(s_ap_netif);
        esp_netif_set_ip_info(s_ap_netif, &ip_info);
        esp_netif_dhcps_start(s_ap_netif);
    }

    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_max_tx_power(78);

    captive_dns_start();
    web_server_start();

    ESP_LOGI(TAG, "SoftAP Active: SSID=\"%s\", IP=%s", s_current_ap_ssid, PROV_AP_IP);
    return ESP_OK;
}

esp_err_t wifi_manager_stop_softap(void)
{
    exit_ap_mode();
    return ESP_OK;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    if (ssid == NULL || strlen(ssid) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_ap_exit_timer && esp_timer_is_active(s_ap_exit_timer)) {
        esp_timer_stop(s_ap_exit_timer);
    }

    internet_check_stop();

    wifi_config_t sta_config = { 0 };
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    if (password != NULL && strlen(password) > 0) {
        strncpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password) - 1);
        sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    sta_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    strncpy(s_current_ssid, ssid, sizeof(s_current_ssid) - 1);
    s_sta_connected = false;
    s_retry_count   = 0;
    s_app_state     = WIFI_APP_STATE_STA_CONNECTING;

    if (esp_timer_is_active(s_retry_timer)) {
        esp_timer_stop(s_retry_timer);
    }
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Connecting to SSID: \"%s\" ...", ssid);
    err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "esp_wifi_connect() returned: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

esp_err_t wifi_manager_try_connect(const char *ssid, const char *password)
{
    if (ssid == NULL || strlen(ssid) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_in_provisioning) {
        ESP_LOGI(TAG, "[Prov] Received new credentials while connecting -> aborting previous session.");
        s_prov_aborting = true;

        if (s_prov_timeout_timer && esp_timer_is_active(s_prov_timeout_timer)) {
            esp_timer_stop(s_prov_timeout_timer);
        }
        if (s_no_internet_exit_timer && esp_timer_is_active(s_no_internet_exit_timer)) {
            esp_timer_stop(s_no_internet_exit_timer);
        }

        internet_check_stop();
        esp_wifi_disconnect();
    }

    strncpy(s_pending_ssid, ssid, sizeof(s_pending_ssid) - 1);
    strncpy(s_pending_pass, password ? password : "", sizeof(s_pending_pass) - 1);
    s_pending_ssid[sizeof(s_pending_ssid) - 1] = '\0';
    s_pending_pass[sizeof(s_pending_pass) - 1] = '\0';

    s_prov_state.status = PROV_STATUS_CONNECTING;
    memset(s_prov_state.error_msg, 0, sizeof(s_prov_state.error_msg));
    memset(s_prov_state.ip, 0, sizeof(s_prov_state.ip));
    s_in_provisioning = true;

    if (s_prov_timeout_timer) {
        if (esp_timer_is_active(s_prov_timeout_timer)) {
            esp_timer_stop(s_prov_timeout_timer);
        }
        esp_timer_start_once(s_prov_timeout_timer, 15000000ULL); // 15s
    }

    wifi_config_t sta_config = { 0 };
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    if (password && strlen(password) > 0) {
        strncpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password) - 1);
        sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    sta_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    strncpy(s_current_ssid, ssid, sizeof(s_current_ssid) - 1);
    s_sta_connected = false;
    s_retry_count   = 0;
    s_app_state     = WIFI_APP_STATE_STA_CONNECTING;

    if (s_retry_timer && esp_timer_is_active(s_retry_timer)) {
        esp_timer_stop(s_retry_timer);
    }
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[Prov] esp_wifi_set_config error: %s", esp_err_to_name(err));
        s_in_provisioning = false;
        s_prov_state.status = PROV_STATUS_FAIL_GENERIC;
        return err;
    }

    ESP_LOGI(TAG, "[Prov] Connecting to SSID: \"%s\" (timeout=15s)...", ssid);
    err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "[Prov] esp_wifi_connect() returned: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {

        case WIFI_EVENT_STA_DISCONNECTED: {
            s_sta_connected = false;
            memset(s_current_ip, 0, sizeof(s_current_ip));
            internet_check_stop();

            wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
            uint8_t reason = disc ? disc->reason : 0;

            if (s_in_provisioning) {
                if (s_prov_aborting) {
                    s_prov_aborting = false;
                    ESP_LOGI(TAG, "[Prov] STA disconnect due to new session, ignoring.");
                    break;
                }

                if (s_prov_timeout_timer && esp_timer_is_active(s_prov_timeout_timer)) {
                    esp_timer_stop(s_prov_timeout_timer);
                }

                s_in_provisioning = false;
                memset(s_pending_ssid, 0, sizeof(s_pending_ssid));
                memset(s_pending_pass, 0, sizeof(s_pending_pass));

                switch (reason) {
                case WIFI_REASON_AUTH_FAIL:
                case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
                    s_prov_state.status = PROV_STATUS_FAIL_WRONG_PASS;
                    strncpy(s_prov_state.error_msg, "Sai mat khau Wi-Fi",
                            sizeof(s_prov_state.error_msg) - 1);
                    break;
                case WIFI_REASON_NO_AP_FOUND:
                    s_prov_state.status = PROV_STATUS_FAIL_NO_AP;
                    strncpy(s_prov_state.error_msg, "Khong tim thay mang Wi-Fi",
                            sizeof(s_prov_state.error_msg) - 1);
                    break;
                case WIFI_REASON_ASSOC_FAIL:
                case WIFI_REASON_ASSOC_LEAVE:
                    s_prov_state.status = PROV_STATUS_FAIL_ASSOC;
                    strncpy(s_prov_state.error_msg, "Khong the lien ket voi AP",
                            sizeof(s_prov_state.error_msg) - 1);
                    break;
                default:
                    s_prov_state.status = PROV_STATUS_FAIL_GENERIC;
                    snprintf(s_prov_state.error_msg, sizeof(s_prov_state.error_msg),
                             "Ket noi that bai (ma loi: %d)", reason);
                    break;
                }

                ESP_LOGW(TAG, "[Prov] FAILED: %s (reason=%d). SoftAP remains open for retry.",
                         s_prov_state.error_msg, reason);
                break;
            }

            if (s_app_state == WIFI_APP_STATE_AP_MODE) break;

            ESP_LOGW(TAG, "STA Disconnected (reason=%d)", reason);

            if (strlen(s_current_ssid) == 0) break;

            if (s_retry_count < STA_MAXIMUM_RETRY) {
                s_retry_count++;
                s_app_state = WIFI_APP_STATE_STA_CONNECTING;
                ESP_LOGI(TAG, "Retrying STA connection (%d/%d)...", s_retry_count, STA_MAXIMUM_RETRY);
                esp_wifi_connect();
            } else {
                ESP_LOGW(TAG, "Retry limit (%d) reached. Periodic retry every %dms.",
                         STA_MAXIMUM_RETRY, RETRY_SLOW_INTERVAL_MS);
                s_app_state = WIFI_APP_STATE_STA_FAIL;
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                if (!esp_timer_is_active(s_retry_timer)) {
                    esp_timer_start_periodic(s_retry_timer,
                                             (uint64_t)RETRY_SLOW_INTERVAL_MS * 1000ULL);
                }
            }
            break;
        }

        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *evt = (wifi_event_ap_staconnected_t *)event_data;
            ESP_LOGI(TAG, "[SoftAP] Client connected, MAC: " MACSTR, MAC2STR(evt->mac));
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *evt = (wifi_event_ap_stadisconnected_t *)event_data;
            ESP_LOGI(TAG, "[SoftAP] Client disconnected, MAC: " MACSTR, MAC2STR(evt->mac));
            break;
        }

        default:
            break;
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

        s_sta_connected = true;
        s_retry_count   = 0;
        if (s_retry_timer && esp_timer_is_active(s_retry_timer)) {
            esp_timer_stop(s_retry_timer);
        }

        snprintf(s_current_ip, sizeof(s_current_ip), IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        ESP_LOGI(TAG, "==============================================");
        ESP_LOGI(TAG, " Wi-Fi Connected Successfully!");
        ESP_LOGI(TAG, " SSID : %s", s_current_ssid);
        ESP_LOGI(TAG, " IP   : %s", s_current_ip);
        ESP_LOGI(TAG, "==============================================");

        if (s_in_provisioning) {
            esp_err_t nvs_err = nvs_storage_save_wifi_credentials(s_pending_ssid, s_pending_pass);
            if (nvs_err == ESP_OK) {
                ESP_LOGI(TAG, "[Prov] Saved Wi-Fi credentials to NVS (SSID: %s)", s_pending_ssid);
            } else {
                ESP_LOGW(TAG, "[Prov] Could not save to NVS: %s", esp_err_to_name(nvs_err));
            }

            s_prov_state.status = PROV_STATUS_GOT_IP;
            strncpy(s_prov_state.ip, s_current_ip, sizeof(s_prov_state.ip) - 1);
            ESP_LOGI(TAG, "[Prov] Got IP: %s -> Verifying Internet...", s_current_ip);
        }

        internet_check_start(on_internet_result);
    }
}

bool wifi_manager_is_connected(void) { return s_sta_connected; }

void wifi_manager_get_sta_ip(char *ip_buf, size_t ip_buf_len)
{
    if (!ip_buf || ip_buf_len == 0) return;
    strncpy(ip_buf, s_current_ip, ip_buf_len - 1);
    ip_buf[ip_buf_len - 1] = '\0';
}

void wifi_manager_get_connected_ssid(char *ssid_buf, size_t ssid_buf_len)
{
    if (!ssid_buf || ssid_buf_len == 0) return;
    strncpy(ssid_buf, s_current_ssid, ssid_buf_len - 1);
    ssid_buf[ssid_buf_len - 1] = '\0';
}

void wifi_manager_get_prov_state(prov_state_t *out)
{
    if (!out) return;
    *out = s_prov_state;
}

esp_err_t wifi_manager_scan(wifi_ap_record_t *ap_records, uint16_t *ap_count)
{
    if (!ap_records || !ap_count || *ap_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Starting Wi-Fi Scan...");
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 120,
        .scan_time.active.max = 300,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_scan_get_ap_records(ap_count, ap_records);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_get_ap_records failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Scan completed. Found %u APs.", *ap_count);
    return ESP_OK;
}

/* Standalone ping function for AT+PING */
static SemaphoreHandle_t s_cmd_ping_sem = NULL;
static uint32_t s_cmd_ping_sent = 0;
static uint32_t s_cmd_ping_received = 0;
static uint32_t s_cmd_ping_timegap_sum = 0;

static void on_cmd_ping_success(esp_ping_handle_t hdl, void *args)
{
    uint32_t elapsed_ms;
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_ms, sizeof(elapsed_ms));
    s_cmd_ping_timegap_sum += elapsed_ms;
    printf("Ping reply received, RTT=%lums\n", (unsigned long)elapsed_ms);
}

static void on_cmd_ping_timeout(esp_ping_handle_t hdl, void *args)
{
    printf("Ping timeout\n");
}

static void on_cmd_ping_end(esp_ping_handle_t hdl, void *args)
{
    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &s_cmd_ping_sent, sizeof(s_cmd_ping_sent));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY,   &s_cmd_ping_received, sizeof(s_cmd_ping_received));
    esp_ping_delete_session(hdl);
    if (s_cmd_ping_sem) {
        xSemaphoreGive(s_cmd_ping_sem);
    }
}

esp_err_t wifi_manager_ping(const char *target_ip, uint32_t *latency_ms)
{
    if (!target_ip || strlen(target_ip) == 0) return ESP_ERR_INVALID_ARG;

    ip_addr_t target;
    if (!ipaddr_aton(target_ip, &target)) {
        ESP_LOGE(TAG, "Invalid Ping IP: %s", target_ip);
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_cmd_ping_sem) {
        s_cmd_ping_sem = xSemaphoreCreateBinary();
    }

    s_cmd_ping_sent = 0;
    s_cmd_ping_received = 0;
    s_cmd_ping_timegap_sum = 0;

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count = 3;
    cfg.interval_ms = 1000;
    cfg.timeout_ms = 2000;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = on_cmd_ping_success,
        .on_ping_timeout = on_cmd_ping_timeout,
        .on_ping_end     = on_cmd_ping_end,
        .cb_args         = NULL,
    };

    esp_ping_handle_t ping_hdl = NULL;
    esp_err_t err = esp_ping_new_session(&cfg, &cbs, &ping_hdl);
    if (err != ESP_OK) return err;

    esp_ping_start(ping_hdl);
    xSemaphoreTake(s_cmd_ping_sem, pdMS_TO_TICKS(15000));

    printf("--- %s ping statistics ---\n", target_ip);
    printf("%lu packets transmitted, %lu received\n",
           (unsigned long)s_cmd_ping_sent, (unsigned long)s_cmd_ping_received);

    if (s_cmd_ping_received > 0) {
        if (latency_ms) {
            *latency_ms = s_cmd_ping_timegap_sum / s_cmd_ping_received;
        }
        return ESP_OK;
    }

    return ESP_FAIL;
}

bool wifi_manager_check_internet(char *out_info, size_t max_len)
{
    uint32_t lat = 0;
    esp_err_t err = wifi_manager_ping(PRIMARY_PING_TARGET, &lat);
    if (err == ESP_OK) {
        s_internet_status = INTERNET_ONLINE;
        if (out_info && max_len > 0) {
            snprintf(out_info, max_len, "SUCCESS (Primary %s OK, RTT: %ums)", PRIMARY_PING_TARGET, (unsigned int)lat);
        }
        return true;
    }

    err = wifi_manager_ping(BACKUP_PING_TARGET, &lat);
    if (err == ESP_OK) {
        s_internet_status = INTERNET_ONLINE;
        if (out_info && max_len > 0) {
            snprintf(out_info, max_len, "SUCCESS (Backup %s OK, RTT: %ums)", BACKUP_PING_TARGET, (unsigned int)lat);
        }
        return true;
    }

    s_internet_status = INTERNET_OFFLINE;
    if (out_info && max_len > 0) {
        snprintf(out_info, max_len, "FAILED (Primary %s & Backup %s Unreachable)", PRIMARY_PING_TARGET, BACKUP_PING_TARGET);
    }
    return false;
}
