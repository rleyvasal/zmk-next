/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/bluetooth/services/bas.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/event_manager.h>
#include <zmk/battery.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/activity_state_changed.h>
#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/usb.h>
#endif
#include <zmk/activity.h>
#include <zmk/workqueue.h>
#if IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#endif

static uint8_t last_state_of_charge = 0;
static uint8_t last_cell_soc;
static bool have_last_cell;
static uint16_t last_millivolts;

uint8_t zmk_battery_state_of_charge(void) { return last_state_of_charge; }

int zmk_battery_last_cell_soc(void) { return have_last_cell ? (int)last_cell_soc : -1; }

uint16_t zmk_battery_millivolts(void) { return last_millivolts; }

static void remember_cell_soc(uint8_t soc) {
#if IS_ENABLED(CONFIG_ZMK_USB)
    if (zmk_usb_is_powered()) {
        return;
    }
#endif
    last_cell_soc = soc;
    have_last_cell = true;
}

#if IS_ENABLED(CONFIG_SETTINGS)
static void persist_cell_soc(void) {
    if (!have_last_cell) {
        return;
    }
    int rc = settings_save_one("zmk/batt/cell", &last_cell_soc, sizeof(last_cell_soc));
    if (rc != 0) {
        LOG_WRN("Failed to persist last cell SoC (%d)", rc);
    }
}

static int batt_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;

    if (settings_name_steq(name, "cell", &next) && !next) {
        if (len != sizeof(last_cell_soc)) {
            return -EINVAL;
        }
        int rc = read_cb(cb_arg, &last_cell_soc, sizeof(last_cell_soc));
        if (rc >= 0) {
            have_last_cell = true;
        }
        return MIN(rc, 0);
    }
    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(zmk_batt, "zmk/batt", NULL, batt_settings_set, NULL, NULL);
#endif

#if DT_HAS_CHOSEN(zmk_battery)
static const struct device *const battery = DEVICE_DT_GET(DT_CHOSEN(zmk_battery));
#else
#warning                                                                                           \
    "Using a node labeled BATTERY for the battery sensor is deprecated. Set a zmk,battery chosen node instead. (Ignore this if you don't have a battery sensor.)"
static const struct device *battery;
#endif

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING_FETCH_MODE_LITHIUM_VOLTAGE)
static uint8_t lithium_ion_mv_to_pct(int16_t bat_mv) {
    // Simple linear approximation of a battery based off adafruit's discharge graph:
    // https://learn.adafruit.com/li-ion-and-lipoly-batteries/voltages

    if (bat_mv >= 4200) {
        return 100;
    } else if (bat_mv <= 3450) {
        return 0;
    }

    return bat_mv * 2 / 15 - 459;
}

#endif // IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING_FETCH_MODE_LITHIUM_VOLTAGE)

static int zmk_battery_update(const struct device *battery) {
    struct sensor_value state_of_charge;
    int rc;

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING_FETCH_MODE_STATE_OF_CHARGE)

    rc = sensor_sample_fetch_chan(battery, SENSOR_CHAN_GAUGE_STATE_OF_CHARGE);
    if (rc != 0) {
        LOG_DBG("Failed to fetch battery values: %d", rc);
        return rc;
    }

    rc = sensor_channel_get(battery, SENSOR_CHAN_GAUGE_STATE_OF_CHARGE, &state_of_charge);

    if (rc != 0) {
        LOG_DBG("Failed to get battery state of charge: %d", rc);
        return rc;
    }
#elif IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING_FETCH_MODE_LITHIUM_VOLTAGE)
    rc = sensor_sample_fetch_chan(battery, SENSOR_CHAN_VOLTAGE);
    if (rc != 0) {
        LOG_DBG("Failed to fetch battery values: %d", rc);
        return rc;
    }

    struct sensor_value voltage;
    rc = sensor_channel_get(battery, SENSOR_CHAN_VOLTAGE, &voltage);

    if (rc != 0) {
        LOG_DBG("Failed to get battery voltage: %d", rc);
        return rc;
    }

    uint16_t mv = voltage.val1 * 1000 + (voltage.val2 / 1000);
    last_millivolts = mv;
    state_of_charge.val1 = lithium_ion_mv_to_pct(mv);

    LOG_DBG("State of change %d from %d mv", state_of_charge.val1, mv);
#else
#error "Not a supported reporting fetch mode"
#endif

    if (last_state_of_charge != state_of_charge.val1) {
        last_state_of_charge = state_of_charge.val1;

        rc = raise_zmk_battery_state_changed(
            (struct zmk_battery_state_changed){.state_of_charge = last_state_of_charge});

        if (rc != 0) {
            LOG_ERR("Failed to raise battery state changed event: %d", rc);
            return rc;
        }
    }
    remember_cell_soc(last_state_of_charge);

#if IS_ENABLED(CONFIG_BT_BAS) && !IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_PROXY)
#if IS_ENABLED(CONFIG_ZMK_USB)
    /* USB holds the cell at charge voltage; lithium_ion_mv_to_pct maps
     * >= 4200 mV to 100%. Do not publish that as the host-facing SoC. */
    if (zmk_usb_is_powered()) {
        /* leave BAS at the last on-battery sample */
    } else
#endif
        if (bt_bas_get_battery_level() != last_state_of_charge) {
        LOG_DBG("Setting BAS GATT battery level to %d.", last_state_of_charge);

        rc = bt_bas_set_battery_level(last_state_of_charge);

        if (rc != 0) {
            LOG_WRN("Failed to set BAS GATT battery level (err %d)", rc);
            return rc;
        }
    }
#endif

    return rc;
}

static void zmk_battery_work(struct k_work *work) {
    int rc = zmk_battery_update(battery);

    if (rc != 0) {
        LOG_DBG("Failed to update battery value: %d.", rc);
    }
}

K_WORK_DEFINE(battery_work, zmk_battery_work);

static void zmk_battery_timer(struct k_timer *timer) {
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &battery_work);
}

K_TIMER_DEFINE(battery_timer, zmk_battery_timer, NULL);

static void zmk_battery_start_reporting() {
    if (device_is_ready(battery)) {
        k_timer_start(&battery_timer, K_NO_WAIT, K_SECONDS(CONFIG_ZMK_BATTERY_REPORT_INTERVAL));
    }
}

static int zmk_battery_init(void) {
#if !DT_HAS_CHOSEN(zmk_battery)
    battery = device_get_binding("BATTERY");

    if (battery == NULL) {
        return -ENODEV;
    }

    LOG_WRN("Finding battery device labeled BATTERY is deprecated. Use zmk,battery chosen node.");
#endif

    if (!device_is_ready(battery)) {
        LOG_ERR("Battery device \"%s\" is not ready", battery->name);
        return -ENODEV;
    }

    zmk_battery_start_reporting();
    return 0;
}

static int battery_event_listener(const zmk_event_t *eh) {
#if IS_ENABLED(CONFIG_ZMK_USB)
    if (as_zmk_usb_conn_state_changed(eh)) {
        if (!zmk_usb_is_powered()) {
            k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &battery_work);
        } else {
#if IS_ENABLED(CONFIG_SETTINGS)
            persist_cell_soc();
#endif
        }
        return 0;
    }
#endif

    if (as_zmk_activity_state_changed(eh)) {
        switch (zmk_activity_get_state()) {
        case ZMK_ACTIVITY_ACTIVE:
            zmk_battery_start_reporting();
            return 0;
        case ZMK_ACTIVITY_IDLE:
        case ZMK_ACTIVITY_SLEEP:
            k_timer_stop(&battery_timer);
            return 0;
        default:
            break;
        }
    }
    return -ENOTSUP;
}

ZMK_LISTENER(battery, battery_event_listener);

ZMK_SUBSCRIPTION(battery, zmk_activity_state_changed);
#if IS_ENABLED(CONFIG_ZMK_USB)
ZMK_SUBSCRIPTION(battery, zmk_usb_conn_state_changed);
#endif

SYS_INIT(zmk_battery_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
