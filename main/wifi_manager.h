#pragma once

#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PRIMARY_PING_TARGET "1.1.1.1"
#define BACKUP_PING_TARGET  "8.8.8.8"

/** Main Wi-Fi State Machine states */
typedef enum {
    WIFI_APP_STATE_IDLE,            /**< No config or idle */
    WIFI_APP_STATE_STA_CONNECTING,  /**< Trying to connect STA */
    WIFI_APP_STATE_ONLINE,          /**< Connected and Internet verified OK */
    WIFI_APP_STATE_AP_MODE,         /**< SoftAP + DNS + WebServer active */
    WIFI_APP_STATE_STA_FAIL,        /**< Connect failed after retries */
} wifi_app_state_t;

/** Web Provisioning Session Status */
typedef enum {
    PROV_STATUS_IDLE = 0,           /**< Idle */
    PROV_STATUS_CONNECTING,         /**< Authenticating with Router */
    PROV_STATUS_GOT_IP,             /**< Obtained IP, verifying Internet */
    PROV_STATUS_SUCCESS,            /**< Connected & Internet verified OK */
    PROV_STATUS_FAIL_WRONG_PASS,    /**< Incorrect password */
    PROV_STATUS_FAIL_NO_AP,         /**< AP not found */
    PROV_STATUS_FAIL_ASSOC,         /**< Association failed */
    PROV_STATUS_FAIL_TIMEOUT,       /**< Timeout (15s) */
    PROV_STATUS_FAIL_GENERIC,       /**< Generic failure */
    PROV_STATUS_NO_INTERNET,        /**< Connected but no Internet */
} prov_status_t;

typedef struct {
    prov_status_t status;
    char error_msg[80];
    char ip[16];
} prov_state_t;

typedef enum {
    INTERNET_UNKNOWN = 0,
    INTERNET_CHECKING,
    INTERNET_ONLINE,
    INTERNET_OFFLINE
} internet_status_t;

typedef void (*wifi_ap_exit_callback_t)(void);

// Initialization & Core Lifecycle
esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_start_softap(char *out_ap_ssid, size_t max_len);
esp_err_t wifi_manager_stop_softap(void);
esp_err_t wifi_manager_connect(const char *ssid, const char *password);
esp_err_t wifi_manager_try_connect(const char *ssid, const char *password);

// Status Getters
bool wifi_manager_is_connected(void);
void wifi_manager_get_sta_ip(char *ip_buf, size_t ip_buf_len);
void wifi_manager_get_connected_ssid(char *ssid_buf, size_t ssid_buf_len);
void wifi_manager_get_prov_state(prov_state_t *out);
wifi_app_state_t wifi_manager_get_status(void);
int8_t wifi_manager_get_rssi(void);

// Internet Ping
esp_err_t wifi_manager_ping(const char *target_ip, uint32_t *latency_ms);
bool wifi_manager_check_internet(char *out_info, size_t max_len);
internet_status_t wifi_manager_get_internet_status(void);
void wifi_manager_get_internet_info(char *info_str, size_t max_len);

// Callbacks
void wifi_manager_set_ap_exit_callback(wifi_ap_exit_callback_t cb);

#ifdef __cplusplus
}
#endif
