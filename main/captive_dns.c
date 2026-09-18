/**
 * @file captive_dns.c
 * @brief Xem mo ta trong captive_dns.h
 */
#include "captive_dns.h"

#include <string.h>
#include <errno.h>
#include <stdio.h>
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

/* Fallback neu Kconfig chua dinh nghia */
#ifndef CONFIG_PROV_AP_IP
#define CONFIG_PROV_AP_IP "192.168.4.1"
#endif

static const char *TAG = "captive_dns";

#define DNS_PORT     53
#define DNS_BUF_LEN  512

/* Header DNS toi gian theo RFC 1035 (12 byte dau tien cua moi goi tin) */
typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;

/* Bien noi bo de dung task */
static TaskHandle_t s_dns_task_handle = NULL;
static volatile int s_dns_socket      = -1;
static volatile bool s_dns_running    = false;

static void captive_dns_task(void *pvParameters)
{
    char rx_buffer[DNS_BUF_LEN];

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Khong tao duoc socket UDP: errno %d", errno);
        s_dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    s_dns_socket = sock;

    /* Dat timeout de recvfrom khong block mai mai → giup stop() hieu qua hon */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(DNS_PORT),
    };

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Khong bind duoc port %d: errno %d", DNS_PORT, errno);
        close(sock);
        s_dns_socket = -1;
        s_dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Captive-portal DNS server dang lang nghe tren UDP port %d", DNS_PORT);

    while (s_dns_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0,
                            (struct sockaddr *)&client_addr, &client_len);
        if (len < 0) {
            /* timeout (EAGAIN/EWOULDBLOCK) hoac loi thuc su */
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue; /* kiem tra lai co s_dns_running khong */
            }
            if (s_dns_running) {
                ESP_LOGW(TAG, "recvfrom loi: errno %d", errno);
            }
            break;
        }
        if (len < (int)sizeof(dns_header_t)) {
            continue; /* goi tin qua ngan, khong hop le */
        }

        /* Sua header: danh dau day la 1 response hop le, co 1 answer record */
        dns_header_t *header = (dns_header_t *)rx_buffer;
        header->flags   = htons(0x8180); /* QR=1 (response), RA=1, RCODE=0 */
        header->ancount = htons(1);
        header->nscount = 0;
        header->arcount = 0;

        /* Tim diem ket thuc phan Question de biet cho noi chen Answer */
        int pos = sizeof(dns_header_t);
        while (pos < len && rx_buffer[pos] != 0) {
            pos += (uint8_t)rx_buffer[pos] + 1;
            if (pos >= DNS_BUF_LEN) break;
        }
        pos += 1 /* byte 0x00 ket thuc QNAME */ + 4 /* QTYPE + QCLASS */;

        if (pos <= sizeof(dns_header_t) || pos + 16 > DNS_BUF_LEN || pos > len) {
            continue;
        }

        /* Answer record: tro moi ten mien ve IP cua SoftAP (CONFIG_PROV_AP_IP) */
        uint8_t a = 192, b = 168, c = 4, d = 1; /* gia tri mac dinh */
        sscanf(CONFIG_PROV_AP_IP, "%hhu.%hhu.%hhu.%hhu", &a, &b, &c, &d);
        const uint8_t answer[16] = {
            0xC0, 0x0C,                  /* NAME: con tro nen (pointer) ve offset 12 */
            0x00, 0x01,                  /* TYPE: A record (IPv4)                    */
            0x00, 0x01,                  /* CLASS: IN                                */
            0x00, 0x00, 0x00, 0x3C,      /* TTL: 60 giay                             */
            0x00, 0x04,                  /* RDLENGTH: 4 byte                         */
            a, b, c, d,                  /* RDATA: lay tu CONFIG_PROV_AP_IP          */
        };
        memcpy(rx_buffer + pos, answer, sizeof(answer));
        int response_len = pos + (int)sizeof(answer);

        sendto(sock, rx_buffer, response_len, 0, (struct sockaddr *)&client_addr, client_len);
    }

    close(sock);
    s_dns_socket      = -1;
    s_dns_task_handle = NULL;
    ESP_LOGI(TAG, "DNS task da dung");
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------- */
void captive_dns_start(void)
{
    if (s_dns_running) {
        ESP_LOGW(TAG, "DNS server da dang chay");
        return;
    }
    s_dns_running = true;
    xTaskCreate(captive_dns_task, "captive_dns", 4096, NULL, 5, &s_dns_task_handle);
}

void captive_dns_stop(void)
{
    if (!s_dns_running) return;

    s_dns_running = false;

    /* Dong socket buoc recvfrom ket thuc ngay lap tuc thay vi cho timeout */
    if (s_dns_socket >= 0) {
        close(s_dns_socket);
        s_dns_socket = -1;
    }

    /* Cho task tu xoa (co the mat toi 1s do SO_RCVTIMEO) */
    /* Task tu goi vTaskDelete(NULL) khi thoat vong lap */
    ESP_LOGI(TAG, "Da gui lenh dung DNS server");
}