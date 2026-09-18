/**
 * @file internet_check.c
 * @brief Xem mo ta trong internet_check.h
 */
#include "internet_check.h"

#include "ping/ping_sock.h"
#include "lwip/inet.h"
#include "lwip/ip_addr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "internet_check";

#ifndef CONFIG_INTERNET_CHECK_IP
#define CONFIG_INTERNET_CHECK_IP "1.1.1.1"
#endif

#ifndef CONFIG_INTERNET_CHECK_FALLBACK_IP
#define CONFIG_INTERNET_CHECK_FALLBACK_IP "8.8.8.8"
#endif

#ifndef CONFIG_INTERNET_CHECK_RETRY_SEC
#define CONFIG_INTERNET_CHECK_RETRY_SEC 30
#endif

static internet_check_cb_t s_callback           = NULL;
static esp_ping_handle_t   s_ping               = NULL;
static esp_timer_handle_t  s_retry_timer        = NULL;
static bool                s_stopped            = false;
static bool                s_is_fallback_active = false;

/* Khai bao truoc */
static void do_ping(const char *ip_str);

/* ----- Retry timer callback ----- */
static void retry_timer_cb(void *arg)
{
    if (s_stopped) return;
    ESP_LOGI(TAG, "Thu ping lai sau %d giay...", CONFIG_INTERNET_CHECK_RETRY_SEC);
    s_is_fallback_active = false;
    do_ping(CONFIG_INTERNET_CHECK_IP);
}

/* ----- Ping callbacks ----- */
static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    uint32_t elapsed_ms;
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_ms, sizeof(elapsed_ms));
    ESP_LOGI(TAG, "Ping reply nhan duoc, RTT=%"PRIu32"ms", elapsed_ms);
}

static void on_ping_timeout(esp_ping_handle_t hdl, void *args)
{
    ESP_LOGW(TAG, "Ping timeout");
}

static void on_ping_end(esp_ping_handle_t hdl, void *args)
{
    uint32_t sent, received;
    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &sent,     sizeof(sent));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY,   &received, sizeof(received));

    esp_ping_delete_session(hdl);
    s_ping = NULL;

    if (s_stopped) return;   /* da bi huy truoc khi ket thuc */

    if (received > 0) {
        /* Ping thanh cong (it nhat 1 goi co phan hoi) */
        ESP_LOGI(TAG, "Ket qua: %"PRIu32"/%"PRIu32" goi thanh cong (%s) -> Internet: CO",
                 received, sent, s_is_fallback_active ? CONFIG_INTERNET_CHECK_FALLBACK_IP : CONFIG_INTERNET_CHECK_IP);
        s_is_fallback_active = false;

        if (s_callback) {
            s_callback(true);
        }
    } else {
        /* Ca 3 goi deu timeout */
        if (!s_is_fallback_active) {
            /* Ping primary (1.1.1.1) fail -> chuyen sang ping fallback (8.8.8.8) */
            s_is_fallback_active = true;
            ESP_LOGW(TAG, "Ping %s timeout (0/%"PRIu32"). Chuyen sang ping du phong %s...",
                     CONFIG_INTERNET_CHECK_IP, sent, CONFIG_INTERNET_CHECK_FALLBACK_IP);
            do_ping(CONFIG_INTERNET_CHECK_FALLBACK_IP);
        } else {
            /* Ca primary lan fallback (8.8.8.8) deu fail -> ket luan khong co Internet */
            ESP_LOGE(TAG, "Ca %s va %s deu timeout (0/%"PRIu32") -> Internet: KHONG",
                     CONFIG_INTERNET_CHECK_IP, CONFIG_INTERNET_CHECK_FALLBACK_IP, sent);
            s_is_fallback_active = false;

            if (s_callback) {
                s_callback(false);
            }

            if (s_retry_timer && !s_stopped) {
                esp_timer_start_once(s_retry_timer,
                    (uint64_t)CONFIG_INTERNET_CHECK_RETRY_SEC * 1000000ULL);
            }
        }
    }
}

/* ----- Bat dau 1 phien ping ----- */
static void do_ping(const char *ip_str)
{
    if (s_ping != NULL) {
        ESP_LOGW(TAG, "Ping dang chay, bo qua");
        return;
    }

    ip_addr_t target;
    if (!ipaddr_aton(ip_str, &target)) {
        ESP_LOGE(TAG, "IP khong hop le: %s", ip_str);
        return;
    }

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr  = target;
    cfg.count        = 3;       /* ping 3 lan */
    cfg.interval_ms  = 1000;
    cfg.timeout_ms   = 2000;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = on_ping_success,
        .on_ping_timeout = on_ping_timeout,
        .on_ping_end     = on_ping_end,
        .cb_args         = NULL,
    };

    esp_err_t err = esp_ping_new_session(&cfg, &cbs, &s_ping);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Khong tao duoc ping session (%s): %s", ip_str, esp_err_to_name(err));
        return;
    }

    esp_ping_start(s_ping);
    ESP_LOGI(TAG, "Dang ping %s...", ip_str);
}

/* -------------------------------------------------------------------- */
esp_err_t internet_check_start(internet_check_cb_t cb)
{
    s_callback           = cb;
    s_stopped            = false;
    s_is_fallback_active = false;

    /* Tao retry timer neu chua co */
    if (!s_retry_timer) {
        const esp_timer_create_args_t timer_args = {
            .callback        = retry_timer_cb,
            .name            = "inet_retry",
            .dispatch_method = ESP_TIMER_TASK,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_retry_timer));
    }

    do_ping(CONFIG_INTERNET_CHECK_IP);
    return ESP_OK;
}

void internet_check_stop(void)
{
    s_stopped            = true;
    s_callback           = NULL;
    s_is_fallback_active = false;

    /* Dung retry timer */
    if (s_retry_timer && esp_timer_is_active(s_retry_timer)) {
        esp_timer_stop(s_retry_timer);
    }

    /* Dung ping session dang chay */
    if (s_ping) {
        esp_ping_stop(s_ping);
        esp_ping_delete_session(s_ping);
        s_ping = NULL;
    }
}
