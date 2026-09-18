#ifndef WEB_PORTAL_H
#define WEB_PORTAL_H

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t web_portal_start(void);
esp_err_t web_portal_stop(void);
bool web_portal_is_running(void);

#ifdef __cplusplus
}
#endif

#endif // WEB_PORTAL_H
