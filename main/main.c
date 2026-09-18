#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "qrcodegen.h"
#include "vnpt_logo_center.h"
#include "vnpt_bg_data.h"

static const char *TAG = "F91_VNPT_STANDEE";

#define LCD_W 240
#define LCD_H 320
#define TOTAL_RAW_IMAGE_BYTES (LCD_W * LCD_H * 2) // 153,600 bytes (150 KB)

// Pinout mapping for ST7789 8-bit i80 Parallel LCD (F91 Hardware)
#define PIN_POWER_42     42
#define PIN_GPIO14       14
#define PIN_GPIO41       41
#define PIN_GPIO40       40
#define PIN_BK_LIGHT     21
#define PIN_LCD_RST      18

#define PIN_LCD_CS       13
#define PIN_LCD_DC       12
#define PIN_LCD_WR       11

static const int data_pins[8] = {10, 9, 8, 7, 6, 5, 3, 2}; // D0..D7

// Color Palette (RGB565 - INVOFF Mode)
#define COLOR_BLACK      0x0000
#define COLOR_WHITE      0xFFFF
#define COLOR_RED        0xF800
#define COLOR_VNPT_BLUE  0x02EB   // Official VNPT Blue RGB(0, 90, 171)

// Application States
typedef enum {
    STATE_STANDBY,
    STATE_QR_ACTIVE,
    STATE_RAW_STREAM,
    STATE_CUSTOM_RAW_PERM
} app_state_t;

static volatile app_state_t current_state = STATE_STANDBY;
static volatile int qr_countdown_sec = 0;
static int raw_stream_timeout = 0;
static size_t raw_stream_bytes_received = 0;
static bool raw_skip_newline = false;
static char current_payload[512] = "https://vnpt.vn";
static bool at_echo_enabled = true;

// Static QR Buffers (Prevents Stack Overflow)
static uint8_t qrcode_global[qrcodegen_BUFFER_LEN_MAX];
static uint8_t tempBuffer_global[qrcodegen_BUFFER_LEN_MAX];

// FreeRTOS Mutex for thread-safe LCD writes
static SemaphoreHandle_t lcd_mutex = NULL;

static inline void lcd_lock(void) {
    if (lcd_mutex) xSemaphoreTake(lcd_mutex, portMAX_DELAY);
}

static inline void lcd_unlock(void) {
    if (lcd_mutex) xSemaphoreGive(lcd_mutex);
}

// Set 8-bit data bus pins
static inline void set_data_bus(uint8_t byte) {
    for (int i = 0; i < 8; i++) {
        gpio_set_level(data_pins[i], (byte >> i) & 1);
    }
}

// Write 8-bit Command
static void lcd_write_cmd(uint8_t cmd) {
    gpio_set_level(PIN_LCD_DC, 0);
    gpio_set_level(PIN_LCD_CS, 0);
    set_data_bus(cmd);
    gpio_set_level(PIN_LCD_WR, 0);
    esp_rom_delay_us(1);
    gpio_set_level(PIN_LCD_WR, 1);
    esp_rom_delay_us(1);
    gpio_set_level(PIN_LCD_CS, 1);
}

// Write 8-bit Data
static void lcd_write_data(uint8_t data) {
    gpio_set_level(PIN_LCD_DC, 1);
    gpio_set_level(PIN_LCD_CS, 0);
    set_data_bus(data);
    gpio_set_level(PIN_LCD_WR, 0);
    esp_rom_delay_us(1);
    gpio_set_level(PIN_LCD_WR, 1);
    esp_rom_delay_us(1);
    gpio_set_level(PIN_LCD_CS, 1);
}

// Set address window
static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    lcd_write_cmd(0x2A);
    lcd_write_data((x0 >> 8) & 0xFF);
    lcd_write_data(x0 & 0xFF);
    lcd_write_data((x1 >> 8) & 0xFF);
    lcd_write_data(x1 & 0xFF);

    lcd_write_cmd(0x2B);
    lcd_write_data((y0 >> 8) & 0xFF);
    lcd_write_data(y0 & 0xFF);
    lcd_write_data((y1 >> 8) & 0xFF);
    lcd_write_data(y1 & 0xFF);

    lcd_write_cmd(0x2C);
}

// Fill rectangular area
static void lcd_fill_rect(int x, int y, int w, int h, uint16_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_W) w = LCD_W - x;
    if (y + h > LCD_H) h = LCD_H - y;
    if (w <= 0 || h <= 0) return;

    lcd_set_window(x, y, x + w - 1, y + h - 1);
    gpio_set_level(PIN_LCD_DC, 1);
    gpio_set_level(PIN_LCD_CS, 0);

    uint8_t high = (color >> 8) & 0xFF;
    uint8_t low  = color & 0xFF;

    int total_pixels = w * h;
    for (int i = 0; i < total_pixels; i++) {
        set_data_bus(high);
        gpio_set_level(PIN_LCD_WR, 0);
        esp_rom_delay_us(1);
        gpio_set_level(PIN_LCD_WR, 1);

        set_data_bus(low);
        gpio_set_level(PIN_LCD_WR, 0);
        esp_rom_delay_us(1);
        gpio_set_level(PIN_LCD_WR, 1);
    }
    gpio_set_level(PIN_LCD_CS, 1);
}

// Clear full screen with color
static void lcd_clear_screen(uint16_t color) {
    lcd_fill_rect(0, 0, LCD_W, LCD_H, color);
}

// Draw VNPT 36x36 Logo (at center of QR)
static void lcd_draw_vnpt_logo(int x0, int y0) {
    int x1 = x0 + VNPT_LOGO_WIDTH - 1;
    int y1 = y0 + VNPT_LOGO_HEIGHT - 1;
    if (x0 < 0 || y0 < 0 || x1 >= LCD_W || y1 >= LCD_H) return;

    lcd_set_window(x0, y0, x1, y1);
    gpio_set_level(PIN_LCD_DC, 1);
    gpio_set_level(PIN_LCD_CS, 0);

    for (int i = 0; i < VNPT_LOGO_WIDTH * VNPT_LOGO_HEIGHT; i++) {
        uint16_t color = vnpt_logo_center_pixels[i];

        set_data_bus((color >> 8) & 0xFF);
        gpio_set_level(PIN_LCD_WR, 0);
        esp_rom_delay_us(1);
        gpio_set_level(PIN_LCD_WR, 1);

        set_data_bus(color & 0xFF);
        gpio_set_level(PIN_LCD_WR, 0);
        esp_rom_delay_us(1);
        gpio_set_level(PIN_LCD_WR, 1);
    }
    gpio_set_level(PIN_LCD_CS, 1);
}

// 8x12 Font Digits Bitmap ('0'-'9', 's')
static const uint8_t font_8x12_digits[11][12] = {
    {0x3C,0x66,0x66,0x6E,0x76,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, // 0
    {0x18,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x00}, // 1
    {0x3C,0x66,0x06,0x06,0x0C,0x18,0x30,0x60,0x60,0x66,0x7E,0x00}, // 2
    {0x3C,0x66,0x06,0x06,0x1C,0x06,0x06,0x06,0x06,0x66,0x3C,0x00}, // 3
    {0x0C,0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 4
    {0x7E,0x60,0x60,0x60,0x7C,0x06,0x06,0x06,0x06,0x66,0x3C,0x00}, // 5
    {0x3C,0x66,0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, // 6
    {0x7E,0x66,0x06,0x0C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, // 7
    {0x3C,0x66,0x66,0x66,0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, // 8
    {0x3C,0x66,0x66,0x66,0x3E,0x06,0x06,0x06,0x06,0x66,0x3C,0x00}, // 9
    {0x00,0x00,0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00,0x00,0x00}  // s
};

// Draw Countdown Badge at bottom of screen (Centered at X=85, Y=280)
static void lcd_draw_countdown_badge(int seconds) {
    int badge_w = 70;
    int badge_h = 24;
    int badge_x = (LCD_W - badge_w) / 2;
    int badge_y = 280;

    // Draw red pill container
    lcd_fill_rect(badge_x, badge_y, badge_w, badge_h, COLOR_RED);

    int tens = (seconds / 10) % 10;
    int ones = seconds % 10;

    int digit_indices[3] = {tens, ones, 10}; // tens, ones, 's'
    int start_xs[3] = {badge_x + 14, badge_x + 28, badge_x + 44};

    for (int d = 0; d < 3; d++) {
        const uint8_t *glyph = font_8x12_digits[digit_indices[d]];
        int sx = start_xs[d];
        int sy = badge_y + 6;

        for (int r = 0; r < 12; r++) {
            uint8_t line = glyph[r];
            for (int c = 0; c < 8; c++) {
                if (line & (0x80 >> c)) {
                    lcd_fill_rect(sx + c, sy + r, 1, 1, COLOR_WHITE);
                }
            }
        }
    }
}

// Full ST7789 Initialization Sequence
static void lcd_init_st7789(void) {
    ESP_LOGI(TAG, "Initializing ST7789 (INVOFF Mode)...");

    gpio_set_level(PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));

    lcd_write_cmd(0x01); // SWRESET
    vTaskDelay(pdMS_TO_TICKS(150));

    lcd_write_cmd(0x11); // SLPOUT
    vTaskDelay(pdMS_TO_TICKS(120));

    lcd_write_cmd(0x36); // MADCTL
    lcd_write_data(0x00); 

    lcd_write_cmd(0x3A); // COLMOD 16-bit
    lcd_write_data(0x55);

    lcd_write_cmd(0xB2); // PORCTRL
    lcd_write_data(0x0C);
    lcd_write_data(0x0C);
    lcd_write_data(0x00);
    lcd_write_data(0x33);
    lcd_write_data(0x33);

    lcd_write_cmd(0xB7); // GCTRL
    lcd_write_data(0x35);

    lcd_write_cmd(0xBB); // VCOMS
    lcd_write_data(0x19);

    lcd_write_cmd(0xC0); // LCMCTRL
    lcd_write_data(0x2C);

    lcd_write_cmd(0xC2); // VDVVRHEN
    lcd_write_data(0x01);

    lcd_write_cmd(0xC3); // VRHS
    lcd_write_data(0x12);

    lcd_write_cmd(0xC4); // VDVS
    lcd_write_data(0x20);

    lcd_write_cmd(0xC6); // FRCTRL2 (60Hz)
    lcd_write_data(0x0F);

    lcd_write_cmd(0xD0); // PWCTRL1
    lcd_write_data(0xA4);
    lcd_write_data(0xA1);

    lcd_write_cmd(0xE0); // PVGAMCTRL
    lcd_write_data(0xD0);
    lcd_write_data(0x04);
    lcd_write_data(0x0D);
    lcd_write_data(0x11);
    lcd_write_data(0x13);
    lcd_write_data(0x2B);
    lcd_write_data(0x3F);
    lcd_write_data(0x54);
    lcd_write_data(0x4C);
    lcd_write_data(0x18);
    lcd_write_data(0x0D);
    lcd_write_data(0x0B);
    lcd_write_data(0x1F);
    lcd_write_data(0x23);

    lcd_write_cmd(0xE1); // NVGAMCTRL
    lcd_write_data(0xD0);
    lcd_write_data(0x04);
    lcd_write_data(0x0C);
    lcd_write_data(0x11);
    lcd_write_data(0x13);
    lcd_write_data(0x2C);
    lcd_write_data(0x3F);
    lcd_write_data(0x44);
    lcd_write_data(0x51);
    lcd_write_data(0x2F);
    lcd_write_data(0x1F);
    lcd_write_data(0x1F);
    lcd_write_data(0x20);
    lcd_write_data(0x23);

    lcd_write_cmd(0x20); // INVOFF (True Colors)

    lcd_write_cmd(0x29); // DISPON
    vTaskDelay(pdMS_TO_TICKS(100));
}

// Render High-Definition VNPT Standby Background (from Flash)
static void render_standby_screen(void) {
    lcd_lock();
    current_state = STATE_STANDBY;
    qr_countdown_sec = 0;

    // Stream entire high-definition 240x320 image from Flash (.rodata)
    lcd_set_window(0, 0, LCD_W - 1, LCD_H - 1);
    gpio_set_level(PIN_LCD_DC, 1);
    gpio_set_level(PIN_LCD_CS, 0);

    for (int i = 0; i < LCD_W * LCD_H; i++) {
        uint16_t color = vnpt_bg_pixels[i];
        set_data_bus((color >> 8) & 0xFF);
        gpio_set_level(PIN_LCD_WR, 0);
        esp_rom_delay_us(1);
        gpio_set_level(PIN_LCD_WR, 1);

        set_data_bus(color & 0xFF);
        gpio_set_level(PIN_LCD_WR, 0);
        esp_rom_delay_us(1);
        gpio_set_level(PIN_LCD_WR, 1);
    }
    gpio_set_level(PIN_LCD_CS, 1);

    lcd_unlock();
    ESP_LOGI(TAG, "High-Definition VNPT Standby Screen Rendered!");
}

// Generate & Render Payment QR Code with Center Logo and Countdown Badge
static bool render_qr_payment_screen(const char *qr_payload, int timeout_sec) {
    ESP_LOGI(TAG, "Rendering Payment QR: \"%s\" (Timeout: %ds)", qr_payload, timeout_sec);

    bool ok = qrcodegen_encodeText(
        qr_payload,
        tempBuffer_global,
        qrcode_global,
        qrcodegen_Ecc_HIGH,
        qrcodegen_VERSION_MIN,
        qrcodegen_VERSION_MAX,
        qrcodegen_Mask_AUTO,
        true
    );

    if (!ok) {
        ok = qrcodegen_encodeText(
            qr_payload,
            tempBuffer_global,
            qrcode_global,
            qrcodegen_Ecc_MEDIUM,
            qrcodegen_VERSION_MIN,
            qrcodegen_VERSION_MAX,
            qrcodegen_Mask_AUTO,
            true
        );
    }

    if (!ok) {
        ESP_LOGE(TAG, "QR Encoding failed!");
        return false;
    }

    int qr_size = qrcodegen_getSize(qrcode_global);
    // Scale QR to fit ~210x210 cleanly with margin for countdown badge
    int max_display_w = 210;
    int scale = max_display_w / qr_size;
    if (scale < 1) scale = 1;
    if (scale > 7) scale = 7;

    int qr_pixel_size = qr_size * scale;
    int start_x = (LCD_W - qr_pixel_size) / 2;
    int start_y = (timeout_sec > 0) ? (30 + (230 - qr_pixel_size) / 2) : ((LCD_H - qr_pixel_size) / 2);

    lcd_lock();

    // 1. Pure White Fullscreen Canvas
    lcd_clear_screen(COLOR_WHITE);

    // 2. Render QR Modules
    for (int y = 0; y < qr_size; y++) {
        for (int x = 0; x < qr_size; x++) {
            bool module = qrcodegen_getModule(qrcode_global, x, y);
            if (module) {
                lcd_fill_rect(start_x + (x * scale), start_y + (y * scale), scale, scale, COLOR_BLACK);
            }
        }
    }

    // 3. Center VNPT Logo (36x36) with White Pad
    int center_x = start_x + (qr_pixel_size / 2);
    int center_y = start_y + (qr_pixel_size / 2);
    int logo_pad = 4;
    int logo_box_x = center_x - (VNPT_LOGO_WIDTH / 2) - logo_pad;
    int logo_box_y = center_y - (VNPT_LOGO_HEIGHT / 2) - logo_pad;
    int logo_box_w = VNPT_LOGO_WIDTH + (logo_pad * 2);
    int logo_box_h = VNPT_LOGO_HEIGHT + (logo_pad * 2);

    lcd_fill_rect(logo_box_x, logo_box_y, logo_box_w, logo_box_h, COLOR_WHITE);
    lcd_draw_vnpt_logo(center_x - (VNPT_LOGO_WIDTH / 2), center_y - (VNPT_LOGO_HEIGHT / 2));

    // 4. Draw Initial Countdown Badge if timeout > 0
    if (timeout_sec > 0) {
        lcd_draw_countdown_badge(timeout_sec);
    }

    current_state = STATE_QR_ACTIVE;
    qr_countdown_sec = timeout_sec;

    lcd_unlock();
    ESP_LOGI(TAG, "Payment QR Screen updated on LCD successfully!");
    return true;
}

// Process AT Commands from USB Serial
static void process_at_command(const char *cmd) {
    ESP_LOGI(TAG, "Processing AT Command: \"%s\"", cmd);

    // 1. AT / ATE0 / ATE1
    if (strcmp(cmd, "AT") == 0) {
        printf("OK\r\n");
        fflush(stdout);
        return;
    }
    if (strcmp(cmd, "ATE0") == 0) {
        at_echo_enabled = false;
        printf("OK\r\n");
        fflush(stdout);
        return;
    }
    if (strcmp(cmd, "ATE1") == 0) {
        at_echo_enabled = true;
        printf("OK\r\n");
        fflush(stdout);
        return;
    }

    // 2. AT+VER
    if (strcmp(cmd, "AT+VER") == 0) {
        printf("+VER: F91_VNPT_STANDEE_V2.0\r\nOK\r\n");
        fflush(stdout);
        return;
    }

    // 3. AT+BG / AT+CLEAR / AT+DISPLAY_CLEAR
    if (strcmp(cmd, "AT+BG") == 0 || strcmp(cmd, "AT+CLEAR") == 0 || strcmp(cmd, "AT+DISPLAY_CLEAR") == 0) {
        render_standby_screen();
        printf("OK\r\n");
        fflush(stdout);
        return;
    }

    // 4. AT+DTIME=<timeout>  or  AT+DTIME=0  (Raw 150KB Stream Mode)
    if (strncmp(cmd, "AT+DTIME=", 9) == 0) {
        int timeout = atoi(cmd + 9);
        raw_stream_timeout = timeout;
        raw_stream_bytes_received = 0;
        raw_skip_newline = true;

        lcd_lock();
        lcd_set_window(0, 0, LCD_W - 1, LCD_H - 1);
        gpio_set_level(PIN_LCD_DC, 1);
        gpio_set_level(PIN_LCD_CS, 0);
        current_state = STATE_RAW_STREAM;
        qr_countdown_sec = 0;

        ESP_LOGI(TAG, "RAW STREAM MODE ACTIVE. Waiting for %d bytes...", TOTAL_RAW_IMAGE_BYTES);
        printf("OK\r\n");
        fflush(stdout);
        return;
    }

    // 5. AT+QR=<payload>,<timeout>  or  AT+QR="<payload>",<timeout>
    if (strncmp(cmd, "AT+QR=", 6) == 0) {
        const char *p = cmd + 6;
        while (*p == ' ' || *p == '\t') p++;

        char payload[512] = {0};
        int timeout = 30;

        if (*p == '"') {
            p++;
            const char *close_quote = strrchr(p, '"');
            if (close_quote) {
                size_t plen = close_quote - p;
                if (plen >= sizeof(payload)) plen = sizeof(payload) - 1;
                strncpy(payload, p, plen);
                payload[plen] = '\0';

                const char *comma = strchr(close_quote + 1, ',');
                if (comma) timeout = atoi(comma + 1);
            } else {
                strncpy(payload, p, sizeof(payload) - 1);
            }
        } else {
            const char *comma = strrchr(p, ',');
            if (comma && comma > p && (comma[1] >= '0' && comma[1] <= '9')) {
                size_t plen = comma - p;
                if (plen >= sizeof(payload)) plen = sizeof(payload) - 1;
                strncpy(payload, p, plen);
                payload[plen] = '\0';
                timeout = atoi(comma + 1);
            } else {
                strncpy(payload, p, sizeof(payload) - 1);
            }
        }

        if (strlen(payload) > 0) {
            strncpy(current_payload, payload, sizeof(current_payload) - 1);
            render_qr_payment_screen(current_payload, timeout);
            printf("+QR: OK\r\n");
        } else {
            printf("+QR: ERROR\r\n");
        }
        fflush(stdout);
        return;
    }

    // 6. AT+QR_DISPLAY=<timeout>,<payload> (F91 Hemipay standard protocol)
    if (strncmp(cmd, "AT+QR_DISPLAY=", 14) == 0) {
        const char *p = cmd + 14;
        int timeout = atoi(p);
        const char *comma = strchr(p, ',');
        const char *payload = comma ? (comma + 1) : p;

        while (*payload == ' ' || *payload == '\t') payload++;
        if (strlen(payload) > 0) {
            strncpy(current_payload, payload, sizeof(current_payload) - 1);
            render_qr_payment_screen(current_payload, timeout);
            printf("+QR_DISPLAY: OK\r\n");
        } else {
            printf("+QR_DISPLAY: ERROR\r\n");
        }
        fflush(stdout);
        return;
    }

    printf("ERROR\r\n");
    fflush(stdout);
}

// USB Serial Reader Task
static void usb_serial_task(void *pvParameters) {
    char rx_line[512];
    size_t rx_idx = 0;
    uint8_t rx_chunk[512];
    int raw_idle_count = 0;

    fcntl(0, F_SETFL, O_NONBLOCK);

    while (1) {
        if (current_state == STATE_RAW_STREAM) {
            int bytes = read(0, rx_chunk, sizeof(rx_chunk));
            if (bytes > 0) {
                raw_idle_count = 0;
                for (int i = 0; i < bytes; i++) {
                    uint8_t b = rx_chunk[i];
                    if (raw_skip_newline) {
                        if (b == '\r' || b == '\n' || b == ' ') continue;
                        raw_skip_newline = false;
                    }

                    if (raw_stream_bytes_received < TOTAL_RAW_IMAGE_BYTES) {
                        set_data_bus(b);
                        gpio_set_level(PIN_LCD_WR, 0);
                        esp_rom_delay_us(1);
                        gpio_set_level(PIN_LCD_WR, 1);
                        raw_stream_bytes_received++;
                    }
                }

                if (raw_stream_bytes_received >= TOTAL_RAW_IMAGE_BYTES) {
                    gpio_set_level(PIN_LCD_CS, 1);
                    lcd_unlock();

                    ESP_LOGI(TAG, "RAW IMAGE RECEIVED (153,600 bytes)!");
                    printf("+DTIME: OK\r\n");
                    fflush(stdout);

                    if (raw_stream_timeout > 0) {
                        qr_countdown_sec = raw_stream_timeout;
                        current_state = STATE_QR_ACTIVE;
                    } else {
                        current_state = STATE_CUSTOM_RAW_PERM;
                    }
                }
            } else {
                raw_idle_count++;
                if (raw_idle_count > 1500) {
                    gpio_set_level(PIN_LCD_CS, 1);
                    lcd_unlock();
                    ESP_LOGW(TAG, "RAW STREAM TIMEOUT (%d bytes received)", (int)raw_stream_bytes_received);
                    printf("+DTIME: TIMEOUT\r\n");
                    fflush(stdout);
                    render_standby_screen();
                    raw_idle_count = 0;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        } else {
            uint8_t ch;
            int bytes = read(0, &ch, 1);
            if (bytes > 0) {
                if (ch == '\r' || ch == '\n') {
                    if (rx_idx > 0) {
                        rx_line[rx_idx] = '\0';
                        if (at_echo_enabled) {
                            printf("\r\n");
                            fflush(stdout);
                        }
                        process_at_command(rx_line);
                        rx_idx = 0;
                    }
                } else if (ch == '\b' || ch == 127) {
                    if (rx_idx > 0) {
                        rx_idx--;
                        if (at_echo_enabled) {
                            printf("\b \b");
                            fflush(stdout);
                        }
                    }
                } else if (ch >= 32 && ch != 127 && rx_idx < sizeof(rx_line) - 1) {
                    rx_line[rx_idx++] = ch;
                    if (at_echo_enabled) {
                        putchar(ch);
                        fflush(stdout);
                    }
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    }
}

// Timer Task for QR Timeout Auto-Reset & Countdown Badge Update
static void timer_task(void *pvParameters) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        lcd_lock();
        if (current_state == STATE_QR_ACTIVE && qr_countdown_sec > 0) {
            qr_countdown_sec--;
            if (qr_countdown_sec > 0) {
                lcd_draw_countdown_badge(qr_countdown_sec);
                lcd_unlock();
            } else {
                lcd_unlock();
                ESP_LOGI(TAG, "QR Timeout reached (0s). Returning to HD Standby Screen...");
                render_standby_screen();
            }
        } else {
            lcd_unlock();
        }
    }
}

void app_main(void) {
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, " F91 VNPT Standee (HD Background & Countdown UI)  ");
    ESP_LOGI(TAG, "==================================================");

    // Create FreeRTOS Mutex
    lcd_mutex = xSemaphoreCreateMutex();

    // 1. Configure Power and Control GPIOs
    gpio_reset_pin(PIN_POWER_42);
    gpio_set_direction(PIN_POWER_42, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_POWER_42, 1);

    gpio_reset_pin(PIN_GPIO14);
    gpio_set_direction(PIN_GPIO14, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_GPIO14, 1);

    gpio_reset_pin(PIN_GPIO41);
    gpio_set_direction(PIN_GPIO41, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_GPIO41, 0);

    gpio_reset_pin(PIN_GPIO40);
    gpio_set_direction(PIN_GPIO40, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_GPIO40, 0);

    gpio_reset_pin(PIN_BK_LIGHT);
    gpio_set_direction(PIN_BK_LIGHT, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_BK_LIGHT, 0);

    // 2. Configure 8080 Bus Control Pins
    gpio_reset_pin(PIN_LCD_CS);
    gpio_set_direction(PIN_LCD_CS, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LCD_CS, 1);

    gpio_reset_pin(PIN_LCD_DC);
    gpio_set_direction(PIN_LCD_DC, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LCD_DC, 1);

    gpio_reset_pin(PIN_LCD_WR);
    gpio_set_direction(PIN_LCD_WR, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LCD_WR, 1);

    gpio_reset_pin(PIN_LCD_RST);
    gpio_set_direction(PIN_LCD_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LCD_RST, 1);

    // 3. Configure 8-bit Data Bus Pins
    for (int i = 0; i < 8; i++) {
        gpio_reset_pin(data_pins[i]);
        gpio_set_direction(data_pins[i], GPIO_MODE_OUTPUT);
        gpio_set_level(data_pins[i], 0);
    }

    // 4. Initialize ST7789 with full sequence
    lcd_init_st7789();

    // 5. Turn ON Backlight
    gpio_set_level(PIN_BK_LIGHT, 1);

    // 6. Show Initial High-Definition VNPT Standby Screen
    render_standby_screen();

    // 7. Create FreeRTOS Tasks
    xTaskCreate(usb_serial_task, "usb_serial_task", 8192, NULL, 5, NULL);
    xTaskCreate(timer_task, "timer_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "System Ready! Listening for AT commands on USB Serial...");
}
