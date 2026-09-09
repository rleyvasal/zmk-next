/*
 * Copyright (c) 2021 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

uint8_t zmk_battery_state_of_charge(void);

/* Last SoC sampled while this half was not on USB. -1 if never known.
 * Use this for host UI while USB is holding the divider at charge voltage. */
int zmk_battery_last_cell_soc(void);

/* Last ADC millivolts (0 if not sampled yet). USB charge is typically >= 4200. */
uint16_t zmk_battery_millivolts(void);
