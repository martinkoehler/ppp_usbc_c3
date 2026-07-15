/*
 * PPP-over-USB + WiFi SoftAP Router (ESP32-C3)
 *
 * PPP module extracted from original monolithic source.
 *
 * Author: Martin Köhler [martinkoehler]
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "lwip/ip4_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file ppp.h
 * @brief PPP-over-USB Serial/JTAG module.
 *
 * Responsibilities:
 *  - Install USB Serial/JTAG driver.
 *  - Create PPPoS instance.
 *  - Feed RX bytes into lwIP PPP.
 *  - Provide PPO interface status/IP info.
 *  - Run reconnect loop in background.
 */

esp_err_t ppp_usb_start(void);

/** Is PPP link currently up? */
bool ppp_is_up(void);

/**
 * @brief Get PPP IP/GW/NM. If PPP not up, addresses are 0.
 */
void ppp_get_ip_info(ip4_addr_t *ip, ip4_addr_t *gw, ip4_addr_t *nm);

/**
 * @brief Get PPP IP only (convenience).
 */
ip4_addr_t ppp_get_ip(void);

#ifdef __cplusplus
}
#endif
