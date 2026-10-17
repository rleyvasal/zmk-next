/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zmk/keys.h>
#include <zmk/ble/profile.h>

#define ZMK_BLE_IS_CENTRAL                                                                         \
    (IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))

#if ZMK_BLE_IS_CENTRAL
#define ZMK_BLE_PROFILE_COUNT (CONFIG_BT_MAX_PAIRED - CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS)
#define ZMK_SPLIT_BLE_PERIPHERAL_COUNT CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS
#else
#define ZMK_BLE_PROFILE_COUNT CONFIG_BT_MAX_PAIRED
#endif

void zmk_ble_clear_bonds(void);
int zmk_ble_prof_next(void);
int zmk_ble_prof_prev(void);
int zmk_ble_prof_select(uint8_t index);
void zmk_ble_clear_all_bonds(void);
int zmk_ble_prof_disconnect(uint8_t index);

int zmk_ble_active_profile_index(void);
int zmk_ble_profile_index(const bt_addr_le_t *addr);
bt_addr_le_t *zmk_ble_profile_address(uint8_t index);

bt_addr_le_t *zmk_ble_active_profile_addr(void);
struct bt_conn *zmk_ble_active_profile_conn(void);

bool zmk_ble_profile_is_connected(uint8_t index);
bool zmk_ble_profile_is_open(uint8_t index);

bool zmk_ble_active_profile_is_open(void);
bool zmk_ble_active_profile_is_connected(void);
char *zmk_ble_active_profile_name(void);

/* Host advertising state, for modules that watch or recover host connections. */
/* True while host advertising is held off: paused by CONFIG_ZMK_BLE_ADV_TIMEOUT, or on USB power
 * with CONFIG_ZMK_BLE_HOST_ADV_OFF_ON_USB. */
bool zmk_ble_host_adv_suppressed(void);
/* "on" (open or directed advertising), "dark" (suppressed), or "off". */
const char *zmk_ble_adv_state(void);
/* Leave the timeout pause and advertise again (with the boost, if enabled). */
void zmk_ble_adv_wake(void);
/* Restart the advertising boost window without leaving a pause. */
void zmk_ble_adv_boost_rearm(void);
/* Start open advertising if it should be running but isn't. */
void zmk_ble_adv_kick(void);

enum zmk_ble_adv_stage {
    ZMK_BLE_ADV_STOP,
    ZMK_BLE_ADV_PEER,
    ZMK_BLE_ADV_CLEAR,
    ZMK_BLE_ADV_ADD,
    ZMK_BLE_ADV_START_OPEN,
    ZMK_BLE_ADV_START_FILTERED,
    ZMK_BLE_ADV_START_DIRECTED_RPA,
    ZMK_BLE_ADV_START_DIRECTED,
    ZMK_BLE_ADV_HANDOFF_DISCONNECT,
};
/* Weak hook called after each advertising HCI step; err is the unchanged Zephyr result.
 * The default does nothing. */
void zmk_ble_advertising_observed(uint8_t stage, int err);

int zmk_ble_unpair_all(void);

int zmk_ble_set_device_name(char *name);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
int zmk_ble_put_peripheral_addr(const bt_addr_le_t *addr);
#endif /* IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) */
