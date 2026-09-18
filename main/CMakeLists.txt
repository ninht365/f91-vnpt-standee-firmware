idf_component_register(SRCS "main.c" "qrcodegen.c" "wifi_manager.c" "web_portal.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver esp_lcd console vfs esp_wifi esp_netif esp_event nvs_flash esp_http_server lwip)
