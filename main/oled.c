/*
 * PPP-over-USB + WiFi SoftAP Router (ESP32-C3)
 *
 * OLED module (u8g2 + SSD1306).
 * Enhanced with WiFi signal strength indicator (horizontal RSSI bar).
 *
 * Author: Martin Köhler [martinkoehler]
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "oled.h"
#include "ap_config.h"
#include "mqtt_broker.h"
#include "web_server.h"
#include "client_rssi.h"

#include <limits.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"

#include "u8g2.h"
#include "u8g2_esp32_hal.h"

/* === OLED Configuration === */
#define OLED_SDA GPIO_NUM_5
#define OLED_SCL GPIO_NUM_6
#define OLED_RST U8G2_ESP32_HAL_UNDEFINED
#define OLED_DEBUG_BUTTON GPIO_NUM_9
#define BUTTON_DEBOUNCE_SAMPLES 2
#define BUTTON_LONG_PRESS_MS 1200
#define BUTTON_SEQUENCE_GAP_MS 2000
#define AUTH_TOGGLE_PRESS_COUNT 6
#define CREDENTIAL_VALUE_CHARS 12

static const int width   = 72;
static const int height  = 40;
static const int xOffset = 28;
static const int yOffset = 18;
static const uint8_t I2C_ADDR_8BIT = (0x3C << 1);

static const char *TAG = "oled";
static u8g2_t u8g2; // OLED driver handle

// statics (oben in der Datei, dort wo width/height/xOffset/yOffset sind)
static bool screensaver = false;
static int idle_seconds = 0;
static const int SCREENSAVER_DELAY = 60; // s
static const uint8_t CONTRAST = 125;   // 0..255 (für kleines Display niedrig halten)
static const uint8_t DIM_CONTRAST = 12;   // 0..255 (für kleines Display niedrig halten)
static int ss_x, ss_y, ss_dx = 1, ss_dy = 1;
static int normal_jitter_phase = 0;
static int blank_seconds = 0;
static int64_t last_web_check_us = 0;
static int last_web_status = 0;
static esp_err_t last_web_err = ESP_OK;
static bool debug_mode = false;
static bool debug_toggle_requested = false;
static portMUX_TYPE debug_toggle_lock = portMUX_INITIALIZER_UNLOCKED;
static unsigned credential_scroll_phase = 0;

static int get_connected_client_count(void)
{
    wifi_sta_list_t sta_list = {0};
    if (esp_wifi_ap_get_sta_list(&sta_list) != ESP_OK) {
        return 0;
    }
    return sta_list.num;
}

static char get_obk_connected_marker(void)
{
    int state = mqtt_broker_get_obk_connected_state();
    if (state > 0) return '+';
    if (state == 0) return '-';
    return 0;
}

/* WiFi signal strength indicator: best connected client RSSI. */
static int8_t get_best_client_rssi(void)
{
    return client_rssi_get_best();
}

/* Draw a horizontal RSSI bar with one pixel per dBm over the configured range. */
static void draw_signal_bar(u8g2_t *u8g2, int x, int y, int8_t rssi)
{
    enum {
        BAR_W = 58,
        BAR_H = 6,
        RSSI_MIN_DBM = -100,
        RSSI_MAX_DBM = -45,
    };

    u8g2_DrawFrame(u8g2, x, y, BAR_W, BAR_H);

    if (rssi == INT8_MIN) {
        return;
    }

    if (rssi < RSSI_MIN_DBM) {
        rssi = RSSI_MIN_DBM;
    } else if (rssi > RSSI_MAX_DBM) {
        rssi = RSSI_MAX_DBM;
    }

    uint8_t fill_w = 1 + (uint16_t)(rssi - RSSI_MIN_DBM) * (BAR_W - 3) /
                         (RSSI_MAX_DBM - RSSI_MIN_DBM);

    u8g2_DrawBox(u8g2, x + 1, y + 1, fill_w, BAR_H - 2);
}

// Hilfsfunktion: sichere Grenzen für die Animation berechnen
static void ss_init_bounds(void) {
    // verwende den Bereich innerhalb deines Offsets und der width/height
    int drawable_w = width;
    int drawable_h = height;
    // starte in der Mitte des nutzbaren Bereichs
    ss_x = xOffset + drawable_w / 2;
    ss_y = yOffset + drawable_h / 2;
    ss_dx = 1; ss_dy = 1;
}

static void draw_screensaver(u8g2_t *u8g2) {
    char buf[16];
    char marker = get_obk_connected_marker();
    if (marker) {
        snprintf(buf, sizeof(buf), "%d%c", get_connected_client_count(), marker);
    } else {
        snprintf(buf, sizeof(buf), "%d", get_connected_client_count());
    }

    u8g2_SetFont(u8g2, u8g2_font_6x10_tr);
    int text_w = u8g2_GetStrWidth(u8g2, buf);
    int text_h = 10; // baseline height for 6x10 font

    int min_x = xOffset + 1;
    int min_y = yOffset + text_h + 1;
    int max_x = xOffset + width - text_w - 1;
    int max_y = yOffset + height - 1;
    if (max_x < min_x) {
        min_x = max_x = xOffset + 1;
    }
    if (max_y < min_y) {
        min_y = max_y = yOffset + text_h + 1;
    }

    ss_x += ss_dx;
    ss_y += ss_dy;
    if (ss_x < min_x) { ss_x = min_x; ss_dx = -ss_dx; }
    if (ss_x > max_x) { ss_x = max_x; ss_dx = -ss_dx; }
    if (ss_y < min_y) { ss_y = min_y; ss_dy = -ss_dy; }
    if (ss_y > max_y) { ss_y = max_y; ss_dy = -ss_dy; }

    u8g2_ClearBuffer(u8g2);
    u8g2_DrawStr(u8g2, ss_x, ss_y, buf);
    u8g2_SendBuffer(u8g2);
}

static float parse_power(const char *s) {
    if (!s || !*s) return 0.0f;
    return strtof(s, NULL);
}

static void oled_button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << OLED_DEBUG_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&cfg);
}

static void toggle_debug_page(void)
{
    debug_mode = !debug_mode;
    screensaver = false;
    idle_seconds = 0;
    blank_seconds = 0;
    normal_jitter_phase = 0;
}

static void poll_debug_button(void)
{
    static int sampled_level = 1;
    static int stable_level = 1;
    static unsigned same_sample_count = 0;
    static TickType_t press_started_at = 0;
    static TickType_t last_short_release_at = 0;
    static unsigned short_press_count = 0;
    static bool long_press_handled = false;
    TickType_t now = xTaskGetTickCount();
    int level = gpio_get_level(OLED_DEBUG_BUTTON);

    if (stable_level == 1 && short_press_count > 0 &&
        (now - last_short_release_at) > pdMS_TO_TICKS(BUTTON_SEQUENCE_GAP_MS)) {
        short_press_count = 0;
    }

    if (level == sampled_level) {
        if (same_sample_count < BUTTON_DEBOUNCE_SAMPLES) {
            same_sample_count++;
        }
    } else {
        sampled_level = level;
        same_sample_count = 1;
    }

    if (stable_level == 0 && !long_press_handled &&
        (now - press_started_at) >= pdMS_TO_TICKS(BUTTON_LONG_PRESS_MS)) {
        toggle_debug_page();
        long_press_handled = true;
        short_press_count = 0;
        ESP_LOGI(TAG, "BOOT long press toggled OLED debug page");
    }

    if (same_sample_count < BUTTON_DEBOUNCE_SAMPLES ||
        sampled_level == stable_level) {
        return;
    }

    stable_level = sampled_level;
    if (stable_level == 0) {
        press_started_at = now;
        long_press_handled = false;
        return;
    }

    if (!long_press_handled) {
        if (short_press_count > 0 &&
            (now - last_short_release_at) > pdMS_TO_TICKS(BUTTON_SEQUENCE_GAP_MS)) {
            short_press_count = 0;
        }
        last_short_release_at = now;
        short_press_count++;

        if (short_press_count >= AUTH_TOGGLE_PRESS_COUNT) {
            bool auth_enabled = web_server_toggle_authentication();
            short_press_count = 0;
            credential_scroll_phase = 0;
            screensaver = false;
            idle_seconds = 0;
            blank_seconds = 0;
            u8g2_SetPowerSave(&u8g2, 0);
            u8g2_SetContrast(&u8g2, CONTRAST);
            ESP_LOGW(TAG, "Six BOOT presses set web authentication to %s",
                     auth_enabled ? "ON" : "OFF");
        }
    }
}

static void process_requested_debug_toggle(void)
{
    bool requested;

    portENTER_CRITICAL(&debug_toggle_lock);
    requested = debug_toggle_requested;
    debug_toggle_requested = false;
    portEXIT_CRITICAL(&debug_toggle_lock);

    if (requested) {
        toggle_debug_page();
    }
}

static void update_cached_web_health(void)
{
    web_server_get_cached_health(&last_web_status, &last_web_err,
                                 &last_web_check_us);
}

static void draw_debug_page(void)
{
    char line1[16];
    char line2[16];
    char line3[16];
    char web_state = web_server_is_running() ? 'R' : 'S';
    char health[8] = "H:--";
    int ota_pct = web_server_get_ota_progress();

    if (last_web_check_us != 0) {
        if (last_web_err == ESP_ERR_INVALID_STATE) {
            snprintf(health, sizeof(health), "H:NA");
        } else if (last_web_status > 0) {
            int code = last_web_status;
            if (code > 999) {
                code = 999;
            }
            snprintf(health, sizeof(health), "H:%d", code);
        } else {
            snprintf(health, sizeof(health), "H:ER");
        }
    }

    snprintf(line1, sizeof(line1), "W:%c %s", web_state, health);
    if (web_server_is_ota_in_progress()) {
        if (ota_pct >= 0) {
            snprintf(line2, sizeof(line2), "OTA:%d%%", ota_pct);
        } else {
            snprintf(line2, sizeof(line2), "OTA:--");
        }
    } else if (last_web_check_us == 0) {
        snprintf(line2, sizeof(line2), "E:--");
    } else if (last_web_err == ESP_OK) {
        snprintf(line2, sizeof(line2), "E:OK");
    } else {
        snprintf(line2, sizeof(line2), "E:%04X", (unsigned)last_web_err & 0xFFFF);
    }
    snprintf(line3, sizeof(line3), "AP:%c C:%d M:%c",
             ap_get_health_code(),
             get_connected_client_count(),
             mqtt_broker_is_running() ? 'R' : 'S');

    u8g2_ClearBuffer(&u8g2);
    u8g2_SetFont(&u8g2, u8g2_font_6x10_tr);
    u8g2_DrawStr(&u8g2, xOffset + 0, yOffset + 14, line1);
    u8g2_DrawStr(&u8g2, xOffset + 0, yOffset + 26, line2);
    u8g2_DrawStr(&u8g2, xOffset + 0, yOffset + 38, line3);
    u8g2_SendBuffer(&u8g2);
}

static void make_scrolling_value(const char *value, unsigned phase,
                                 char out[CREDENTIAL_VALUE_CHARS + 1])
{
    size_t len = strlen(value);
    if (len <= CREDENTIAL_VALUE_CHARS) {
        strlcpy(out, value, CREDENTIAL_VALUE_CHARS + 1);
        return;
    }

    size_t cycle_len = len + 3;
    size_t start = phase % cycle_len;
    for (size_t i = 0; i < CREDENTIAL_VALUE_CHARS; i++) {
        size_t pos = (start + i) % cycle_len;
        out[i] = pos < len ? value[pos] : ' ';
    }
    out[CREDENTIAL_VALUE_CHARS] = 0;
}

static void draw_credentials_page(void)
{
    char ssid[33];
    char password[65];
    char ssid_window[CREDENTIAL_VALUE_CHARS + 1];
    char password_window[CREDENTIAL_VALUE_CHARS + 1];
    char ssid_line[CREDENTIAL_VALUE_CHARS + 3];
    char password_line[CREDENTIAL_VALUE_CHARS + 3];

    ap_get_config_snapshot(ssid, sizeof(ssid), password, sizeof(password), NULL);
    const char *display_password = password[0] ? password : "<OPEN>";
    make_scrolling_value(ssid, credential_scroll_phase, ssid_window);
    make_scrolling_value(display_password, credential_scroll_phase,
                         password_window);
    credential_scroll_phase += 2;

    snprintf(ssid_line, sizeof(ssid_line), "S:%s", ssid_window);
    snprintf(password_line, sizeof(password_line), "P:%s", password_window);

    u8g2_SetPowerSave(&u8g2, 0);
    u8g2_SetContrast(&u8g2, CONTRAST);
    u8g2_ClearBuffer(&u8g2);
    u8g2_SetFont(&u8g2, u8g2_font_5x7_tr);
    u8g2_DrawStr(&u8g2, xOffset, yOffset + 9, "WEB AUTH:OFF");
    u8g2_DrawStr(&u8g2, xOffset, yOffset + 22, ssid_line);
    u8g2_DrawStr(&u8g2, xOffset, yOffset + 35, password_line);
    u8g2_SendBuffer(&u8g2);
}

/**
 * @brief Renders OLED UI: solar power value with WiFi signal strength.
 */

static void handle_oled(void)
{
    char power_copy[30];
    mqtt_broker_get_obk_power(power_copy, sizeof(power_copy));
    float p = parse_power(power_copy);

    if (!web_server_is_auth_enabled()) {
        screensaver = false;
        idle_seconds = 0;
        blank_seconds = 0;
        draw_credentials_page();
        return;
    }

    if (debug_mode) {
        screensaver = false;
        idle_seconds = 0;
        blank_seconds = 0;
        draw_debug_page();
        return;
    }

    if (blank_seconds > 0) {
        blank_seconds--;
        u8g2_SetPowerSave(&u8g2, 0);
        u8g2_SetContrast(&u8g2, CONTRAST);
        u8g2_ClearBuffer(&u8g2);
        u8g2_SendBuffer(&u8g2);
        return;
    }

    if (p <= 0.0001f) {
        idle_seconds++;
    } else {
        idle_seconds = 0;
        if (screensaver) {
            screensaver = false;
            u8g2_SetPowerSave(&u8g2, 0);
            u8g2_SetContrast(&u8g2, CONTRAST);
        }
    }

    if (!screensaver && idle_seconds >= SCREENSAVER_DELAY) {
        screensaver = true;
        // wähle Option: komplett aus ODER dim + animate
        // Option komplett aus:
        // u8g2_SetPowerSave(&u8g2, 1);
        // Option dim + animate:
        u8g2_SetPowerSave(&u8g2, 0);
        u8g2_SetContrast(&u8g2, DIM_CONTRAST);
        ss_init_bounds();
    }

    if (screensaver) {
        draw_screensaver(&u8g2);
        return;
    }

    // Normale Anzeige mit gelegentlichem, kleinem Jitter (alle Aufrufe cycles)
    normal_jitter_phase = (normal_jitter_phase + 1) % 60; // z.B. 1 px Veränderung pro Minute
    int xoff = xOffset;
    int yoff = yOffset;
    if (normal_jitter_phase == 0) xoff += 1;
    else if (normal_jitter_phase == 30) xoff -= 1;

    u8g2_ClearBuffer(&u8g2);
    u8g2_SetFont(&u8g2, u8g2_font_6x10_tr);
    u8g2_DrawStr(&u8g2, xoff + 0, yoff + 14, "Power (W)");

    u8g2_SetFont(&u8g2, u8g2_font_9x15_tr);
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%s", power_copy);
    u8g2_DrawStr(&u8g2, xoff + 0, yoff + 32, buffer);

    u8g2_SetFont(&u8g2, u8g2_font_6x10_tr);
    
    /* Display client count with WiFi signal strength indicator */
    int client_count = get_connected_client_count();
    int8_t client_rssi = get_best_client_rssi();
    
    /* Draw client count number */
    char count_str[4];
    snprintf(count_str, sizeof(count_str), "%d", client_count);
    u8g2_DrawStr(&u8g2, xoff + 0, yoff + 44, count_str);
    
    /* Draw OBK offline marker, otherwise show WiFi signal indicator. */
    char marker = get_obk_connected_marker();
    if (marker == '-') {
        u8g2_DrawStr(&u8g2, xoff + 12, yoff + 44, "-");
    } else if (client_count > 0) {
        draw_signal_bar(&u8g2, xoff + 12, yoff + 38, client_rssi);
    }

    u8g2_SendBuffer(&u8g2);
}

/**
 * @brief Task that periodically refreshes the OLED display.
 */
static void oled_task(void *arg)
{
    (void)arg;
    int tick = 0;
    while (1) {
        poll_debug_button();
        process_requested_debug_toggle();
        update_cached_web_health();
        if ((tick % 20) == 0) {
            handle_oled();
        }
        tick = (tick + 1) % 1000;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t oled_start(void)
{
    oled_button_init();
    u8g2_esp32_hal_t hal = U8G2_ESP32_HAL_DEFAULT;
    hal.bus.i2c.sda = OLED_SDA;
    hal.bus.i2c.scl = OLED_SCL;
    hal.reset = OLED_RST;
    hal.dc    = U8G2_ESP32_HAL_UNDEFINED;

    u8g2_esp32_hal_init(hal);

    u8g2_Setup_ssd1306_i2c_128x64_noname_f(
        &u8g2,
        U8G2_R0,
        u8g2_esp32_i2c_byte_cb,
        u8g2_esp32_gpio_and_delay_cb
    );

    u8g2_SetI2CAddress(&u8g2, I2C_ADDR_8BIT);
    u8g2_InitDisplay(&u8g2);
    u8g2_SetPowerSave(&u8g2, 0);
    u8g2_SetContrast(&u8g2, CONTRAST);

    ESP_LOGI(TAG, "OLED init OK. Active window %dx%d @ offset (%d,%d)",
             width, height, xOffset, yOffset);

    if (xTaskCreate(oled_task, "oled_task", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OLED task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void oled_blank_and_reset_screensaver(void)
{
    screensaver = false;
    idle_seconds = 0;
    normal_jitter_phase = 0;
    blank_seconds = 2;
}

void oled_request_debug_toggle(void)
{
    portENTER_CRITICAL(&debug_toggle_lock);
    debug_toggle_requested = true;
    portEXIT_CRITICAL(&debug_toggle_lock);
}
