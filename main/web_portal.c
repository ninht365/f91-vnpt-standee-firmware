#include "web_portal.h"
#include "wifi_manager.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"

static const char *TAG = "WEB_PORTAL";
static httpd_handle_t server = NULL;

// Modern Mobile-Friendly VNPT Responsive Web Portal HTML
static const char html_index[] = 
"<!DOCTYPE html>"
"<html lang='vi'>"
"<head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no'>"
"<title>VNPT F91 - Cấu Hình Wi-Fi</title>"
"<style>"
"* { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; }"
"body { background: #f0f4f8; color: #333; display: flex; justify-content: center; align-items: center; min-height: 100vh; padding: 15px; }"
".card { background: #fff; width: 100%; max-width: 380px; border-radius: 16px; box-shadow: 0 8px 30px rgba(0,91,170,0.12); overflow: hidden; }"
".header { background: linear-gradient(135deg, #005BAA 0%, #0088FF 100%); color: white; padding: 24px 20px; text-align: center; }"
".header h1 { font-size: 20px; font-weight: 700; letter-spacing: 0.5px; }"
".header p { font-size: 13px; opacity: 0.9; margin-top: 4px; }"
".content { padding: 24px 20px; }"
".form-group { margin-bottom: 18px; }"
"label { display: block; font-size: 13px; font-weight: 600; color: #4a5568; margin-bottom: 6px; }"
"input, select { width: 100%; padding: 12px 14px; border: 1.5px solid #cbd5e0; border-radius: 10px; font-size: 15px; outline: none; transition: 0.2s; background: #fff; }"
"input:focus, select:focus { border-color: #005BAA; box-shadow: 0 0 0 3px rgba(0,91,170,0.15); }"
".btn { width: 100%; padding: 13px; border: none; border-radius: 10px; font-size: 15px; font-weight: 600; cursor: pointer; transition: 0.2s; display: flex; justify-content: center; align-items: center; gap: 8px; }"
".btn-primary { background: #005BAA; color: white; margin-top: 10px; }"
".btn-primary:active { background: #004580; transform: scale(0.98); }"
".btn-secondary { background: #e2e8f0; color: #2d3748; margin-bottom: 14px; font-size: 13px; padding: 10px; }"
".status-box { margin-top: 18px; padding: 12px; border-radius: 10px; font-size: 13px; text-align: center; display: none; }"
".status-info { background: #ebf8ff; color: #2b6cb0; border: 1px solid #bee3f8; }"
".status-success { background: #f0fff4; color: #276749; border: 1px solid #c6f6d5; }"
".status-error { background: #fff5f5; color: #9b2c2c; border: 1px solid #fed7d7; }"
".footer { text-align: center; padding: 14px; font-size: 12px; color: #a0aec0; background: #fafafa; border-top: 1px solid #edf2f7; }"
"</style>"
"</head>"
"<body>"
"<div class='card'>"
"<div class='header'>"
"<h1>VNPT F91 STANDEE</h1>"
"<p>Cấu Hình Mạng Wi-Fi Cho Thiết Bị</p>"
"</div>"
"<div class='content'>"
"<button type='button' class='btn btn-secondary' onclick='scanWifi()' id='btnScan'>🔍 Quét Mạng Wi-Fi Xung Quanh</button>"
"<div class='form-group'>"
"<label for='ssid'>Tên Mạng Wi-Fi (SSID):</label>"
"<select id='ssidSelect' onchange='selectSsid()' style='display:none; margin-bottom:8px;'><option value=''>-- Chọn mạng đã quét --</option></select>"
"<input type='text' id='ssid' placeholder='Nhập hoặc chọn tên Wi-Fi...'>"
"</div>"
"<div class='form-group'>"
"<label for='password'>Mật Khẩu Wi-Fi:</label>"
"<input type='password' id='password' placeholder='Nhập mật khẩu Wi-Fi...'>"
"</div>"
"<button type='button' class='btn btn-primary' onclick='saveWifi()' id='btnSave'>💾 Lưu & Kết Nối Ngay</button>"
"<div id='statusBox' class='status-box'></div>"
"</div>"
"<div class='footer'>Thiết bị Loa Thanh Toán VNPT Standee v2.3</div>"
"</div>"
"<script>"
"function showStatus(msg, type) {"
"  var b = document.getElementById('statusBox');"
"  b.className = 'status-box status-' + type;"
"  b.innerHTML = msg;"
"  b.style.display = 'block';"
"}"
"function scanWifi() {"
"  var btn = document.getElementById('btnScan');"
"  btn.innerText = '⏳ Đang quét sóng...';"
"  btn.disabled = true;"
"  showStatus('Đang quét danh sách mạng Wi-Fi 2.4GHz...', 'info');"
"  fetch('/api/scan')"
"    .then(function(r){ return r.json(); })"
"    .then(function(list){"
"      btn.innerText = '🔍 Quét Lại';"
"      btn.disabled = false;"
"      var sel = document.getElementById('ssidSelect');"
"      sel.innerHTML = '<option value=\"\">-- Chọn mạng đã quét (' + list.length + ' mạng) --</option>';"
"      list.forEach(function(item){"
"        var opt = document.createElement('option');"
"        opt.value = item.ssid;"
"        opt.innerText = item.ssid + ' (' + item.rssi + ' dBm)';"
"        sel.appendChild(opt);"
"      });"
"      sel.style.display = 'block';"
"      showStatus('Đã tìm thấy ' + list.length + ' mạng xung quanh!', 'success');"
"    })"
"    .catch(function(err){"
"      btn.innerText = '🔍 Quét Mạng Wi-Fi';"
"      btn.disabled = false;"
"      showStatus('Lỗi khi quét mạng: ' + err, 'error');"
"    });"
"}"
"function selectSsid() {"
"  var sel = document.getElementById('ssidSelect');"
"  if (sel.value) document.getElementById('ssid').value = sel.value;"
"}"
"function saveWifi() {"
"  var s = document.getElementById('ssid').value.trim();"
"  var p = document.getElementById('password').value;"
"  if (!s) { alert('Vui lòng nhập hoặc chọn tên Wi-Fi!'); return; }"
"  var btn = document.getElementById('btnSave');"
"  btn.innerText = '⏳ Đang gửi cấu hình...';"
"  btn.disabled = true;"
"  showStatus('Đang lưu và kết nối vào mạng ' + s + '...', 'info');"
"  fetch('/api/connect', {"
"    method: 'POST',"
"    headers: {'Content-Type': 'application/json'},"
"    body: JSON.stringify({ssid: s, password: p})"
"  })"
"  .then(function(r){ return r.json(); })"
"  .then(function(res){"
"    showStatus('✅ Đã lưu cấu hình! F91 đang kết nối vào mạng...', 'success');"
"    pollStatus();"
"  })"
"  .catch(function(err){"
"    btn.innerText = '💾 Lưu & Kết Nối';"
"    btn.disabled = false;"
"    showStatus('Lỗi gửi cấu hình: ' + err, 'error');"
"  });"
"}"
"function pollStatus() {"
"  setInterval(function(){"
"    fetch('/api/status')"
"      .then(function(r){ return r.json(); })"
"      .then(function(st){"
"        if (st.status === 'connected') {"
"          showStatus('🎉 KẾT NỐI THÀNH CÔNG!<br>IP: <b>' + st.ip + '</b><br>SSID: ' + st.ssid, 'success');"
"        } else if (st.status === 'failed') {"
"          showStatus('❌ Kết nối thất bại. Vui lòng kiểm tra lại mật khẩu!', 'error');"
"          document.getElementById('btnSave').disabled = false;"
"          document.getElementById('btnSave').innerText = '💾 Thử Lại';"
"        }"
"      });"
"  }, 2000);"
"}"
"</script>"
"</body>"
"</html>";

// GET / - Root Web Page
static esp_err_t get_root_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html_index, HTTPD_RESP_USE_STRLEN);
}

// GET /api/scan - Scan Wi-Fi APs and return JSON array
static esp_err_t get_scan_handler(httpd_req_t *req) {
    uint16_t ap_count = 15;
    wifi_ap_record_t *ap_records = malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (!ap_records) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    esp_err_t ret = wifi_manager_scan_networks(ap_records, &ap_count);
    char *buf = malloc(2048);
    if (!buf) {
        free(ap_records);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int offset = snprintf(buf, 2048, "[");
    if (ret == ESP_OK) {
        bool first = true;
        for (int i = 0; i < ap_count; i++) {
            if (strlen((char*)ap_records[i].ssid) == 0) continue;
            if (!first) offset += snprintf(buf + offset, 2048 - offset, ",");
            first = false;
            offset += snprintf(buf + offset, 2048 - offset,
                              "{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                              (char*)ap_records[i].ssid, (int)ap_records[i].rssi, (int)ap_records[i].authmode);
            if (offset >= 2040) break;
        }
    }
    snprintf(buf + offset, 2048 - offset, "]");
    free(ap_records);

    httpd_resp_set_type(req, "application/json");
    esp_err_t res = httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    free(buf);
    return res;
}

// POST /api/connect - Save credentials and connect
static esp_err_t post_connect_handler(httpd_req_t *req) {
    char buf[512];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char ssid[64] = {0};
    char pass[64] = {0};

    // Parse JSON
    char *s_ptr = strstr(buf, "\"ssid\"");
    if (s_ptr) {
        s_ptr = strchr(s_ptr, ':');
        if (s_ptr) {
            s_ptr = strchr(s_ptr, '"');
            if (s_ptr) {
                s_ptr++;
                char *end = strchr(s_ptr, '"');
                if (end) {
                    size_t len = end - s_ptr;
                    if (len >= sizeof(ssid)) len = sizeof(ssid) - 1;
                    strncpy(ssid, s_ptr, len);
                }
            }
        }
    }

    char *p_ptr = strstr(buf, "\"password\"");
    if (p_ptr) {
        p_ptr = strchr(p_ptr, ':');
        if (p_ptr) {
            p_ptr = strchr(p_ptr, '"');
            if (p_ptr) {
                p_ptr++;
                char *end = strchr(p_ptr, '"');
                if (end) {
                    size_t len = end - p_ptr;
                    if (len >= sizeof(pass)) len = sizeof(pass) - 1;
                    strncpy(pass, p_ptr, len);
                }
            }
        }
    }

    if (strlen(ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Web Portal received Wi-Fi credentials: SSID=\"%s\"", ssid);
    wifi_manager_save_credentials(ssid, pass);
    wifi_manager_connect_sta(ssid, pass);

    const char *resp = "{\"status\":\"ok\",\"message\":\"Saved and connecting\"}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

// GET /api/status - Get current Wi-Fi status
static esp_err_t get_status_handler(httpd_req_t *req) {
    wifi_mgr_status_t st = wifi_manager_get_status();
    char ip[32] = {0};
    char ssid[64] = {0};
    wifi_manager_get_ip(ip, sizeof(ip));
    wifi_manager_get_current_ssid(ssid, sizeof(ssid));
    int8_t rssi = wifi_manager_get_rssi();

    const char *st_str = "idle";
    if (st == WIFI_MGR_STATUS_CONNECTED) st_str = "connected";
    else if (st == WIFI_MGR_STATUS_CONNECTING) st_str = "connecting";
    else if (st == WIFI_MGR_STATUS_FAILED) st_str = "failed";
    else if (st == WIFI_MGR_STATUS_AP_ACTIVE) st_str = "ap_active";

    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"status\":\"%s\",\"ip\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d}",
             st_str, ip, ssid, (int)rssi);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

esp_err_t web_portal_start(void) {
    if (server) return ESP_OK;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size = 8192;

    ESP_LOGI(TAG, "Starting Web Portal HTTP Server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root_uri = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = get_root_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &root_uri);

        httpd_uri_t scan_uri = {
            .uri       = "/api/scan",
            .method    = HTTP_GET,
            .handler   = get_scan_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &scan_uri);

        httpd_uri_t connect_uri = {
            .uri       = "/api/connect",
            .method    = HTTP_POST,
            .handler   = post_connect_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &connect_uri);

        httpd_uri_t status_uri = {
            .uri       = "/api/status",
            .method    = HTTP_GET,
            .handler   = get_status_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &status_uri);

        ESP_LOGI(TAG, "Web Portal handlers registered successfully!");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Error starting Web Portal server!");
    return ESP_FAIL;
}

esp_err_t web_portal_stop(void) {
    if (server) {
        httpd_stop(server);
        server = NULL;
        ESP_LOGI(TAG, "Web Portal stopped.");
    }
    return ESP_OK;
}

bool web_portal_is_running(void) {
    return (server != NULL);
}
