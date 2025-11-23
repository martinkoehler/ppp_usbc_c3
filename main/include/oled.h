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

void oled_start(void);

#ifdef __cplusplus
}
#endif

