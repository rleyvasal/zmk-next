/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <sys/types.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/bluetooth/gatt.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/event_manager.h>
#include <zmk/battery.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/split/central.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_PROXY_SWAP)
#include <zephyr/bluetooth/services/bas.h>
#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/usb.h>
#endif

/*
 * Mighty Mitts flip-flops 0x2A19 callbacks: first completion = left slot,
 * second = right slot. On this stack the extra BAS (this file) completes
 * first and Zephyr's primary BAS second, so:
 *   extra BAS  -> left half (central)
 *   primary BAS -> right half (peripheral)
 * USB charge voltage is not a cell reading; freeze the left slot then.
 */

static uint8_t last_left_sent;
static bool have_left_sent;
static uint8_t last_right;
static bool have_right;
static int64_t last_left_ms;
static int64_t last_right_ms;

/* Not the live reading: on USB that is charge voltage (see zmk_battery_reported_soc()). */
static uint8_t left_level_for_host(void) { return zmk_battery_reported_soc(); }

static bool should_publish(bool have, uint8_t last, uint8_t now, int64_t last_ms) {
    int64_t t = k_uptime_get();

    if (have && last == now) {
        return false;
    }
    if (have && (t - last_ms) < 1000) {
        return false;
    }
    return true;
}
#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_PROXY_SWAP)

static void blvl_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    ARG_UNUSED(attr);

    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);

    LOG_INF("BAS Notifications %s", notif_enabled ? "enabled" : "disabled");
}

static ssize_t read_blvl(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                         uint16_t len, uint16_t offset) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_PROXY_SWAP)
    uint8_t level = left_level_for_host();
#else
    const uint8_t source = *(uint8_t *)attr->user_data;
    uint8_t level = 0;
    int rc = zmk_split_central_get_peripheral_battery_level(source, &level);

    if (rc == -EINVAL) {
        LOG_ERR("Invalid peripheral index requested for battery level read: %d", source);
        return 0;
    }
#endif

    return bt_gatt_attr_read(conn, attr, buf, len, offset, &level, sizeof(uint8_t));
}

static const struct bt_gatt_cpf aux_level_cpf = {
    .format = 0x04, // uint8
    .exponent = 0x0,
    .unit = 0x27AD,     // Percentage
    .name_space = 0x01, // Bluetooth SIG
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_PROXY_SWAP)
    .description = 0x0106, // "main": the left half for Mighty Mitts
#else
    .description = 0x0108, // "auxiliary"
#endif
};

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_PROXY_SWAP)
#define PERIPH_CUD_(x) "Left"
#else
#define PERIPH_CUD_(x) "Peripheral " #x
#endif
#define PERIPH_CUD(x) PERIPH_CUD_(x)

// How many GATT attributes each battery level adds to our service
#define PERIPH_BATT_LEVEL_ATTR_COUNT 5
// The second generated attribute is the one used to send GATT notifications
#define PERIPH_BATT_LEVEL_ATTR_NOTIFY_IDX 1

#define PERIPH_BATT_LEVEL_ATTRS(i, _)                                                              \
    BT_GATT_CHARACTERISTIC(BT_UUID_BAS_BATTERY_LEVEL, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,     \
                           BT_GATT_PERM_READ, read_blvl, NULL, ((uint8_t[]){i})),                  \
        BT_GATT_CCC(blvl_ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),                 \
        BT_GATT_CPF(&aux_level_cpf), BT_GATT_CUD(PERIPH_CUD(i), BT_GATT_PERM_READ),

BT_GATT_SERVICE_DEFINE(bas_aux, BT_GATT_PRIMARY_SERVICE(BT_UUID_BAS),
                       LISTIFY(CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS, PERIPH_BATT_LEVEL_ATTRS,
                               ()));

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_PROXY_SWAP)
static void notify_left(uint8_t level) {
    int index = PERIPH_BATT_LEVEL_ATTR_NOTIFY_IDX;
    int rc = bt_gatt_notify(NULL, &bas_aux.attrs[index], &level, sizeof(uint8_t));

    if (rc < 0 && rc != -ENOTCONN) {
        LOG_WRN("Failed to notify hosts of left battery level: %d", rc);
        return;
    }
    last_left_sent = level;
    have_left_sent = true;
    last_left_ms = k_uptime_get();
}

int peripheral_batt_lvl_listener(const zmk_event_t *eh) {
#if IS_ENABLED(CONFIG_ZMK_USB)
    if (as_zmk_usb_conn_state_changed(eh) != NULL) {
        notify_left(left_level_for_host());
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_BATTERY_CHARGE_STATUS)
    /* The primary BAS carries the right half, so its Battery Level Status
     * (0x2BED, next to that 2A19) carries the right half's charge state. */
    const struct zmk_peripheral_battery_charging_changed *charging_ev =
        as_zmk_peripheral_battery_charging_changed(eh);

    if (charging_ev != NULL) {
        LOG_DBG("Right charging for host BAS: %d full %d", charging_ev->charging,
                charging_ev->full);
        bt_bas_bls_set_battery_charge_state(
            charging_ev->full       ? BT_BAS_BLS_CHARGE_STATE_DISCHARGING_INACTIVE
            : charging_ev->charging ? BT_BAS_BLS_CHARGE_STATE_CHARGING
                                    : BT_BAS_BLS_CHARGE_STATE_DISCHARGING_ACTIVE);
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif

    const struct zmk_peripheral_battery_state_changed *ev =
        as_zmk_peripheral_battery_state_changed(eh);

    if (ev != NULL) {
        if (ev->source >= CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS) {
            LOG_WRN("Got battery level event for an out of range peripheral index");
            return ZMK_EV_EVENT_BUBBLE;
        }
        if (!should_publish(have_right, last_right, ev->state_of_charge, last_right_ms)) {
            return ZMK_EV_EVENT_BUBBLE;
        }
        LOG_DBG("Right battery level for host BAS: %u", ev->state_of_charge);
        if (bt_bas_set_battery_level(ev->state_of_charge) == 0) {
            last_right = ev->state_of_charge;
            have_right = true;
            last_right_ms = k_uptime_get();
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (as_zmk_battery_state_changed(eh) != NULL) {
        uint8_t lvl = left_level_for_host();

        if (!should_publish(have_left_sent, last_left_sent, lvl, last_left_ms)) {
            return ZMK_EV_EVENT_BUBBLE;
        }
        notify_left(lvl);
    }

    return ZMK_EV_EVENT_BUBBLE;
};
#else
int peripheral_batt_lvl_listener(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev =
        as_zmk_peripheral_battery_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    };

    if (ev->source >= CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS) {
        LOG_WRN("Got battery level event for an out of range peripheral index");
        return ZMK_EV_EVENT_BUBBLE;
    }

    LOG_DBG("Peripheral battery level event: %u", ev->state_of_charge);

    // Offset by the index of the source plus the specific offset to find the attribute to notify
    // on.
    int index = (PERIPH_BATT_LEVEL_ATTR_COUNT * ev->source) + PERIPH_BATT_LEVEL_ATTR_NOTIFY_IDX;

    int rc = bt_gatt_notify(NULL, &bas_aux.attrs[index], &ev->state_of_charge, sizeof(uint8_t));
    if (rc < 0 && rc != -ENOTCONN) {
        LOG_WRN("Failed to notify hosts of peripheral battery level: %d", rc);
    }

    return ZMK_EV_EVENT_BUBBLE;
};
#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_PROXY_SWAP)

ZMK_LISTENER(peripheral_batt_lvl_listener, peripheral_batt_lvl_listener);
ZMK_SUBSCRIPTION(peripheral_batt_lvl_listener, zmk_peripheral_battery_state_changed);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_PROXY_SWAP)
ZMK_SUBSCRIPTION(peripheral_batt_lvl_listener, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_ZMK_BATTERY_CHARGE_STATUS)
ZMK_SUBSCRIPTION(peripheral_batt_lvl_listener, zmk_peripheral_battery_charging_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_USB)
ZMK_SUBSCRIPTION(peripheral_batt_lvl_listener, zmk_usb_conn_state_changed);
#endif
#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_PROXY_SWAP)
