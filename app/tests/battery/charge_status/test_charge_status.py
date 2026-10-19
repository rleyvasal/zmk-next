"""A half on USB keeps its last on-battery level and reports charging that the central decodes."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


HARNESS = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define IS_ENABLED(x) x
#define CONFIG_ZMK_USB 0
#define CONFIG_BT_BAS 1
#define CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_PROXY 0
#define CONFIG_ZMK_BATTERY_CHARGE_STATUS 1
#define CONFIG_ZMK_BATTERY_HOLD_ON_EXTERNAL_POWER 1
#define CONFIG_ZMK_BATTERY_REPORTING_FETCH_MODE_STATE_OF_CHARGE 1
#define CONFIG_ZMK_BATTERY_REPORTING_FETCH_MODE_LITHIUM_VOLTAGE 0
#define OWN_BAS 1
#define NRF_POWER_HAS_USBREG 1
#define LOG_DBG(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define CHARGE_CURRENT_UA 0
#define BATTERY_CAPACITY_UAH 0
#define SETTLE_AFTER_CHARGE_MS 0
enum zmk_battery_charge_state {
    ZMK_BATTERY_CHARGE_STATE_DISCHARGING,
    ZMK_BATTERY_CHARGE_STATE_CHARGING,
    ZMK_BATTERY_CHARGE_STATE_FULL,
};
static int64_t now_ms;
static int64_t k_uptime_get(void) { return now_ms; }
/* A charger status pin (asserted while charging), as on a XIAO. */
#define HAS_CHARGER_STATUS 1
struct gpio_dt_spec { int unused; };
static const struct gpio_dt_spec charger_status;
static bool charger_status_ready = true;
static int charger_pin = 1;
static int gpio_pin_get_dt(const struct gpio_dt_spec *spec) { (void)spec; return charger_pin; }
typedef int NRF_POWER_Type;
static NRF_POWER_Type power_regs;
#define NRF_POWER (&power_regs)
static bool vbusdet;
static bool nrf_power_usbregstatus_vbusdet_get(NRF_POWER_Type const *p_reg) {
    assert(p_reg == NRF_POWER);
    return vbusdet;
}
struct device { int unused; };
struct sensor_value { int val1, val2; };
enum { SENSOR_CHAN_GAUGE_STATE_OF_CHARGE, SENSOR_CHAN_GAUGE_VOLTAGE };
static int sampled_soc;
static uint16_t last_millivolts;
static int sensor_sample_fetch_chan(const struct device *dev, int chan) {
    (void)dev; assert(chan == SENSOR_CHAN_GAUGE_STATE_OF_CHARGE); return 0;
}
/* A gauge with no voltage channel: battery.c then leaves last_millivolts at 0. */
static int sensor_channel_get(const struct device *dev, int chan, struct sensor_value *val) {
    (void)dev;
    if (chan == SENSOR_CHAN_GAUGE_VOLTAGE) {
        return -ENOTSUP;
    }
    assert(chan == SENSOR_CHAN_GAUGE_STATE_OF_CHARGE);
    val->val1 = sampled_soc; val->val2 = 0; return 0;
}
struct zmk_battery_state_changed { uint8_t state_of_charge; };
static int raise_zmk_battery_state_changed(struct zmk_battery_state_changed ev) { (void)ev; return 0; }
static uint8_t bas_level = 100; /* Zephyr's BAS starts at 100. */
static int bas_sets;
static uint8_t bt_bas_get_battery_level(void) { return bas_level; }
static int bt_bas_set_battery_level(uint8_t level) { bas_level = level; bas_sets++; return 0; }
enum bt_bas_bls_battery_charge_state {
    BT_BAS_BLS_CHARGE_STATE_UNKNOWN = 0,
    BT_BAS_BLS_CHARGE_STATE_CHARGING = 1,
    BT_BAS_BLS_CHARGE_STATE_DISCHARGING_ACTIVE = 2,
    BT_BAS_BLS_CHARGE_STATE_DISCHARGING_INACTIVE = 3,
};
/* What Zephyr's bas_bls.c sends: flags, then Power State (LE) with the charge
 * state in bits 5-6. Other power state bits are left set to prove they are masked. */
static uint8_t bls_value[3] = {0x00, 0x03, 0x0E};
static int bls_sets;
static void bt_bas_bls_set_battery_charge_state(enum bt_bas_bls_battery_charge_state state) {
    uint16_t ps = bls_value[1] | (bls_value[2] << 8);
    ps = (ps & ~(0x3 << 5)) | ((state << 5) & (0x3 << 5));
    bls_value[1] = ps & 0xff; bls_value[2] = ps >> 8;
    bls_sets++;
}
static uint8_t last_state_of_charge;
static uint8_t last_cell_soc;
static bool have_last_cell;
/* ACTUAL_REMEMBER */
/* ACTUAL_OWN_BAS */
/* ACTUAL_UPDATE */
/* ACTUAL_PARSER */
static const struct device battery_dev;
static void sample(int soc, bool vbus) {
    now_ms += 60000;
    sampled_soc = soc; vbusdet = vbus;
    assert(zmk_battery_update(&battery_dev) == 0);
}
static int central_sees_charging(void) {
    return batt_status_charge_state(bls_value, sizeof(bls_value));
}
int main(void) {
    /* Malformed or short status values are rejected. */
    assert(batt_status_charge_state(NULL, 3) == -EINVAL);
    assert(batt_status_charge_state(bls_value, 2) == -EINVAL);
    (void)zmk_battery_charge_state;
    (void)needs_samples;
    assert(central_sees_charging() == 0); /* Charge state unknown before the first sample. */

    /* On battery: the live level is published and remembered; discharging. */
    sample(64, false);
    assert(bas_level == 64 && have_last_cell && last_cell_soc == 64);
    assert(bls_sets == 1 && central_sees_charging() == 0);
    assert(((bls_value[1] >> 5) & 3) == BT_BAS_BLS_CHARGE_STATE_DISCHARGING_ACTIVE);

    /* Plugged in: charge voltage reads 100%, but the BAS keeps the cell level. */
    sample(100, true);
    assert(bas_level == 64 && last_cell_soc == 64);
    assert(bls_sets == 2 && central_sees_charging() == 1);
    sample(100, true);
    assert(bls_sets == 2); /* Unchanged state: no new notification. */
    assert(bas_level == 64);

    /* The charger reports the charge complete: 100, "discharging, inactive",
     * which the central decodes as full. */
    charger_pin = 0;
    sample(100, true);
    assert(bas_level == 100 && last_cell_soc == 100);
    assert(((bls_value[1] >> 5) & 3) == BT_BAS_BLS_CHARGE_STATE_DISCHARGING_INACTIVE);
    assert(bls_sets == 3 && central_sees_charging() == ZMK_BATTERY_CHARGE_STATE_FULL);
    charger_pin = 1;

    /* Unplugged: live readings again, discharging. */
    sample(61, false);
    assert(bas_level == 61 && last_cell_soc == 61);
    assert(bls_sets == 4 && central_sees_charging() == 0);
    assert((bls_value[1] & 0x1f) == 0x03 && bls_value[2] == 0x0E); /* Other bits untouched. */

    /* Booted on USB with no cell level known: the live reading is all there is. */
    have_last_cell = false; last_state_of_charge = 0; prev_vbus = false;
    sample(99, true);
    assert(bas_level == 99 && !have_last_cell && central_sees_charging() == 1);
    return 0;
}
"""


def _section(source, start, end, include_end=False):
    i = source.index(start)
    j = source.index(end, i)
    return source[i:j + len(end)] if include_end else source[i:j]


class ChargeStatusTests(unittest.TestCase):
    def test_vbus_freezes_level_and_central_decodes_charging(self):
        app = Path(__file__).resolve().parents[3]
        battery = (app / "src/battery.c").read_text()
        central = (app / "src/split/bluetooth/central.c").read_text()
        harness = HARNESS.replace(
            "/* ACTUAL_REMEMBER */",
            _section(battery, "static bool vbus_present(",
                     "\n#if IS_ENABLED(CONFIG_SETTINGS)\nstatic void persist_cell_soc("))
        harness = harness.replace(
            "/* ACTUAL_OWN_BAS */",
            _section(battery, "#if OWN_BAS && IS_ENABLED(CONFIG_ZMK_BATTERY_CHARGE_STATUS)\n",
                     "\n#if DT_HAS_CHOSEN(zmk_battery)\n"))
        harness = harness.replace(
            "/* ACTUAL_UPDATE */",
            _section(battery, "static int zmk_battery_update(",
                     "\nstatic void stop_sampling_if_idle(void);"))
        harness = harness.replace(
            "/* ACTUAL_PARSER */",
            _section(central, "static int batt_status_charge_state(",
                     "\nstatic void report_batt_status("))
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "test_charge_status")
            subprocess.run(
                [os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                 "-x", "c", "-", "-o", executable],
                input=harness, text=True, check=True, timeout=30,
            )
            subprocess.run([executable], check=True, timeout=3)


if __name__ == "__main__":
    unittest.main()
