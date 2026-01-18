/*
 * PPP-over-USB + WiFi SoftAP Router (ESP32-C3)
 *
 * OLED module (u8g2 + SSD1306).
 *
 * Author: Martin Köhler [martinkoehler]
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "oled.h"
#include "mqtt_broker.h"

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_wifi.h"

#include "u8g2.h"
#include "u8g2_esp32_hal.h"

/* === OLED Configuration === */
#define OLED_SDA GPIO_NUM_5
#define OLED_SCL GPIO_NUM_6
#define OLED_RST U8G2_ESP32_HAL_UNDEFINED

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

static int get_connected_client_count(void)
{
    wifi_sta_list_t sta_list = {0};
    if (esp_wifi_ap_get_sta_list(&sta_list) != ESP_OK) {
        return 0;
    }
    return sta_list.num;
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
    snprintf(buf, sizeof(buf), "%d", get_connected_client_count());

    u8g2_SetFont(u8g2, u8g2_font_6x10_tr);
    int text_w = u8g2_GetStrWidth(u8g2, buf);
    int text_h = 10; // baseline height for 6x10 font

    int min_x = xOffset + 1;
    int min_y = yOffset + text_h;
    int max_x = xOffset + width - text_w - 1;
    int max_y = yOffset + height - 2;
    if (max_x < min_x) {
        min_x = max_x = xOffset + 1;
    }
    if (max_y < min_y) {
        min_y = max_y = yOffset + text_h;
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

/**
 * @brief Renders OLED UI: solar power value, simple title.
 */

static void handle_oled(void)
{
    char power_copy[30];
    mqtt_broker_get_obk_power(power_copy, sizeof(power_copy));
    float p = parse_power(power_copy);

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
    u8g2_DrawStr(&u8g2, xoff + 0, yoff + 15, "Power (W)"); // Koordinaten an kleine Höhe anpassen
    u8g2_SetFont(&u8g2, u8g2_font_9x15_tr);
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%s", power_copy);
    u8g2_DrawStr(&u8g2, xoff + 0, yoff + 35, buffer);
    u8g2_SendBuffer(&u8g2);
}

/**
 * @brief Task that periodically refreshes the OLED display.
 */
static void oled_task(void *arg)
{
    (void)arg;
    while (1) {
        handle_oled();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void oled_start(void)
{
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

    xTaskCreate(oled_task, "oled_task", 4096, NULL, 5, NULL);
}
