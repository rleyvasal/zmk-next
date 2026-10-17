"""Transport choice while USB power is present, with the option off and on.

Host advertising on USB power (CONFIG_ZMK_BLE_HOST_ADV_OFF_ON_USB) is covered by
ble/advertising_worker."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ENDPOINT_HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#define IS_ENABLED(x) x
#define CONFIG_ZMK_USB 1
#define CONFIG_ZMK_BLE 1
#define LOG_DBG(...) ((void)0)
enum zmk_transport { ZMK_TRANSPORT_NONE, ZMK_TRANSPORT_USB, ZMK_TRANSPORT_BLE };
static enum zmk_transport preferred_transport;
static bool usb_powered, usb_hid_ready, ble_connected;
static bool zmk_usb_is_powered(void) { return usb_powered; }
static bool zmk_usb_is_hid_ready(void) { return usb_hid_ready; }
static bool zmk_ble_active_profile_is_connected(void) { return ble_connected; }
/* ACTUAL_SELECTION */
static enum zmk_transport pick(enum zmk_transport pref, bool powered, bool hid, bool ble) {
    preferred_transport = pref; usb_powered = powered; usb_hid_ready = hid; ble_connected = ble;
    return get_selected_transport();
}
int main(void) {
    /* Unchanged either way. */
    assert(pick(ZMK_TRANSPORT_USB, true, true, true) == ZMK_TRANSPORT_USB);
    assert(pick(ZMK_TRANSPORT_USB, false, false, true) == ZMK_TRANSPORT_BLE);
    assert(pick(ZMK_TRANSPORT_BLE, true, true, true) == ZMK_TRANSPORT_BLE);
    assert(pick(ZMK_TRANSPORT_BLE, true, true, false) == ZMK_TRANSPORT_USB);
    /* Cable in, USB HID not ready yet (or a charge-only cable). */
    assert(pick(ZMK_TRANSPORT_USB, true, false, true) ==
           (NO_FALLBACK ? ZMK_TRANSPORT_NONE : ZMK_TRANSPORT_BLE));
    return 0;
}
"""

def _function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 3]


def _compile_and_run(harness, name, defines):
    with tempfile.TemporaryDirectory() as directory:
        executable = str(Path(directory) / name)
        subprocess.run(
            [os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
             "-Wno-unused-variable", "-Wno-unused-but-set-variable", "-Wno-unused-value", *defines,
             "-x", "c", "-", "-o", executable],
            input=harness, text=True, check=True, timeout=30,
        )
        subprocess.run([executable], check=True, timeout=3)


class CabledTests(unittest.TestCase):
    def setUp(self):
        self.app = Path(__file__).resolve().parents[3]

    def test_transport_choice_while_powered(self):
        source = (self.app / "src/endpoints.c").read_text()
        selection = (_function(source, "static bool is_usb_ready(void) {") +
                     _function(source, "static bool is_ble_ready(void) {") +
                     _function(source, "static enum zmk_transport get_selected_transport(void) {"))
        harness = ENDPOINT_HARNESS.replace("/* ACTUAL_SELECTION */", selection)
        for on in (0, 1):
            with self.subTest(no_fallback=on):
                _compile_and_run(harness, f"endpoint_{on}",
                                 [f"-DCONFIG_ZMK_USB_NO_BLE_FALLBACK_WHEN_POWERED={on}",
                                  f"-DNO_FALLBACK={on}"])


if __name__ == "__main__":
    unittest.main()
