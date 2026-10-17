"""The split link slows down while the keyboard is idle and speeds up on the first key press."""

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
#define ARG_UNUSED(x) ((void)(x))
#define LOG_WRN(...) ((void)0)
#define ZMK_EV_EVENT_BUBBLE 0
#define CONFIG_ZMK_SPLIT_BLE_PREF_INT 12
#define CONFIG_ZMK_SPLIT_BLE_PREF_LATENCY 30
#define CONFIG_ZMK_SPLIT_BLE_PREF_TIMEOUT 400
#define ZMK_SPLIT_BLE_PERIPHERAL_COUNT 2
struct k_work { void (*handler)(struct k_work *); int pending; };
#define K_WORK_DEFINE(name, fn) struct k_work name = {fn, 0}
static void k_work_submit(struct k_work *work) { work->pending++; }
static void run(struct k_work *work) {
    while (work->pending) { work->pending--; work->handler(work); }
}
struct bt_conn { int id; };
struct bt_le_conn_param { uint16_t interval_min, interval_max, latency, timeout; };
#define BT_LE_CONN_PARAM_INIT(a, b, c, d) {a, b, c, d}
static struct bt_le_conn_param sent[8];
static struct bt_conn *sent_to[8];
static int updates;
static int bt_conn_le_param_update(struct bt_conn *conn, const struct bt_le_conn_param *p) {
    sent[updates] = *p; sent_to[updates] = conn; updates++;
    return 0;
}
enum peripheral_slot_state { PERIPHERAL_SLOT_STATE_OPEN, PERIPHERAL_SLOT_STATE_CONNECTING,
                             PERIPHERAL_SLOT_STATE_CONNECTED };
struct peripheral_slot { enum peripheral_slot_state state; struct bt_conn *conn; };
static struct bt_conn right = {1}, other = {2};
static struct peripheral_slot peripherals[2];
enum zmk_activity_state { ZMK_ACTIVITY_ACTIVE, ZMK_ACTIVITY_IDLE, ZMK_ACTIVITY_SLEEP };
static enum zmk_activity_state activity;
static enum zmk_activity_state zmk_activity_get_state(void) { return activity; }
typedef int zmk_event_t;
static const int *as_zmk_physical_layout_selection_changed(const zmk_event_t *e) {
    return *e == 1 ? e : NULL;
}
static const int *as_zmk_activity_state_changed(const zmk_event_t *e) {
    return *e == 2 ? e : NULL;
}
static struct k_work update_peripherals_selected_layouts_work;
/* ACTUAL_IDLE */
/* ACTUAL_LISTENER */
static void activity_changed(enum zmk_activity_state state) {
    const zmk_event_t ev = 2;
    activity = state;
    assert(zmk_split_bt_central_listener_cb(&ev) == ZMK_EV_EVENT_BUBBLE);
    run(&split_conn_params_work);
}
/* Bluetooth: timeout (10 ms) > (1 + latency) * interval (1.25 ms) * 2. */
static bool valid(struct split_conn_params p) {
    return (uint32_t)p.timeout * 1000 > (uint32_t)(1 + p.latency) * p.interval * 250;
}
int main(void) {
    struct split_conn_params fast = split_conn_params_for(false);
    struct split_conn_params idle = split_conn_params_for(true);
    assert(fast.interval == 12 && fast.latency == 30 && fast.timeout == 400 && valid(fast));
    assert(idle.interval == IDLE_INT && idle.timeout == 400 && valid(idle));
    assert(idle.latency == EXPECTED_IDLE_LATENCY);
    if (idle.latency < CONFIG_ZMK_SPLIT_BLE_IDLE_LATENCY) {
        idle.latency++;
        assert(!valid(idle)); /* Lowered only as far as needed to fit. */
    }

    peripherals[0] = (struct peripheral_slot){PERIPHERAL_SLOT_STATE_CONNECTED, &right};
    peripherals[1] = (struct peripheral_slot){PERIPHERAL_SLOT_STATE_CONNECTING, &other};

    activity_changed(ZMK_ACTIVITY_ACTIVE); /* Already fast: nothing to send. */
    assert(updates == 0);

    activity_changed(ZMK_ACTIVITY_IDLE);
    assert(updates == 1 && sent_to[0] == &right); /* Only connected slots. */
    assert(sent[0].interval_min == IDLE_INT && sent[0].interval_max == IDLE_INT);
    assert(sent[0].latency == EXPECTED_IDLE_LATENCY && sent[0].timeout == 400);

    activity_changed(ZMK_ACTIVITY_SLEEP); /* Still idle: no second update. */
    assert(updates == 1);

    activity_changed(ZMK_ACTIVITY_ACTIVE); /* First key press. */
    assert(updates == 2 && sent[1].interval_min == 12 && sent[1].interval_max == 12);
    assert(sent[1].latency == 30);

    const zmk_event_t layout = 1;
    assert(zmk_split_bt_central_listener_cb(&layout) == ZMK_EV_EVENT_BUBBLE);
    assert(update_peripherals_selected_layouts_work.pending == 1 && updates == 2);
    return 0;
}
"""


SWEEP = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define CONFIG_ZMK_SPLIT_BLE_PREF_INT 12
#define CONFIG_ZMK_SPLIT_BLE_PREF_LATENCY 30
#define CONFIG_ZMK_SPLIT_BLE_PREF_TIMEOUT 400
#define CONFIG_ZMK_SPLIT_BLE_IDLE_LATENCY 30
/* ACTUAL_PARAMS */
int main(void) {
    /* Every legal interval (7.5 ms .. 4 s) gives parameters Bluetooth accepts. */
    for (int i = 6; i <= 3200; i++) {
        idle_int = i;
        struct split_conn_params p = split_conn_params_for(true);
        assert(p.interval == i && p.timeout == 400);
        if ((uint32_t)p.timeout * 1000 > (uint32_t)p.interval * 250) {
            assert((uint32_t)p.timeout * 1000 > (uint32_t)(1 + p.latency) * p.interval * 250);
        }
    }
    return 0;
}
"""


def _section(source, start, end, include_end=False):
    i = source.index(start)
    j = source.index(end, i)
    return source[i:j + len(end)] if include_end else source[i:j]


def _compile_and_run(source, name, defines=()):
    with tempfile.TemporaryDirectory() as directory:
        executable = str(Path(directory) / name)
        subprocess.run(
            [os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror", *defines,
             "-x", "c", "-", "-o", executable],
            input=source, text=True, check=True, timeout=30,
        )
        subprocess.run([executable], check=True, timeout=3)


class SplitIdleIntervalTests(unittest.TestCase):
    def setUp(self):
        source = (Path(__file__).resolve().parents[3] / "src/split/bluetooth/central.c").read_text()
        self.idle = _section(source, "struct split_conn_params {",
                             "#endif // CONFIG_ZMK_SPLIT_BLE_IDLE_INT > 0")
        self.listener = _section(source, "static int zmk_split_bt_central_listener_cb(",
                                 "\nZMK_LISTENER(zmk_split_bt_central")
        self.listener = self.listener.replace("#if CONFIG_ZMK_SPLIT_BLE_IDLE_INT > 0\n", "")
        self.listener = self.listener.replace("#endif\n", "")

    def harness(self):
        return HARNESS.replace("/* ACTUAL_IDLE */", self.idle).replace(
            "/* ACTUAL_LISTENER */", self.listener)

    def test_totem_idle_interval_and_latency(self):
        # Totem: 60 ms with latency 4, so the right half listens every 300 ms.
        _compile_and_run(self.harness(), "idle48",
                         ["-DCONFIG_ZMK_SPLIT_BLE_IDLE_INT=48",
                          "-DCONFIG_ZMK_SPLIT_BLE_IDLE_LATENCY=4", "-DIDLE_INT=48",
                          "-DEXPECTED_IDLE_LATENCY=4"])

    def test_idle_latency_kept_when_it_fits(self):
        # 60 ms: (1 + 30) * 60 ms * 2 = 3.72 s fits the 4 s timeout.
        _compile_and_run(self.harness(), "idle48l30",
                         ["-DCONFIG_ZMK_SPLIT_BLE_IDLE_INT=48",
                          "-DCONFIG_ZMK_SPLIT_BLE_IDLE_LATENCY=30", "-DIDLE_INT=48",
                          "-DEXPECTED_IDLE_LATENCY=30"])

    def test_slower_idle_interval_lowers_latency(self):
        # 100 ms: latency 30 would need 6.2 s; the largest that fits 4 s is 18.
        _compile_and_run(self.harness(), "idle80",
                         ["-DCONFIG_ZMK_SPLIT_BLE_IDLE_INT=80",
                          "-DCONFIG_ZMK_SPLIT_BLE_IDLE_LATENCY=30", "-DIDLE_INT=80",
                          "-DEXPECTED_IDLE_LATENCY=18"])

    def test_every_interval_gives_valid_parameters(self):
        params = _section(self.idle, "struct split_conn_params {", "\nstatic bool split_idle;")
        params = params.replace("CONFIG_ZMK_SPLIT_BLE_IDLE_INT", "idle_int")
        _compile_and_run(SWEEP.replace("/* ACTUAL_PARAMS */", "static int idle_int;\n" + params),
                         "sweep")

    def test_nothing_is_compiled_with_the_option_off(self):
        """With CONFIG_ZMK_SPLIT_BLE_IDLE_INT=0 (the default) central.c is unchanged."""
        source = (Path(__file__).resolve().parents[3] / "src/split/bluetooth/central.c").read_text()
        kept, depth, skipping = [], 0, 0
        for line in source.splitlines():
            stripped = line.strip()
            if stripped.startswith("#if"):
                depth += 1
                if not skipping and stripped == "#if CONFIG_ZMK_SPLIT_BLE_IDLE_INT > 0":
                    skipping = depth
                    continue
            elif stripped.startswith("#else") or stripped.startswith("#elif"):
                self.assertNotEqual(depth, skipping, "the idle blocks have no #else")
            elif stripped.startswith("#endif"):
                depth -= 1
                if skipping and depth < skipping:
                    skipping = 0
                    continue
            if not skipping:
                kept.append(line)
        off = "\n".join(kept)
        for token in ("split_idle", "split_conn_params", "split_set_idle", "activity",
                      "CONFIG_ZMK_SPLIT_BLE_IDLE"):
            self.assertNotIn(token, off)


if __name__ == "__main__":
    unittest.main()
