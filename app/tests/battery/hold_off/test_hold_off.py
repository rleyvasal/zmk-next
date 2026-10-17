"""Without CONFIG_ZMK_BATTERY_HOLD_ON_EXTERNAL_POWER, battery.c reports live readings as before."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


# Settings and USB are enabled but have no mocks: if any hold code that uses them
# were still compiled with the option off, the harness would not build.
HARNESS = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define IS_ENABLED(x) x
#define CONFIG_ZMK_BATTERY_HOLD_ON_EXTERNAL_POWER 0
#define CONFIG_SETTINGS 1
#define CONFIG_ZMK_USB 1
#define CONFIG_HAS_NRFX 1
#define CONFIG_BT_BAS 1
#define CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_PROXY_SWAP 0
#define CONFIG_ZMK_BATTERY_CHARGE_STATUS 0
#define CONFIG_ZMK_BATTERY_SETTLE_AFTER_CHARGE_SEC 0
#define CONFIG_ZMK_BATTERY_REPORT_INTERVAL 60
#define CONFIG_ZMK_BATTERY_REPORTING_FETCH_MODE_STATE_OF_CHARGE 0
#define CONFIG_ZMK_BATTERY_REPORTING_FETCH_MODE_LITHIUM_VOLTAGE 1
#define CONFIG_APPLICATION_INIT_PRIORITY 90
#define DT_HAS_CHOSEN(node) DT_HAS_CHOSEN_##node
#define DT_HAS_CHOSEN_zmk_battery 1
#define DT_HAS_CHOSEN_zmk_battery_charger 0
#define DT_CHOSEN(node) 0
#define DEVICE_DT_GET(node) (&battery_dev)
#define ARG_UNUSED(x) ((void)(x))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define LOG_DBG(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define LOG_ERR(...) ((void)0)

enum zmk_battery_charge_state {
    ZMK_BATTERY_CHARGE_STATE_DISCHARGING,
    ZMK_BATTERY_CHARGE_STATE_CHARGING,
    ZMK_BATTERY_CHARGE_STATE_FULL,
};

struct device { const char *name; };
static const struct device battery_dev = {"vbatt"};
static bool device_is_ready(const struct device *dev) { return dev == &battery_dev; }

enum sensor_channel { SENSOR_CHAN_VOLTAGE, SENSOR_CHAN_GAUGE_STATE_OF_CHARGE };
struct sensor_value { int val1, val2; };
static int sample_mv;
static int sensor_sample_fetch_chan(const struct device *dev, enum sensor_channel chan) {
    (void)dev; assert(chan == SENSOR_CHAN_VOLTAGE); return 0;
}
static int sensor_channel_get(const struct device *dev, enum sensor_channel chan,
                              struct sensor_value *val) {
    (void)dev; (void)chan;
    val->val1 = sample_mv / 1000; val->val2 = (sample_mv % 1000) * 1000;
    return 0;
}

struct zmk_battery_state_changed { uint8_t state_of_charge; };
static int events, last_event_soc = -1;
static int raise_zmk_battery_state_changed(struct zmk_battery_state_changed ev) {
    events++; last_event_soc = ev.state_of_charge; return 0;
}

static uint8_t bas_level = 0xff;
static int bas_sets;
static uint8_t bt_bas_get_battery_level(void) { return bas_level; }
static int bt_bas_set_battery_level(uint8_t level) { bas_level = level; bas_sets++; return 0; }

struct k_work { void (*handler)(struct k_work *); };
#define K_WORK_DEFINE(name, fn) struct k_work name = {fn}
struct k_work_q { int unused; };
static struct k_work_q lowprio_q;
static struct k_work_q *zmk_workqueue_lowprio_work_q(void) { return &lowprio_q; }
static int k_work_submit_to_queue(struct k_work_q *q, struct k_work *work) {
    (void)q; work->handler(work); return 0;
}
struct k_timer { void (*expiry)(struct k_timer *); bool running; };
#define K_TIMER_DEFINE(name, fn, stop) struct k_timer name = {fn, false}
#define K_NO_WAIT 0
#define K_SECONDS(s) (s)
static void k_timer_start(struct k_timer *t, int delay, int period) {
    (void)delay; (void)period; t->running = true;
}
static void k_timer_stop(struct k_timer *t) { t->running = false; }

enum zmk_activity_state { ZMK_ACTIVITY_ACTIVE, ZMK_ACTIVITY_IDLE, ZMK_ACTIVITY_SLEEP };
static enum zmk_activity_state activity = ZMK_ACTIVITY_ACTIVE;
static enum zmk_activity_state zmk_activity_get_state(void) { return activity; }
typedef struct { int type; } zmk_event_t;
static const zmk_event_t activity_event = {1};
static const void *as_zmk_activity_state_changed(const zmk_event_t *eh) {
    return eh == &activity_event ? eh : NULL;
}
#define ZMK_LISTENER(name, cb) static int (*const name##_cb)(const zmk_event_t *) = cb
#define ZMK_SUBSCRIPTION(name, ev) static const int name##_##ev##_sub = 1
#define SYS_INIT(fn, level, prio) static int (*const init_fn)(void) = fn
/* ACTUAL_BATTERY */
static void sample(int mv) {
    sample_mv = mv;
    battery_timer.expiry(&battery_timer);
}
int main(void) {
    (void)battery_cb; (void)battery_zmk_activity_state_changed_sub;
    assert(init_fn() == 0 && battery_timer.running);

    /* Charge voltage on USB: reported, sent to the BAS and raised as is. */
    sample(4210);
    assert(zmk_battery_state_of_charge() == 100 && zmk_battery_reported_soc() == 100);
    assert(bas_level == 100 && events == 1 && last_event_soc == 100);
    assert(zmk_battery_last_cell_soc() == -1);
    assert(zmk_battery_charge_state() == ZMK_BATTERY_CHARGE_STATE_DISCHARGING);
    assert(zmk_battery_millivolts() == 4210);

    /* No change: no event and no BAS write. */
    sample(4210);
    assert(events == 1 && bas_sets == 1);

    /* Unplugged: the live reading straight away, with no settling hold. */
    sample(3900);
    assert(zmk_battery_reported_soc() == 61 && bas_level == 61 && events == 2);

    /* Idle stops sampling, as upstream does. */
    activity = ZMK_ACTIVITY_IDLE;
    assert(battery_cb(&activity_event) == 0 && !battery_timer.running);
    activity = ZMK_ACTIVITY_ACTIVE;
    assert(battery_cb(&activity_event) == 0 && battery_timer.running);
    return 0;
}
"""


class HoldOffTests(unittest.TestCase):
    def test_live_readings_and_no_persistence_without_the_option(self):
        source = (Path(__file__).resolve().parents[3] / "src/battery.c").read_text()
        start = source.index("/* This half publishes its own level")
        body = source[start:].replace("static void zmk_battery_timer(", "void zmk_battery_timer(", 1)
        harness = HARNESS.replace("/* ACTUAL_BATTERY */", body)
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "test_hold_off")
            subprocess.run(
                [os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                 "-Wno-unused-parameter", "-Wno-unused-function", "-Wno-unused-const-variable",
                 "-x", "c", "-", "-o", executable],
                input=harness, text=True, check=True, timeout=30,
            )
            subprocess.run([executable], check=True, timeout=3)


if __name__ == "__main__":
    unittest.main()
