"""The cell level while charging: estimated rise, full from the charger, and settling after unplug."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define IS_ENABLED(x) x
#define CONFIG_ZMK_USB 0
#define NRF_POWER_HAS_USBREG 1
#define MIN(a, b) ((a) < (b) ? (a) : (b))
/* Totem: XIAO charging at 50 mA into a 110 mAh cell. */
#define CHARGE_CURRENT_UA 50000
#define BATTERY_CAPACITY_UAH 110000
enum zmk_battery_charge_state {
    ZMK_BATTERY_CHARGE_STATE_DISCHARGING,
    ZMK_BATTERY_CHARGE_STATE_CHARGING,
    ZMK_BATTERY_CHARGE_STATE_FULL,
};
typedef int NRF_POWER_Type;
static NRF_POWER_Type power_regs;
#define NRF_POWER (&power_regs)
static bool vbus;
static bool nrf_power_usbregstatus_vbusdet_get(NRF_POWER_Type const *p_reg) {
    (void)p_reg;
    return vbus;
}
static int64_t now_ms;
static int64_t k_uptime_get(void) { return now_ms; }
static uint8_t last_state_of_charge;
static uint8_t last_cell_soc;
static bool have_last_cell;
/* ACTUAL_TRACKING */
#define MIN_ (60 * 1000)
static void sample(int64_t at, uint8_t live, bool on_power, int charger) {
    now_ms = at;
    vbus = on_power;
    last_state_of_charge = live;
    track_cell_level(live, vbus_present(), charger, now_ms);
}
static void reset(void) {
    have_last_cell = false;
    last_cell_soc = 0;
    prev_vbus = false;
    unplugged_seen = false;
    charge_start_soc = -1;
    charged_since_plugged = false;
    charge_state = ZMK_BATTERY_CHARGE_STATE_DISCHARGING;
}
#define EXPECT(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "line %d: %s (cell %d have %d reported %d state %d)\n", __LINE__,     \
                    #cond, last_cell_soc, have_last_cell, zmk_battery_reported_soc(),             \
                    charge_state);                                                                 \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
int main(void) {
    EXPECT(charge_estimate(40, 60 * MIN_, 50000, 0) == -1);
    EXPECT(charge_estimate(-1, 60 * MIN_, 50000, 110000) == -1);
    EXPECT(charge_estimate(40, 60 * MIN_, 50000, 110000) == 85); /* 50 mAh of 110 */
    EXPECT(charge_estimate(40, 600 * MIN_, 50000, 110000) == 99);

    /* On battery: the live reading is the cell level. */
    reset();
    sample(0, 40, false, 1);
    EXPECT(last_cell_soc == 40 && charge_state == ZMK_BATTERY_CHARGE_STATE_DISCHARGING);
    EXPECT(zmk_battery_reported_soc() == 40 && !needs_samples());

    /* Plugged in: charge voltage (100) is not the cell; the level rises by estimate. */
    sample(1 * MIN_, 100, true, 1);
    EXPECT(charge_state == ZMK_BATTERY_CHARGE_STATE_CHARGING);
    EXPECT(last_cell_soc == 40 && zmk_battery_reported_soc() == 40 && needs_samples());
    sample(61 * MIN_, 100, true, 1);
    EXPECT(last_cell_soc == 85 && zmk_battery_reported_soc() == 85);
    sample(181 * MIN_, 100, true, 1);
    EXPECT(last_cell_soc == 99); /* Not 100 until the charger says so. */

    /* The charger finishes, then tops off again: stays 100. */
    sample(190 * MIN_, 100, true, 0);
    EXPECT(charge_state == ZMK_BATTERY_CHARGE_STATE_FULL && last_cell_soc == 100);
    sample(250 * MIN_, 100, true, 1);
    EXPECT(charge_state == ZMK_BATTERY_CHARGE_STATE_CHARGING && last_cell_soc == 100);

    /* Unplugged: readings are still high (99, 97) while the cell settles. */
    sample(260 * MIN_, 99, false, -1);
    EXPECT(charge_state == ZMK_BATTERY_CHARGE_STATE_DISCHARGING);
    if (SETTLE_AFTER_CHARGE_MS > 0) {
        EXPECT(last_cell_soc == 100 && zmk_battery_reported_soc() == 100 && needs_samples());
        sample(264 * MIN_, 97, false, -1);
        EXPECT(last_cell_soc == 100 && zmk_battery_reported_soc() == 100);
        sample(266 * MIN_, 95, false, -1); /* 6 min after unplug: settled. */
        EXPECT(last_cell_soc == 95 && zmk_battery_reported_soc() == 95 && !needs_samples());
    } else {
        EXPECT(last_cell_soc == 99 && zmk_battery_reported_soc() == 99 && !needs_samples());
    }

    /* Plugged in at 93% (4.145 V, above the charger's ~4.1 V recharge
     * threshold): the charger never starts. Not charging, but not 100 either. */
    reset();
    sample(0, 93, false, 1);
    sample(1 * MIN_, 100, true, 0);
    EXPECT(charge_state == ZMK_BATTERY_CHARGE_STATE_FULL && last_cell_soc == 93);
    EXPECT(zmk_battery_reported_soc() == 93);
    sample(30 * MIN_, 100, true, 0);
    EXPECT(last_cell_soc == 93); /* No estimate while not charging. */
    sample(40 * MIN_, 100, true, 1); /* Drained to the threshold: it charges... */
    sample(70 * MIN_, 100, true, 0); /* ...and finishes. */
    EXPECT(charge_state == ZMK_BATTERY_CHARGE_STATE_FULL && last_cell_soc == 100);

    /* No charger status pin (-1): charging is assumed, never full. */
    reset();
    sample(0, 50, false, -1);
    sample(1 * MIN_, 100, true, -1);
    sample(31 * MIN_, 100, true, -1);
    EXPECT(charge_state == ZMK_BATTERY_CHARGE_STATE_CHARGING && last_cell_soc == 72);

    /* Boot on USB: the persisted level loads after the first sample. */
    reset();
    sample(0, 100, true, 1);
    EXPECT(!have_last_cell && zmk_battery_reported_soc() == 100);
    have_last_cell = true; /* settings_load() */
    last_cell_soc = 60;
    sample(1 * MIN_, 100, true, 1);
    EXPECT(last_cell_soc == 60);
    sample(31 * MIN_, 100, true, 1);
    EXPECT(last_cell_soc == 82);
    return 0;
}
"""


class ChargeTrackingTests(unittest.TestCase):
    def run_harness(self, settle_ms):
        source = (Path(__file__).resolve().parents[3] / "src/battery.c").read_text()
        start = source.index("static bool vbus_present(")
        end = source.index("\n#if IS_ENABLED(CONFIG_SETTINGS)\nstatic void persist_cell_soc(", start)
        harness = HARNESS.replace("/* ACTUAL_TRACKING */", source[start:end])
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "test_charge_tracking")
            subprocess.run(
                [os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                 f"-DSETTLE_AFTER_CHARGE_MS={settle_ms}", "-x", "c", "-", "-o", executable],
                input=harness, text=True, check=True, timeout=30,
            )
            subprocess.run([executable], check=True, timeout=3)

    def test_charge_full_and_settle(self):
        self.run_harness("((int64_t)300 * 1000)")

    def test_without_settling(self):
        self.run_harness("0")


if __name__ == "__main__":
    unittest.main()
