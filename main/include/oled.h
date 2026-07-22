/*
 * PPP-over-USB + WiFi SoftAP Router (ESP32-C3)
 *
 * OLED module extracted from original monolithic source.
 *
 * Author: Martin Köhler [martinkoehler]
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GPL text omitted for brevity; see root license.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file oled.h
 * @brief OLED display lifecycle and periodic refresh task.
 *
 * Responsibilities:
 *  - Initialize u8g2 + I2C HAL for SSD1306 OLED.
 *  - Refresh screen periodically.
 *  - Display latest MQTT OBK power payload.
 */

esp_err_t oled_start(void);
void oled_blank_and_reset_screensaver(void);
/** Persistently enable or power-save the OLED. */
esp_err_t oled_set_enabled(bool enabled);
/** Return the persistent OLED enabled setting. */
bool oled_is_enabled(void);

/** Request the same debug-page toggle as a BOOT-button press. */
void oled_request_debug_toggle(void);

#ifdef __cplusplus
}
#endif
