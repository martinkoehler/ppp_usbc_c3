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

/**
 * @brief Renders OLED UI: solar power value, simple title.
 */
static void handle_oled(void)
{
    char power_copy[30];
    mqtt_broker_get_obk_power(power_copy, sizeof(power_copy));

    u8g2_ClearBuffer(&u8g2);
    u8g2_SetFont(&u8g2, u8g2_font_6x10_tr);

    u8g2_DrawStr(&u8g2, xOffset + 0, yOffset + 20, "Solar power");
    u8g2_SetFont(&u8g2, u8g2_font_9x15_tr);
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%s W", power_copy);
    u8g2_DrawStr(&u8g2, xOffset + 0, yOffset + 40, buffer);

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
    u8g2_SetContrast(&u8g2, 255);

    ESP_LOGI(TAG, "OLED init OK. Active window %dx%d @ offset (%d,%d)",
             width, height, xOffset, yOffset);

    xTaskCreate(oled_task, "oled_task", 4096, NULL, 5, NULL);
}

