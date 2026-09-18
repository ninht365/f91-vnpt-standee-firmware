#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_MGR_STATUS_IDLE = 0,
    WIFI_MGR_STATUS_CONNECTING,
    WIFI_MGR_STATUS_CONNECTED,
    WIFI_MGR_STATUS_FAILED,
    WIFI_MGR_STATUS_AP_ACTIVE
} wifi_mgr_status_t;

// Initialize NVS, Netif, Event Loop, and Wi-Fi stack
esp_err_t wifi_manager_init(void);

// NVS Operations
bool wifi_manager_load_credentials(char *ssid, size_t ssid_len, char *password, size_t pass_len);
esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password);
esp_err_t wifi_manager_erase_credentials(void);

// STA & SoftAP Control
esp_err_t wifi_manager_connect_sta(const char *ssid, const char *password);
esp_err_t wifi_manager_start_softap(char *out_ap_ssid, size_t max_len);
esp_err_t wifi_manager_stop_softap(void);
esp_err_t wifi_manager_scan_networks(wifi_ap_record_t *ap_records, uint16_t *ap_count);

// Status & Information
wifi_mgr_status_t wifi_manager_get_status(void);
void wifi_manager_get_ip(char *ip_str, size_t max_len);
void wifi_manager_get_current_ssid(char *ssid_str, size_t max_len);
int8_t wifi_manager_get_rssi(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H
