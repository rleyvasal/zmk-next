"""Exercise the real advertising transaction with interleaved profile/USB changes."""

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
#define CONFIG_ZMK_SPLIT_ROLE_CENTRAL 1
#define CONFIG_TOTEM_ADV_THROTTLE 1
#define CONFIG_TOTEM_EXCLUSIVE_HOST 1
#define CONFIG_ZMK_EXCLUSIVE_HOST 0
#define CONFIG_TOTEM_EXCLUSIVE_DISCONNECT_REASON 0x13
#define CONFIG_TOTEM_EVICT_ADV_COOLDOWN_MS 0
#define ZMK_BLE_PROFILE_HANDOFF 1
#define CONFIG_TOTEM_ACTIVE_ADV_FILTER FILTER
#define CONFIG_BT_FILTER_ACCEPT_LIST FILTER
#define CONFIG_TOTEM_RESELECT_RECONNECT 0
#define CONFIG_TOTEM_IDLE_DISCONNECT 0
#define CONFIG_TOTEM_DIR_THEN_OPEN 0
#define CONFIG_TOTEM_ADV_BOOST 0
#define CONFIG_TOTEM_ADV_THROTTLE_TIMEOUT_MIN 5
#define K_MINUTES(x) (x)
#define ARG_UNUSED(x) ((void)(x))
#define LOG_INF(...) ((void)0)
static void log_debug(const char *format, ...) { (void)format; }
#define LOG_DBG(...) log_debug(__VA_ARGS__)
#define LOG_WRN(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define BT_ID_DEFAULT 0
#define BT_ADDR_LE_PUBLIC 0
#define BT_ADDR_LE_RANDOM 1
#define BT_ADDR_LE_STR_LEN 20
#define ZMK_BLE_PROFILE_COUNT 3
typedef struct { uint8_t type, value; } bt_addr_le_t;
static const bt_addr_le_t any;
#define BT_ADDR_LE_ANY (&any)
static int bt_addr_le_cmp(const bt_addr_le_t *a, const bt_addr_le_t *b) {
    return a->type != b->type || a->value != b->value;
}
static void bt_addr_le_to_str(const bt_addr_le_t *a, char *s, size_t n) {
    (void)a; (void)s; (void)n;
}
#if FILTER
static bool bt_addr_le_is_identity(const bt_addr_le_t *addr) { return addr->type == 0; }
#endif
struct zmk_ble_profile { bt_addr_le_t peer; };
static struct zmk_ble_profile profiles[3] = {{{0, 1}}, {{0, 0}}, {{0, 2}}};
static uint8_t active_profile = 2;
enum advertising_type { ZMK_ADV_NONE, ZMK_ADV_DIR, ZMK_ADV_CONN };
static enum advertising_type advertising_status;
enum { ZMK_BLE_ADV_STOP, ZMK_BLE_ADV_PEER, ZMK_BLE_ADV_CLEAR, ZMK_BLE_ADV_ADD,
       ZMK_BLE_ADV_START_OPEN, ZMK_BLE_ADV_START_FILTERED,
       ZMK_BLE_ADV_HANDOFF_DISCONNECT };
static bool usb_powered, locked;
static bool zmk_usb_is_powered(void) { return usb_powered; }
struct k_spinlock { int unused; };
typedef int k_spinlock_key_t;
static int k_spin_lock(struct k_spinlock *lock) {
    (void)lock; assert(!locked); locked = true; return 0;
}
static void k_spin_unlock(struct k_spinlock *lock, int key) {
    (void)lock; (void)key; assert(locked); locked = false;
}
typedef unsigned atomic_t;
static void atomic_set_bit(atomic_t *a, int bit) { *a |= 1U << bit; }
static bool atomic_test_and_clear_bit(atomic_t *a, int bit) {
    bool set = (*a & (1U << bit)) != 0; *a &= ~(1U << bit); return set;
}
static bool atomic_test_bit(atomic_t *a, int bit) { return (*a & (1U << bit)) != 0; }
static void atomic_clear_bit(atomic_t *a, int bit) { *a &= ~(1U << bit); }
struct k_work { void (*handler)(struct k_work *); };
#define K_WORK_DEFINE(name, callback) struct k_work name = {callback}
static struct { int thread; } k_sys_work_q;
static int current_thread, queued;
static int k_current_get(void) { return current_thread; }
static int k_work_queue_thread_get(void *queue) { (void)queue; return 1; }
static void k_work_submit(struct k_work *work) { (void)work; queued++; }
/* ACTUAL_TARGET */
static void update_advertising_callback(struct k_work *work) { (void)work; }
static int ble_save_profile(void) { return 0; }
static void raise_profile_changed_event(void) {}
/* ACTUAL_SELECT */
struct bt_le_adv_param { int filtered; };
#define BT_CONN_ROLE_PERIPHERAL 1
#define BT_CONN_ROLE_CENTRAL 0
#define BT_CONN_TYPE_LE 1
enum { BT_CONN_STATE_DISCONNECTED, BT_CONN_STATE_CONNECTING,
       BT_CONN_STATE_CONNECTED, BT_CONN_STATE_DISCONNECTING };
struct bt_conn_info { int role, state; };
struct bt_conn { struct bt_conn_info info; bt_addr_le_t addr; };
static struct bt_conn connections[3];
static int disconnects, disconnect_error, change_during_disconnect, usb_during_disconnect;
static int bt_conn_get_info(const struct bt_conn *conn, struct bt_conn_info *info) {
    *info = conn->info; return 0;
}
static const bt_addr_le_t *bt_conn_get_dst(const struct bt_conn *conn) { return &conn->addr; }
static int zmk_ble_profile_index(const bt_addr_le_t *addr) {
    for (int i = 0; i < 3; i++) {
        if (!bt_addr_le_cmp(addr, &profiles[i].peer)) { return i; }
    }
    return -1;
}
static void bt_conn_foreach(int type, void (*cb)(struct bt_conn *, void *), void *data) {
    assert(type == BT_CONN_TYPE_LE);
    for (int i = 0; i < 3; i++) { cb(&connections[i], data); }
}
static int bt_conn_disconnect(struct bt_conn *conn, uint8_t reason) {
    assert(advertising_worker() && !locked && advertising_status == ZMK_ADV_NONE);
    assert(reason == 0x13); disconnects++;
    if (!disconnect_error) { conn->info.state = BT_CONN_STATE_DISCONNECTING; }
    if (change_during_disconnect) { zmk_ble_prof_select(2); }
    if (usb_during_disconnect) { usb_powered = true; adv_target_version++; }
    return disconnect_error;
}
static struct bt_conn *bt_conn_lookup_addr_le(int id, const bt_addr_le_t *addr) {
    (void)id; (void)addr; return NULL;
}
static void bt_conn_unref(struct bt_conn *conn) { (void)conn; }
static const struct bt_le_adv_param open_param = {0}, filtered_param = {1};
#define ZMK_ADV_CONN_NAME (&open_param)
#define ZMK_ADV_CONN_NAME_BOOST (&open_param)
#define ZMK_ADV_CONN_NAME_FILTER (&filtered_param)
#define ZMK_ADV_CONN_NAME_BOOST_FILTER (&filtered_param)
#define BT_LE_ADV_CONN_DIR_LOW_DUTY(addr) (&open_param)
#define BT_LE_ADV_OPT_DIR_ADDR_RPA 0
static const bool totem_adv_boost_active;
static const int zmk_ble_ad[] = {0};
static int starts, stops, clears, adds, stop_error, start_error, setup_error;
static int change_during_start, change_during_add, usb_during_start;
static int selected_peer, actual_filtered;
static void zmk_ble_advertising_observed(uint8_t stage, int err) {
    (void)stage; (void)err; assert(!locked);
}
static int bt_le_adv_stop(void) { assert(!locked); stops++; return stop_error; }
static int bt_le_adv_start(const struct bt_le_adv_param *param, const void *data,
                          size_t len, const void *scan, size_t scan_len) {
    (void)data; (void)len; (void)scan; (void)scan_len;
    assert(advertising_worker() && !locked); starts++;
    actual_filtered = param->filtered;
    if (change_during_start) { zmk_ble_prof_select(0); }
    if (usb_during_start) { usb_powered = true; }
    return start_error;
}
/* ACTUAL_START */
struct bt_bond_info { bt_addr_le_t addr; };
static bool missing_bond;
static void bt_foreach_bond(int id, void (*cb)(const struct bt_bond_info *, void *), void *ctx) {
    (void)id;
    if (missing_bond) { return; }
    const struct bt_bond_info mac = {{0, 1}}, windows = {{0, 2}};
    cb(&mac, ctx); cb(&windows, ctx);
}
#if FILTER
static int bt_le_filter_accept_list_clear(void) {
    assert(advertising_worker() && !locked); clears++; return setup_error;
}
static int bt_le_filter_accept_list_add(const bt_addr_le_t *peer) {
    assert(advertising_worker() && !locked); adds++; selected_peer = peer->value;
    if (change_during_add) { zmk_ble_prof_select(0); }
    return setup_error;
}
#endif
#define CURR_ADV(adv) (adv << 4)
/* ACTUAL_STOP */
/* ACTUAL_FAL */
#undef CHECKED_DIR_ADV
#define CHECKED_DIR_ADV() do { (void)addr; (void)conn; err = -EINVAL; } while (0)
static bool adv_throttled, host_connected;
static int adv_throttle_work, open_adv_retry_work, retry_requests;
static uint8_t open_adv_retry_count;
static void k_work_schedule(int *work, int delay) { (void)work; (void)delay; }
static void k_work_cancel_delayable(int *work) { (void)work; }
static void open_adv_retry_arm(void) { retry_requests++; }
/* ACTUAL_APPLY */
static bool zmk_ble_active_profile_is_open(void) {
    return !bt_addr_le_cmp(&profiles[active_profile].peer, BT_ADDR_LE_ANY);
}
static bool zmk_ble_active_profile_is_connected(void) { return host_connected; }
/* ACTUAL_UPDATE */
static struct k_work raise_profile_changed_event_work;
static bool is_conn_active_profile(const struct bt_conn *conn) {
    return zmk_ble_profile_index(bt_conn_get_dst(conn)) == active_profile;
}
/* ACTUAL_DISCONNECTED */
/* ACTUAL_IDENTITY */
static int apply_open(void) {
    int err = 0;
    struct adv_target target = adv_target_snapshot();
    CHECKED_OPEN_ADV();
    return err;
}
static void reset(void) {
    active_profile = 2; adv_target_version = 0; advertising_target_version = 0;
    advertising_status = ZMK_ADV_NONE; usb_powered = false;
    starts = stops = clears = adds = queued = 0;
    stop_error = start_error = setup_error = 0;
    change_during_start = change_during_add = usb_during_start = 0;
    adv_requests = 0;
    profile_handoff_pending = false;
    adv_profile_index = 2;
    adv_throttled = host_connected = false; retry_requests = 0;
    missing_bond = false;
    disconnects = disconnect_error = change_during_disconnect = usb_during_disconnect = 0;
    for (int i = 0; i < 3; i++) {
        connections[i] = (struct bt_conn){.info = {BT_CONN_ROLE_PERIPHERAL,
                                                  BT_CONN_STATE_DISCONNECTED}};
    }
}
int main(void) {
    (void)bt_conn_lookup_addr_le; (void)bt_conn_unref; (void)bt_addr_le_to_str;
    current_thread = 0;
    /* Profile selection and helper requests do not touch the controller. */
    zmk_ble_prof_select(0); request_advertising(ADV_REARM);
    assert(starts == 0 && stops == 0 && queued == 2);
    assert(active_profile == 0 && adv_target_version == 1);
    update_advertising();
    assert(starts == 0 && stops == 0 && queued == 3);
    current_thread = 1; reset();
    /* Only the owner executes controller work; USB and throttle still win. */
    update_advertising();
    assert(starts == 1 && advertising_status == ZMK_ADV_CONN);
    reset(); usb_powered = true; update_advertising(); assert(starts == 0);
    reset(); adv_throttled = true; update_advertising(); assert(starts == 0);
    reset();
    assert(apply_open() == 0 && starts == 1 && actual_filtered == FILTER);
    assert(advertising_status == ZMK_ADV_CONN);
#if FILTER
    assert(stops == 1 && clears == 1 && adds == 1 && selected_peer == 2);
    /* A profile change during list setup must not start the stale Windows list. */
    reset(); change_during_add = 1;
    apply_open();
    assert(starts == 0 && advertising_status == ZMK_ADV_NONE && queued >= 1);
    change_during_add = 0; update_advertising();
    assert(starts == 1 && selected_peer == 1 && actual_filtered);
    reset(); stop_error = -EIO; advertising_status = ZMK_ADV_CONN;
    assert(apply_open() == -EIO);
    assert(clears == 0 && adds == 0 && starts == 0 && advertising_status == ZMK_ADV_CONN);
    reset(); missing_bond = true;
    assert(apply_open() == -ENOENT && starts == 0 && adds == 0);
    reset(); setup_error = -EIO;
    assert(apply_open() == -EIO && starts == 0 && adds == 0);
#else
    (void)selected_peer; (void)filtered_param;
#endif
    /* A profile switch during HCI start stops the stale result and queues new work. */
    reset(); change_during_start = 1;
    apply_open();
    assert(starts == 1 && stops == FILTER + 1);
    assert(advertising_status == ZMK_ADV_NONE && queued >= 1);
    change_during_start = 0; update_advertising();
    assert(starts == 2 && advertising_status == ZMK_ADV_CONN);
    /* USB priority is checked immediately before and after starting. */
    reset(); usb_powered = true; apply_open();
    assert(starts == 0);
    reset(); usb_during_start = 1; apply_open();
    assert(starts == 1 && advertising_status == ZMK_ADV_NONE);
    /* A failed stale-result stop preserves the known running state. */
    reset(); change_during_start = 1;
    struct adv_target target = adv_target_snapshot();
    stop_error = -EIO;
    assert(start_advertising(&target, &open_param, ZMK_ADV_CONN, ZMK_BLE_ADV_START_OPEN) == -EIO);
    assert(advertising_status == ZMK_ADV_CONN && queued >= 1);
    /* A stale snapshot is rejected even after switching away and back. */
    reset(); target = adv_target_snapshot();
    zmk_ble_prof_select(0); zmk_ble_prof_select(2);
    assert(start_advertising(&target, &open_param, ZMK_ADV_CONN, ZMK_BLE_ADV_START_OPEN) == -EAGAIN);
    assert(starts == 0);
    /* Pairing stays open only for an empty profile. */
    reset(); zmk_ble_prof_select(1); update_advertising();
    assert(starts == 1 && !actual_filtered && adds == 0);
    /* Explicit intent works even before the first profile notification. */
    reset();
    connections[0] = (struct bt_conn){.info = {1, BT_CONN_STATE_CONNECTED}, .addr = {0, 2}};
    connections[1] = (struct bt_conn){.info = {0, BT_CONN_STATE_CONNECTED}, .addr = {0, 99}};
    advertising_status = ZMK_ADV_CONN;
    zmk_ble_prof_select(0);
    assert(update_advertising() == 0 && starts == 0 && disconnects == 1);
    assert(stops == 1 && profile_handoff_pending);
    assert(connections[1].info.state == BT_CONN_STATE_CONNECTED); /* Split survives. */
    update_advertising();
    assert(starts == 0 && disconnects == 1); /* No duplicate disconnect commands. */
    connections[0].info.state = BT_CONN_STATE_DISCONNECTED;
    disconnected(&connections[0], 0x16); update_advertising();
    assert(starts == 1 && !profile_handoff_pending);
    /* The reverse direction follows the same disconnect-completion barrier. */
    reset(); active_profile = adv_profile_index = 0;
    connections[0] = (struct bt_conn){.info = {1, BT_CONN_STATE_CONNECTED}, .addr = {0, 1}};
    zmk_ble_prof_select(2); update_advertising();
    assert(disconnects == 1 && starts == 0);
    connections[0].info.state = BT_CONN_STATE_DISCONNECTED;
    disconnected(&connections[0], 0x16); update_advertising();
    assert(starts == 1 && !profile_handoff_pending);
    /* USB cancels radio work while a handoff is waiting. */
    reset(); connections[0] = (struct bt_conn){.info = {1, BT_CONN_STATE_CONNECTED}, .addr = {0, 2}};
    zmk_ble_prof_select(0); usb_powered = true; update_advertising();
    assert(starts == 0 && disconnects == 0 && profile_handoff_pending);
    usb_powered = false; update_advertising();
    assert(starts == 0 && disconnects == 1);
    usb_powered = true; connections[0].info.state = BT_CONN_STATE_DISCONNECTED;
    disconnected(&connections[0], 0x16); update_advertising(); assert(starts == 0);
    /* Neither stop nor disconnect failures may open advertising. */
    reset(); advertising_status = ZMK_ADV_CONN; stop_error = -EIO;
    zmk_ble_prof_select(0); assert(update_advertising() == -EIO);
    assert(starts == 0 && disconnects == 0 && advertising_status == ZMK_ADV_CONN);
    reset(); connections[0] = (struct bt_conn){.info = {1, BT_CONN_STATE_CONNECTED}, .addr = {0, 2}};
    zmk_ble_prof_select(0); disconnect_error = -EIO;
    assert(update_advertising() == -EIO && starts == 0 && profile_handoff_pending);
    disconnect_error = 0; update_advertising(); assert(disconnects == 2 && starts == 0);
    /* Missing bonds do not evict the working host; empty slots still allow pairing. */
    reset(); connections[0] = (struct bt_conn){.info = {1, BT_CONN_STATE_CONNECTED}, .addr = {0, 2}};
    missing_bond = true; zmk_ble_prof_select(0);
    assert(update_advertising() == -ENOENT && disconnects == 0 && starts == 0);
    zmk_ble_prof_select(1); update_advertising();
    assert(disconnects == 0 && starts == 1 && !actual_filtered);
    /* Unresolved host waits for identity, never gets blindly evicted. */
    reset(); connections[0] = (struct bt_conn){.info = {1, BT_CONN_STATE_CONNECTED}, .addr = {0, 99}};
    zmk_ble_prof_select(0); update_advertising();
    assert(disconnects == 0 && starts == 0 && profile_handoff_pending);
    connections[0].addr.value = 2;
    handoff_identity_resolved(&connections[0], NULL, &connections[0].addr);
    update_advertising(); assert(disconnects == 1 && starts == 0);
    /* Rapid switches coalesce to the latest target, preserving its existing link. */
    reset(); connections[0] = (struct bt_conn){.info = {1, BT_CONN_STATE_CONNECTED}, .addr = {0, 2}};
    host_connected = true; zmk_ble_prof_select(0); zmk_ble_prof_select(2);
    update_advertising(); assert(disconnects == 0 && starts == 0 && !profile_handoff_pending);
    /* A switch inside HCI cannot advertise the now-stale target. */
    reset(); connections[0] = (struct bt_conn){.info = {1, BT_CONN_STATE_CONNECTED}, .addr = {0, 2}};
    zmk_ble_prof_select(0); change_during_disconnect = 1; update_advertising();
    assert(starts == 0 && profile_handoff_pending && active_profile == 2);
    change_during_disconnect = 0; update_advertising();
    assert(starts == 0 && profile_handoff_pending);
    connections[0].info.state = BT_CONN_STATE_DISCONNECTED;
    disconnected(&connections[0], 0x16); update_advertising();
    assert(starts == 1 && !profile_handoff_pending);
    /* Returning to the last advertised profile must still wake its throttled ads. */
    reset(); adv_throttled = true;
    zmk_ble_prof_select(0); zmk_ble_prof_select(2); update_advertising();
    assert(starts == 1 && !adv_throttled && !profile_handoff_pending);
    reset(); connections[0] = (struct bt_conn){.info = {1, BT_CONN_STATE_CONNECTED}, .addr = {0, 2}};
    zmk_ble_prof_select(0); usb_during_disconnect = 1; update_advertising();
    assert(starts == 0 && usb_powered && profile_handoff_pending);
    return 0;
}
"""


class AdvertisingOwnerTests(unittest.TestCase):
    def test_stale_transactions_and_usb_priority(self):
        source = (Path(__file__).resolve().parents[3] / "src/ble.c").read_text()
        ranges = {
            "TARGET": ("/* Profile writers", "\n#define DEVICE_NAME"),
            "SELECT": ("int zmk_ble_prof_select(", "\nint zmk_ble_prof_next("),
            "START": ("static int start_advertising(", "\n#define CHECKED_ADV_STOP"),
            "FAL": ("#if (IS_ENABLED(CONFIG_TOTEM_ACTIVE_ADV_FILTER) &&", "\n#if IS_ENABLED(CONFIG_TOTEM_ADV_THROTTLE) &&"),
            "STOP": ("#define CHECKED_ADV_STOP()", "\n/* Directed advertising"),
            "UPDATE": ("int update_advertising(void) {", "\nstatic void update_advertising_callback("),
            "DISCONNECTED": ("static void disconnected(", "\nstatic void security_changed("),
            "IDENTITY": ("static void handoff_identity_resolved(", "\n#endif"),
            "APPLY": ("static uint8_t adv_profile_index;", "\nstatic int adv_throttle_profile_changed_listener("),
        }
        harness = HARNESS
        for name, (begin, end) in ranges.items():
            start = source.index(begin)
            stop = source.index(end, start)
            harness = harness.replace(f"/* ACTUAL_{name} */", source[start:stop])
        with tempfile.TemporaryDirectory() as directory:
            for filtering in (0, 1):
                with self.subTest(filtering=filtering):
                    executable = str(Path(directory) / f"advertising_{filtering}")
                    subprocess.run(
                        [os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                         f"-DFILTER={filtering}", "-x", "c", "-", "-o", executable],
                        input=harness, text=True, check=True, timeout=30,
                    )
                    subprocess.run([executable], check=True, timeout=3)


if __name__ == "__main__":
    unittest.main()
