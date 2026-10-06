"""Exercise advertising policy with restored profiles and link-only events."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define IS_ENABLED(x) x
#define CONFIG_TOTEM_ADV_THROTTLE 1
#define CONFIG_ZMK_SPLIT_ROLE_CENTRAL 1
#define CONFIG_ZMK_BLE_CLEAR_BONDS_ON_START 0
#define CONFIG_TOTEM_IDLE_DISCONNECT FEATURES
#define CONFIG_TOTEM_DIR_THEN_OPEN FEATURES
#define CONFIG_TOTEM_ADV_BOOST FEATURES
#define CONFIG_TOTEM_EVICT_ADV_COOLDOWN_MS (FEATURES * 500)
#define ARG_UNUSED(x) ((void)(x))
#define LOG_WRN(...) ((void)0)
#define LOG_INF(...) ((void)0)
#define ZMK_EV_EVENT_BUBBLE 0
typedef int zmk_event_t;
enum { ZMK_ADV_NONE, ZMK_ADV_CONN, ZMK_ADV_DIR };
static uint8_t active_profile = 2;
static bool adv_throttled;
static int advertising_status, stops, updates;
#if FEATURES
static bool idle_go_dark;
static int evict_adv_cooldown_work, cancelled, directed, boosts;
static void k_work_cancel_delayable(int *work) {
    assert(work == &evict_adv_cooldown_work); cancelled++;
}
static void totem_dir_phase_arm(void) { directed++; }
static void totem_adv_boost_arm(void) { boosts++; }
#endif
static int bt_le_adv_stop(void) { stops++; return 0; }
static int update_advertising(void) { updates++; return 0; }
static int conn_callbacks, zmk_ble_auth_cb_display, zmk_ble_auth_info_cb_display;
static void bt_conn_cb_register(int *cb) { assert(cb == &conn_callbacks); }
static void bt_conn_auth_cb_register(int *cb) { assert(cb == &zmk_ble_auth_cb_display); }
static void bt_conn_auth_info_cb_register(int *cb) { assert(cb == &zmk_ble_auth_info_cb_display); }
static void zmk_ble_ready(int err) { assert(err == 0); }
/* ACTUAL_LISTENER */
/* ACTUAL_STARTUP */
int main(void) {
    assert(zmk_ble_complete_startup() == 0);
    assert(adv_profile_index == 2);
    adv_throttled = true;
    advertising_status = ZMK_ADV_CONN;
#if FEATURES
    idle_go_dark = true;
#endif
    /* Connect/disconnect notifications must not reopen or boost advertising. */
    for (int i = 0; i < 3; i++) {
        assert(adv_throttle_profile_changed_listener(NULL) == 0);
    }
    assert(adv_throttled && stops == 0 && updates == 0);
#if FEATURES
    assert(idle_go_dark && cancelled == 0 && directed == 0 && boosts == 0);
#endif
    /* A real switch, even before any host connected, still acts immediately. */
    active_profile = 0;
    assert(adv_throttle_profile_changed_listener(NULL) == 0);
    assert(!adv_throttled && adv_profile_index == 0 && stops == 1 && updates == 1);
#if FEATURES
    assert(!idle_go_dark && cancelled == 1 && directed == 1 && boosts == 1);
#endif
    adv_throttle_profile_changed_listener(NULL);
    assert(stops == 1 && updates == 1);
    active_profile = 2;
    advertising_status = ZMK_ADV_DIR;
    adv_throttle_profile_changed_listener(NULL);
    assert(stops == 2 && updates == 2);
    return 0;
}
"""


class ProfileNotificationTests(unittest.TestCase):
    def test_only_real_profile_switch_restarts_advertising(self):
        source = (Path(__file__).resolve().parents[3] / "src/ble.c").read_text()
        start = source.index("static uint8_t adv_profile_index;")
        end = source.index("\nZMK_LISTENER(totem_adv_throttle_profile", start)
        startup = source.index("static int zmk_ble_complete_startup(void) {")
        startup_end = source.index("\nstatic int zmk_ble_init(", startup)
        harness = HARNESS.replace("/* ACTUAL_LISTENER */", source[start:end])
        harness = harness.replace("/* ACTUAL_STARTUP */", source[startup:startup_end])
        with tempfile.TemporaryDirectory() as directory:
            for features in (0, 1):
                with self.subTest(features=features):
                    executable = str(Path(directory) / f"test_profile_{features}")
                    subprocess.run(
                        [os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                         f"-DFEATURES={features}", "-x", "c", "-", "-o", executable],
                        input=harness, text=True, check=True, timeout=30,
                    )
                    subprocess.run([executable], check=True, timeout=3)


if __name__ == "__main__":
    unittest.main()
