/*
 * Copyright (c) 2021 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

enum zmk_battery_charge_state {
    /* On battery. */
    ZMK_BATTERY_CHARGE_STATE_DISCHARGING,
    /* On external power and charging (or no charger status to say otherwise). */
    ZMK_BATTERY_CHARGE_STATE_CHARGING,
    /* On external power and the charger is not charging: the charge is complete,
     * or the cell was above the charger's recharge threshold when plugged in. */
    ZMK_BATTERY_CHARGE_STATE_FULL,
};

/* Live reading of the last sample. On external power it reflects charge voltage. */
uint8_t zmk_battery_state_of_charge(void);

/* Best known level of the cell, -1 if never known: the last reading on battery,
 * then while charging an estimate (with a zmk,battery-charger current and
 * capacity) or 100 once a charge since plugging in has completed. Always -1
 * without CONFIG_ZMK_BATTERY_HOLD_ON_EXTERNAL_POWER. */
int zmk_battery_last_cell_soc(void);

/* The level to show for this half: the live reading on battery, but with
 * CONFIG_ZMK_BATTERY_HOLD_ON_EXTERNAL_POWER the cell level while on external
 * power and for CONFIG_ZMK_BATTERY_SETTLE_AFTER_CHARGE_SEC after it is removed. */
uint8_t zmk_battery_reported_soc(void);

/* Always ZMK_BATTERY_CHARGE_STATE_DISCHARGING without
 * CONFIG_ZMK_BATTERY_HOLD_ON_EXTERNAL_POWER. */
enum zmk_battery_charge_state zmk_battery_charge_state(void);

/* Last measured battery voltage in millivolts: from the voltage fetch in LITHIUM_VOLTAGE mode, or
 * the gauge's voltage channel in STATE_OF_CHARGE mode. 0 if not sampled yet or the gauge has no
 * voltage channel. USB charge is typically >= 4200. */
uint16_t zmk_battery_millivolts(void);
