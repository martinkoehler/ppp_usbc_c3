/*
 * Watchdog Utility Header for ESP32-C3 Projects
 *
 * Provides API for starting and stopping a background watchdog feed loop, ensuring
 * automatic MCU reset if tasks freeze. Implements Task Watchdog via ESP-IDF TWDT.
 *
 * Author: Martin Köhler [martinkoehler]
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Copyright (C) 2025 Martin Köhler
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file watchdog.h
 * @brief ESP32-C3 TWDT (Task Watchdog Timer) helper API for automatic system resets.
 *
 * Usage:
 *   - Include this header in your main application or modules.
 *   - In app_main(), call watchdog_start().
 *   - The watchdog_task will regularly feed TWDT; if the task is not scheduled, MCU resets.
 *   - You may call watchdog_deinit() to remove the feed loop and disable the watchdog.
 */

#pragma once

#include <stdint.h>

/**
 * @brief Start watchdog timer with feed loop in background task.
 *
 * @param timeout_seconds Watchdog timeout in seconds.
 * @param period_ms Interval between feeds, in milliseconds.
 */
void watchdog_start(uint32_t timeout_seconds, uint32_t period_ms);

/**
 * @brief Stop and deinitialize watchdog feed loop.
 */
void watchdog_deinit(void);
