"""Exercise the real advertising worker with every multi-host option off."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#define IS_ENABLED(x) x
#define CONFIG_ZMK_USB 1
#define CONFIG_ZMK_BLE_HOST_ADV_OFF_ON_USB USB_OFF
#define CONFIG_ZMK_SPLIT_ROLE_CENTRAL 1
#define CONFIG_ZMK_BLE_ADV_TIMEOUT 0
#define CONFIG_ZMK_BLE_ADV_BOOST 0
#define CONFIG_ZMK_BLE_ADV_POST_SWITCH_DARK 0
#define CONFIG_ZMK_BLE_DIRECTED_ADV 0
#define CONFIG_ZMK_BLE_ADV_FILTER_ACTIVE_PROFILE 0
#define CONFIG_BT_FILTER_ACCEPT_LIST 0
#define CONFIG_ZMK_BLE_RESELECT_RECONNECT 0
#define CONFIG_ZMK_BLE_IDLE_DISCONNECT 0
#define CONFIG_ZMK_BLE_EVICT_ADV_COOLDOWN_MS 0
#define CONFIG_ZMK_BLE_PROFILE_HANDOFF 0
#define ZMK_BLE_PROFILE_COUNT 3
#define ARG_UNUSED(x) ((void)(x))
#define LOG_INF(...) ((void)0)
#define LOG_DBG(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#define BT_ID_DEFAULT 0
#define BT_ADDR_LE_STR_LEN 20
#define STRINGIFY(x) #x
#define BUILD_ASSERT(...)

typedef struct { uint8_t type, value; } bt_addr_le_t;
static const bt_addr_le_t any;
#define BT_ADDR_LE_ANY (&any)
static int bt_addr_le_cmp(const bt_addr_le_t *a, const bt_addr_le_t *b) {
    return a->type != b->type || a->value != b->value;
}
static void bt_addr_le_to_str(const bt_addr_le_t *a, char *s, size_t n) { (void)a; (void)s; (void)n; }
struct zmk_ble_profile { bt_addr_le_t peer; };
enum advertising_type { ZMK_ADV_NONE, ZMK_ADV_DIR, ZMK_ADV_CONN } advertising_status;
#define CURR_ADV(adv) (adv << 4)
enum zmk_ble_adv_stage { ZMK_BLE_ADV_STOP, ZMK_BLE_ADV_PEER, ZMK_BLE_ADV_CLEAR, ZMK_BLE_ADV_ADD,
    ZMK_BLE_ADV_START_OPEN, ZMK_BLE_ADV_START_FILTERED, ZMK_BLE_ADV_START_DIRECTED_RPA,
    ZMK_BLE_ADV_START_DIRECTED, ZMK_BLE_ADV_HANDOFF_DISCONNECT };

/* Advertising parameters only need to be distinguishable here. */
struct bt_le_adv_param { int interval_min, options; };
#define BT_LE_ADV_OPT_CONN 1
#define BT_LE_ADV_OPT_USE_NAME 2
#define BT_LE_ADV_OPT_FORCE_NAME_IN_AD 4
#define BT_LE_ADV_OPT_FILTER_CONN 8
#define BT_LE_ADV_OPT_DIR_ADDR_RPA 16
#define BT_GAP_ADV_FAST_INT_MIN_1 48
#define BT_GAP_ADV_FAST_INT_MAX_1 96
#define BT_GAP_ADV_FAST_INT_MIN_2 160
#define BT_GAP_ADV_FAST_INT_MAX_2 240
#define BT_LE_ADV_PARAM(opts, min, max, peer) (&(struct bt_le_adv_param){(min), (opts)})
#define BT_LE_ADV_CONN_DIR_LOW_DUTY(peer) BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, 1, 1, peer)
struct bt_data { int unused; };
static struct bt_data zmk_ble_ad[1];
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

struct k_spinlock { int unused; };
typedef int k_spinlock_key_t;
static int locks;
static k_spinlock_key_t k_spin_lock(struct k_spinlock *l) { (void)l; locks++; return 0; }
static void k_spin_unlock(struct k_spinlock *l, k_spinlock_key_t k) { (void)l; (void)k; locks--; }
typedef long atomic_t;
static void atomic_set_bit(atomic_t *a, int bit) { *a |= 1L << bit; }
static bool atomic_test_bit(atomic_t *a, int bit) { return *a & (1L << bit); }
static void atomic_clear_bit(atomic_t *a, int bit) { *a &= ~(1L << bit); }
static bool atomic_test_and_clear_bit(atomic_t *a, int bit) {
    bool was = atomic_test_bit(a, bit); atomic_clear_bit(a, bit); return was;
}
struct k_work { void (*handler)(struct k_work *); int pending; };
#define K_WORK_DEFINE(name, fn) struct k_work name = {fn, 0}
static int k_work_submit(struct k_work *w) { w->pending++; return 0; }
static int on_worker = 1;
static int k_current_get(void) { return on_worker; }
static int k_sys_work_q;
static int k_work_queue_thread_get(int *q) { (void)q; return 1; }
struct bt_conn { int unused; };
static struct bt_conn *bt_conn_lookup_addr_le(int id, const bt_addr_le_t *a) { (void)id; (void)a; return NULL; }
static void bt_conn_unref(struct bt_conn *c) { (void)c; }

static bool usb_powered;
static bool zmk_usb_is_powered(void) { return usb_powered; }
static int observed;
void zmk_ble_advertising_observed(uint8_t stage, int err) { (void)stage; (void)err; observed++; }

/* ACTUAL_TARGET */

static bool profile_connected;
bool zmk_ble_active_profile_is_open(void) { return !bt_addr_le_cmp(&profiles[active_profile].peer, BT_ADDR_LE_ANY); }
bool zmk_ble_active_profile_is_connected(void) { return profile_connected; }
bt_addr_le_t *zmk_ble_active_profile_addr(void) { return &profiles[active_profile].peer; }

static int starts, stops, start_err;
static bool running;
static void (*during_start)(void);
static int bt_le_adv_start(const struct bt_le_adv_param *p, const struct bt_data *ad, size_t n,
                           const void *sd, size_t sn) {
    (void)p; (void)ad; (void)n; (void)sd; (void)sn;
    starts++;
    if (during_start) { void (*f)(void) = during_start; during_start = NULL; f(); }
    if (start_err) return start_err;
    running = true;
    return 0;
}
static int bt_le_adv_stop(void) { stops++; running = false; return 0; }

/* ACTUAL_START */
/* ACTUAL_FAL */
/* ACTUAL_MACROS */
/* ACTUAL_UPDATE */

static void update_advertising_callback(struct k_work *work) { (void)work; update_advertising(); }

static void switch_profile(void) {
    k_spinlock_key_t key = k_spin_lock(&adv_target_lock);
    active_profile = 0;
    adv_target_version++;
    k_spin_unlock(&adv_target_lock, key);
}
static void run(void) {
    while (update_advertising_work.pending) { update_advertising_work.pending--; update_advertising(); }
}
static void reset(void) { starts = stops = 0; start_err = 0; }

int main(void) {
    /* Bonded profile 2, host away: advertise, remembering the target version. */
    profiles[0].peer = (bt_addr_le_t){0, 1};
    profiles[2].peer = (bt_addr_le_t){0, 2};
    active_profile = 2;
    assert(update_advertising() == 0);
    assert(advertising_status == ZMK_ADV_CONN && running && starts == 1 && locks == 0);
    assert(advertising_target_version == adv_target_version);

    /* A connected host: advertising stops. */
    profile_connected = true; reset();
    update_advertising();
    assert(advertising_status == ZMK_ADV_NONE && !running && stops == 1);
    profile_connected = false;

    /* A profile switch while the controller starts advertising: the stale start is
     * stopped and the work runs again for the new profile. */
    reset();
    during_start = switch_profile;
    assert(update_advertising() == 0);
    assert(!running && advertising_status == ZMK_ADV_NONE && stops == 1);
    assert(update_advertising_work.pending == 1);
    run();
    assert(running && advertising_status == ZMK_ADV_CONN && starts == 2);
    assert(advertising_target_version == adv_target_version);

    /* Advertising for an outdated target is restarted for the current one. */
    reset();
    adv_target_version++;
    update_advertising();
    assert(stops == 1 && starts == 1 && running);

    /* A connection ends connectable advertising in the controller: no stop call. */
    reset();
    atomic_set_bit(&adv_requests, ADV_LINK_COMPLETE);
    profile_connected = true;
    update_advertising();
    assert(stops == 0 && advertising_status == ZMK_ADV_NONE);
    profile_connected = false;

    /* A restart request (new device name) stops and starts again. */
    update_advertising(); reset();
    atomic_set_bit(&adv_requests, ADV_RESTART);
    update_advertising();
    assert(stops == 1 && starts == 1 && running && !atomic_test_bit(&adv_requests, ADV_RESTART));

    /* A start error is returned, as before. */
    bt_le_adv_stop(); advertising_status = ZMK_ADV_NONE; reset();
    start_err = -ENOMEM;
    assert(update_advertising() == -ENOMEM && advertising_status == ZMK_ADV_NONE);
    start_err = 0;
    update_advertising();
    assert(running);

    /* USB power: with CONFIG_ZMK_BLE_HOST_ADV_OFF_ON_USB, advertising stops; without, it runs. */
    reset(); usb_powered = true;
    update_advertising();
    assert(running == !USB_OFF && stops == (USB_OFF ? 1 : 0));
    usb_powered = false;
    update_advertising();
    assert(running);

    /* Off the system work queue, the call only queues the work. */
    reset(); on_worker = 0; adv_target_version++;
    update_advertising();
    assert(starts == 0 && stops == 0 && update_advertising_work.pending == 1);
    on_worker = 1; run();
    assert(running && advertising_target_version == adv_target_version);
    return 0;
}
"""


def _between(source, begin, end):
    start = source.index(begin)
    return source[start:source.index(end, start)]


class AdvertisingWorkerTests(unittest.TestCase):
    def test_worker_with_options_off(self):
        source = (Path(__file__).resolve().parents[3] / "src/ble.c").read_text()
        harness = HARNESS
        harness = harness.replace("/* ACTUAL_TARGET */",
                                  _between(source, "#define ZMK_ADV_OPTS_BASE", "\n#define DEVICE_NAME"))
        harness = harness.replace("/* ACTUAL_START */",
                                  _between(source, "static int start_advertising(", "\n#define CHECKED_ADV_STOP"))
        fal = ""
        if "static int prepare_active_fal(" in source:
            fal = _between(source, "static int prepare_active_fal(", "\n#define CHECKED_OPEN_ADV()")
        harness = harness.replace("/* ACTUAL_FAL */", fal)
        macros = (_between(source, "#define CHECKED_ADV_STOP()", "\n/* Directed advertising") +
                  _between(source, "#define CHECKED_DIR_ADV()", "\nint update_advertising(void);") +
                  _between(source, "#define CHECKED_OPEN_ADV()", "    } while (0)\n") + "    } while (0)\n")
        harness = harness.replace("/* ACTUAL_MACROS */", macros)
        harness = harness.replace("/* ACTUAL_UPDATE */",
                                  _between(source, "int update_advertising(void) {",
                                           "\nstatic void update_advertising_callback("))
        with tempfile.TemporaryDirectory() as directory:
            for usb_off in (0, 1):
                with self.subTest(host_adv_off_on_usb=usb_off):
                    executable = str(Path(directory) / f"worker_{usb_off}")
                    subprocess.run(
                        [os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                         "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-parameter",
                         "-Wno-unused-but-set-variable", f"-DUSB_OFF={usb_off}",
                         "-x", "c", "-", "-o", executable],
                        input=harness, text=True, check=True, timeout=30,
                    )
                    subprocess.run([executable], check=True, timeout=3)


if __name__ == "__main__":
    unittest.main()
