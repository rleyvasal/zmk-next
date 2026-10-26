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
#include <zephyr/drivers/gpio.h>
#include <zephyr/bluetooth/services/bas.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/event_manager.h>
#include <zmk/battery.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/activity_state_changed.h>
#if IS_ENABLED(CONFIG_ZMK_USB) && IS_ENABLED(CONFIG_ZMK_BATTERY_HOLD_ON_EXTERNAL_POWER)
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/usb.h>
#endif
#include <zmk/activity.h>
#include <zmk/workqueue.h>
#if IS_ENABLED(CONFIG_ZMK_BATTERY_HOLD_ON_EXTERNAL_POWER)
#if IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#endif
#if IS_ENABLED(CONFIG_HAS_NRFX)
#include <hal/nrf_power.h>
#endif
#endif

/* This half publishes its own level in the primary BAS. With the swapped proxy layout, a split
 * central publishes it in the extra BAS instead (central_bas_proxy.c). */
#define OWN_BAS                                                                                    \
    (IS_ENABLED(CONFIG_BT_BAS) &&                                                                  \
     !IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_PROXY_SWAP))

/* Halves without ZMK_USB get no USB events, so poll VBUS detect for the charge state. */
#if defined(NRF_POWER_HAS_USBREG) && NRF_POWER_HAS_USBREG && !IS_ENABLED(CONFIG_ZMK_USB) &&        \
    OWN_BAS && IS_ENABLED(CONFIG_ZMK_BATTERY_CHARGE_STATUS) &&                                     \
    IS_ENABLED(CONFIG_ZMK_BATTERY_HOLD_ON_EXTERNAL_POWER)
#define POLL_VBUS 1
#else
#define POLL_VBUS 0
#endif

#if DT_HAS_CHOSEN(zmk_battery_charger) && IS_ENABLED(CONFIG_ZMK_BATTERY_HOLD_ON_EXTERNAL_POWER)
#define CHARGER_NODE DT_CHOSEN(zmk_battery_charger)
#define CHARGE_CURRENT_UA DT_PROP_OR(CHARGER_NODE, charge_current_microamp, 0)
#define BATTERY_CAPACITY_UAH DT_PROP_OR(CHARGER_NODE, charge_full_design_microamp_hours, 0)
#if DT_NODE_HAS_PROP(CHARGER_NODE, status_gpios)
#define HAS_CHARGER_STATUS 1
static const struct gpio_dt_spec charger_status = GPIO_DT_SPEC_GET(CHARGER_NODE, status_gpios);
static bool charger_status_ready;
#endif
#else
#define CHARGE_CURRENT_UA 0
#define BATTERY_CAPACITY_UAH 0
#endif

#define SETTLE_AFTER_CHARGE_MS ((int64_t)CONFIG_ZMK_BATTERY_SETTLE_AFTER_CHARGE_SEC * 1000)

static uint8_t last_state_of_charge = 0;
static uint16_t last_millivolts;

uint8_t zmk_battery_state_of_charge(void) { return last_state_of_charge; }

uint16_t zmk_battery_millivolts(void) { return last_millivolts; }

#if IS_ENABLED(CONFIG_ZMK_BATTERY_HOLD_ON_EXTERNAL_POWER)
static uint8_t last_cell_soc;
static bool have_last_cell;

int zmk_battery_last_cell_soc(void) { return have_last_cell ? (int)last_cell_soc : -1; }

/* zmk_usb_is_powered() reads false until the USB stack reports its first
 * status, which can come after the first battery sample at boot. Where the
 * SoC has a USB regulator, read VBUS detect directly so a sample taken on USB
 * is never mistaken for the cell level. */
static bool vbus_present(void) {
#if NRF_POWER_HAS_USBREG
    if (nrf_power_usbregstatus_vbusdet_get(NRF_POWER)) {
        return true;
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_USB)
    return zmk_usb_is_powered();
#else
    return false;
#endif
}

static enum zmk_battery_charge_state charge_state;
static bool prev_vbus;
static bool unplugged_seen;
static int64_t unplugged_at;
static int64_t charge_start_at;
static int charge_start_soc = -1;
static bool charged_since_plugged;

/* Level after charging for elapsed_ms from start_soc at current_ua into a
 * capacity_uah cell, or -1 if any of those is unknown. Held below 100 until
 * the charger reports full: the current tapers off near the end, so the
 * estimate runs ahead of the cell there. */
static int charge_estimate(int start_soc, int64_t elapsed_ms, uint32_t current_ua,
                           uint32_t capacity_uah) {
    if (start_soc < 0 || current_ua == 0 || capacity_uah == 0 || elapsed_ms < 0) {
        return -1;
    }
    /* percent = current * hours / capacity * 100 = current * ms / (capacity * 36000) */
    int64_t gained = elapsed_ms * current_ua / ((int64_t)capacity_uah * 36000);
    return (int)MIN(start_soc + gained, 99);
}

static bool settling(int64_t now) {
    return SETTLE_AFTER_CHARGE_MS > 0 && unplugged_seen &&
           now - unplugged_at < SETTLE_AFTER_CHARGE_MS;
}

/* Fold one sample into the cell level. charger: -1 without a status pin, 1
 * charging, 0 charge complete. On external power the live reading is charge
 * voltage, so it is never taken as the cell level, only as a ceiling on it. */
static void track_cell_level(uint8_t live_soc, bool vbus, int charger, int64_t now) {
    if (!vbus) {
        if (prev_vbus) {
            unplugged_seen = true;
            unplugged_at = now;
        }
        prev_vbus = false;
        charge_state = ZMK_BATTERY_CHARGE_STATE_DISCHARGING;
        if (!settling(now)) {
            last_cell_soc = live_soc;
            have_last_cell = true;
        }
        return;
    }

    if (!prev_vbus) {
        charged_since_plugged = false;
    }
    if (!prev_vbus || (charge_start_soc < 0 && have_last_cell)) {
        /* Plugged in, or the persisted level arrived after the first sample. */
        charge_start_at = now;
        charge_start_soc = have_last_cell ? last_cell_soc : -1;
    }
    prev_vbus = true;

    /* On external power the reading is at or above the cell's own voltage, so
     * it bounds the cell level from above. A remembered level over it is stale:
     * on-battery levels are kept in RAM only, and a cell that drained until it
     * reset left the level from before. Restart the estimate from the reading. */
    if (charge_start_soc > live_soc) {
        charge_start_soc = live_soc;
        charge_start_at = now;
    }
    if (have_last_cell && last_cell_soc > live_soc) {
        last_cell_soc = live_soc;
    }

    if (charger == 0) {
        /* Not charging on external power. That is a finished charge only if
         * one ran since plugging in: chargers also stay idle when plugged in
         * above their recharge threshold (about 4.1 V, over 90%), and then the
         * last level is still the best one. */
        charge_state = ZMK_BATTERY_CHARGE_STATE_FULL;
        if (charged_since_plugged) {
            last_cell_soc = 100;
            have_last_cell = true;
        }
        return;
    }
    if (charger == 1) {
        charged_since_plugged = true;
    }
    charge_state = ZMK_BATTERY_CHARGE_STATE_CHARGING;

    int estimate = MIN(charge_estimate(charge_start_soc, now - charge_start_at, CHARGE_CURRENT_UA,
                                       BATTERY_CAPACITY_UAH),
                       live_soc);
    if (estimate > (have_last_cell ? last_cell_soc : -1)) {
        last_cell_soc = estimate;
        have_last_cell = true;
    }
}

uint8_t zmk_battery_reported_soc(void) {
    bool held = prev_vbus || settling(k_uptime_get());

    return (held && have_last_cell) ? last_cell_soc : last_state_of_charge;
}

enum zmk_battery_charge_state zmk_battery_charge_state(void) { return charge_state; }

/* Idle normally stops sampling, but the charge estimate, the charger status
 * and the first reading after settling all come from samples. */
static bool needs_samples(void) { return prev_vbus || settling(k_uptime_get()); }

#if IS_ENABLED(CONFIG_SETTINGS)
static void persist_cell_soc(struct k_work *work) {
    ARG_UNUSED(work);
    if (!have_last_cell) {
        return;
    }
    int rc = settings_save_one("zmk/batt/cell", &last_cell_soc, sizeof(last_cell_soc));
    if (rc != 0) {
        LOG_WRN("Failed to persist last cell SoC (%d)", rc);
    }
}

static K_WORK_DEFINE(persist_cell_soc_work, persist_cell_soc);

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

#else // !IS_ENABLED(CONFIG_ZMK_BATTERY_HOLD_ON_EXTERNAL_POWER)

/* Without the hold, the reported level is the live reading and nothing tracks charging. */
int zmk_battery_last_cell_soc(void) { return -1; }

uint8_t zmk_battery_reported_soc(void) { return last_state_of_charge; }

enum zmk_battery_charge_state zmk_battery_charge_state(void) {
    return ZMK_BATTERY_CHARGE_STATE_DISCHARGING;
}

static bool needs_samples(void) { return false; }

#endif // IS_ENABLED(CONFIG_ZMK_BATTERY_HOLD_ON_EXTERNAL_POWER)

#if OWN_BAS && IS_ENABLED(CONFIG_ZMK_BATTERY_CHARGE_STATUS)
static int published_charge_state = -1;

/* On external power and not charging is "discharging, inactive" in BAS terms. */
static void publish_charge_state(enum zmk_battery_charge_state state) {
    int bls;

    switch (state) {
    case ZMK_BATTERY_CHARGE_STATE_CHARGING:
        bls = BT_BAS_BLS_CHARGE_STATE_CHARGING;
        break;
    case ZMK_BATTERY_CHARGE_STATE_FULL:
        bls = BT_BAS_BLS_CHARGE_STATE_DISCHARGING_INACTIVE;
        break;
    default:
        bls = BT_BAS_BLS_CHARGE_STATE_DISCHARGING_ACTIVE;
        break;
    }

    /* Each set notifies and indicates, so only send changes. */
    if (bls != published_charge_state) {
        published_charge_state = bls;
        bt_bas_bls_set_battery_charge_state(bls);
    }
}
#endif // OWN_BAS && IS_ENABLED(CONFIG_ZMK_BATTERY_CHARGE_STATUS)

#if IS_ENABLED(CONFIG_ZMK_BATTERY_HOLD_ON_EXTERNAL_POWER)
/* 1 charging, 0 charge complete, -1 without a charger status pin. */
static int charger_charging(void) {
#if defined(HAS_CHARGER_STATUS)
    if (charger_status_ready) {
        int value = gpio_pin_get_dt(&charger_status);

        if (value >= 0) {
            return value;
        }
    }
#endif
    return -1;
}
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

    /* Gauges that also measure the voltage (ZMK's voltage-divider and nRF VDDH drivers) keep it
     * from the same fetch: record it for zmk_battery_millivolts(). Others leave it at 0. */
    struct sensor_value voltage;

    if (sensor_channel_get(battery, SENSOR_CHAN_GAUGE_VOLTAGE, &voltage) == 0) {
        last_millivolts = voltage.val1 * 1000 + (voltage.val2 / 1000);
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

    uint8_t old_reported = zmk_battery_reported_soc();
    enum zmk_battery_charge_state old_state = zmk_battery_charge_state();
    bool live_changed = last_state_of_charge != state_of_charge.val1;

    last_state_of_charge = state_of_charge.val1;
#if IS_ENABLED(CONFIG_ZMK_BATTERY_HOLD_ON_EXTERNAL_POWER)
    track_cell_level(last_state_of_charge, vbus_present(), charger_charging(), k_uptime_get());
#endif

    /* Listeners read zmk_battery_reported_soc() too, so tell them when it or the
     * charge state moves, not only when the live reading does. */
    if (live_changed || zmk_battery_reported_soc() != old_reported ||
        zmk_battery_charge_state() != old_state) {
        rc = raise_zmk_battery_state_changed(
            (struct zmk_battery_state_changed){.state_of_charge = last_state_of_charge});

        if (rc != 0) {
            LOG_ERR("Failed to raise battery state changed event: %d", rc);
            return rc;
        }
    }

#if OWN_BAS
    /* On external power the live reading is charge voltage (lithium_ion_mv_to_pct
     * maps >= 4200 mV to 100%), so publish the reported level instead. */
    uint8_t bas_level = zmk_battery_reported_soc();

#if IS_ENABLED(CONFIG_ZMK_BATTERY_CHARGE_STATUS)
    publish_charge_state(zmk_battery_charge_state());
#endif
    if (bt_bas_get_battery_level() != bas_level) {
        LOG_DBG("Setting BAS GATT battery level to %d.", bas_level);

        rc = bt_bas_set_battery_level(bas_level);

        if (rc != 0) {
            LOG_WRN("Failed to set BAS GATT battery level (err %d)", rc);
            return rc;
        }
    }
#endif

    return rc;
}

static void stop_sampling_if_idle(void);

static void zmk_battery_work(struct k_work *work) {
    int rc = zmk_battery_update(battery);

    if (rc != 0) {
        LOG_DBG("Failed to update battery value: %d.", rc);
    }
    stop_sampling_if_idle();
}

K_WORK_DEFINE(battery_work, zmk_battery_work);

static void zmk_battery_timer(struct k_timer *timer) {
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &battery_work);
}

K_TIMER_DEFINE(battery_timer, zmk_battery_timer, NULL);

static void stop_sampling_if_idle(void) {
    if (!needs_samples() && zmk_activity_get_state() != ZMK_ACTIVITY_ACTIVE) {
        k_timer_stop(&battery_timer);
    }
}

#if POLL_VBUS
#define VBUS_POLL_INTERVAL K_SECONDS(2)

static bool polled_vbus;

/* Runs in the timer ISR; the register read is cheap. On a change, take a
 * sample now so the BAS level freezes and the charge state follows within one
 * poll, not one battery report interval. */
static void vbus_poll(struct k_timer *timer) {
    bool vbus = nrf_power_usbregstatus_vbusdet_get(NRF_POWER);

    if (vbus == polled_vbus) {
        return;
    }
    polled_vbus = vbus;
    /* Sample now and keep sampling, even while idle (see needs_samples()). */
    k_timer_start(&battery_timer, K_NO_WAIT, K_SECONDS(CONFIG_ZMK_BATTERY_REPORT_INTERVAL));
#if IS_ENABLED(CONFIG_SETTINGS)
    if (vbus) {
        /* Keep the on-battery level across a reboot while plugged in. */
        k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &persist_cell_soc_work);
    }
#endif
}

K_TIMER_DEFINE(vbus_timer, vbus_poll, NULL);
#endif // POLL_VBUS

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

#if defined(HAS_CHARGER_STATUS)
    charger_status_ready = gpio_is_ready_dt(&charger_status) &&
                           gpio_pin_configure_dt(&charger_status, GPIO_INPUT) == 0;
    if (!charger_status_ready) {
        LOG_WRN("Charger status pin not available; charging is assumed while powered");
    }
#endif

    zmk_battery_start_reporting();
#if POLL_VBUS
    /* The first sample already sees the boot state; poll for later changes. */
    polled_vbus = nrf_power_usbregstatus_vbusdet_get(NRF_POWER);
    k_timer_start(&vbus_timer, VBUS_POLL_INTERVAL, VBUS_POLL_INTERVAL);
#endif
    return 0;
}

static int battery_event_listener(const zmk_event_t *eh) {
#if IS_ENABLED(CONFIG_ZMK_USB) && IS_ENABLED(CONFIG_ZMK_BATTERY_HOLD_ON_EXTERNAL_POWER)
    if (as_zmk_usb_conn_state_changed(eh)) {
#if IS_ENABLED(CONFIG_SETTINGS)
        if (zmk_usb_is_powered()) {
            /* USB events run on the system queue, which Bluetooth needs while
             * settings_load() owns the settings lock. Save on the battery queue
             * so waiting for that lock cannot stall HCI command processing. */
            k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &persist_cell_soc_work);
        }
#endif
        /* Sample now and keep sampling, even while idle (see needs_samples()). */
        zmk_battery_start_reporting();
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
            stop_sampling_if_idle();
            return 0;
        default:
            break;
        }
    }
    return -ENOTSUP;
}

ZMK_LISTENER(battery, battery_event_listener);

ZMK_SUBSCRIPTION(battery, zmk_activity_state_changed);
#if IS_ENABLED(CONFIG_ZMK_USB) && IS_ENABLED(CONFIG_ZMK_BATTERY_HOLD_ON_EXTERNAL_POWER)
ZMK_SUBSCRIPTION(battery, zmk_usb_conn_state_changed);
#endif

SYS_INIT(zmk_battery_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
