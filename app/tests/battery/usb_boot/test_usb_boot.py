"""Boot-time battery samples must not be remembered as the cell level while VBUS is present."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define IS_ENABLED(x) x
#define CONFIG_ZMK_USB 1
#define NRF_POWER_HAS_USBREG 1
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
static uint8_t last_state_of_charge;
typedef int NRF_POWER_Type;
static NRF_POWER_Type power_regs;
#define NRF_POWER (&power_regs)
static bool vbusdet, usb_reported_powered;
static bool nrf_power_usbregstatus_vbusdet_get(NRF_POWER_Type const *p_reg) {
    assert(p_reg == NRF_POWER);
    return vbusdet;
}
static bool zmk_usb_is_powered(void) { return usb_reported_powered; }
static uint8_t last_cell_soc;
static bool have_last_cell;
/* ACTUAL_REMEMBER */
static void remember_cell_soc(uint8_t live) {
    now_ms += 60000;
    last_state_of_charge = live;
    track_cell_level(live, vbus_present(), -1, now_ms);
}
static void boot(bool persisted, uint8_t persisted_soc) {
    have_last_cell = persisted;
    last_cell_soc = persisted ? persisted_soc : 0;
    prev_vbus = false;
    unplugged_seen = false;
    charge_start_soc = -1;
    (void)zmk_battery_charge_state;
    (void)zmk_battery_reported_soc;
    (void)needs_samples;
    /* The USB stack has not reported yet: usb_status is still USB_DC_UNKNOWN. */
    usb_reported_powered = false;
}
int main(void) {
    /* Boot on USB: the first sample (charge voltage) runs before the USB
     * status callback, so only VBUS detect knows the cable is there. */
    boot(true, 78);
    vbusdet = true;
    remember_cell_soc(96);
    assert(have_last_cell && last_cell_soc == 78);
    usb_reported_powered = true; /* USB stack catches up. */
    remember_cell_soc(100);
    assert(have_last_cell && last_cell_soc == 78);

    /* Boot on USB with nothing persisted: still unknown, not 96. */
    boot(false, 0);
    vbusdet = true;
    remember_cell_soc(96);
    assert(!have_last_cell);

    /* Boot on battery: the first sample is the cell level. */
    boot(true, 78);
    vbusdet = false;
    remember_cell_soc(64);
    assert(have_last_cell && last_cell_soc == 64);
    boot(false, 0);
    remember_cell_soc(64);
    assert(have_last_cell && last_cell_soc == 64);

    /* USB state alone still blocks a sample, e.g. if VBUS detect lags. */
    usb_reported_powered = true;
    remember_cell_soc(99);
    assert(last_cell_soc == 64);
    return 0;
}
"""


class BatteryUsbBootTests(unittest.TestCase):
    def test_boot_on_usb_keeps_persisted_cell_level(self):
        source = (Path(__file__).resolve().parents[3] / "src/battery.c").read_text()
        start = source.index("static bool vbus_present(")
        end = source.index("\n#if IS_ENABLED(CONFIG_SETTINGS)\nstatic void persist_cell_soc(", start)
        harness = HARNESS.replace("/* ACTUAL_REMEMBER */", source[start:end])
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "test_battery_usb_boot")
            subprocess.run(
                [os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                 "-x", "c", "-", "-o", executable],
                input=harness, text=True, check=True, timeout=30,
            )
            subprocess.run([executable], check=True, timeout=3)


if __name__ == "__main__":
    unittest.main()
