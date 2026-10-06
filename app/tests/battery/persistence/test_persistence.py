"""Exercise the real battery USB listener while startup owns settings."""

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
#include <string.h>
#define IS_ENABLED(x) x
#define CONFIG_SETTINGS 1
#define CONFIG_ZMK_USB 1
#define LOG_WRN(...) ((void)0)
#define ARG_UNUSED(x) ((void)(x))
#define ENOTSUP 95
struct k_work { void (*handler)(struct k_work *); bool queued; };
struct k_work_q { int unused; };
#define K_WORK_DEFINE(name, callback) struct k_work name = { .handler = callback }
typedef int zmk_event_t;
enum { ZMK_ACTIVITY_ACTIVE, ZMK_ACTIVITY_IDLE, ZMK_ACTIVITY_SLEEP };
static bool powered, settings_locked, in_usb_event;
static int saves, blocked_system_saves, battery_updates, timer_stops;
static uint8_t saved_soc;
static struct k_work_q lowprio;
static struct k_work *pending;
static uint8_t last_cell_soc;
static bool have_last_cell;
static bool zmk_usb_is_powered(void) { return powered; }
static struct k_work_q *zmk_workqueue_lowprio_work_q(void) { return &lowprio; }
static int k_work_submit_to_queue(struct k_work_q *queue, struct k_work *work) {
    assert(queue == &lowprio);
    assert(pending == NULL || pending == work);
    pending = work;
    work->queued = true;
    return 1;
}
static int settings_save_one(const char *key, const void *value, size_t size) {
    assert(strcmp(key, "zmk/batt/cell") == 0 && size == 1);
    if (settings_locked && in_usb_event) {
        /* This is the wait cycle: main holds settings and awaits HCI on the
         * system queue, whose USB listener is now waiting for settings. */
        blocked_system_saves++;
        return -1;
    }
    assert(!settings_locked && !in_usb_event);
    saves++;
    saved_soc = *(const uint8_t *)value;
    return 0;
}
static const int *as_zmk_usb_conn_state_changed(const int *event) {
    return *event == 1 ? event : NULL;
}
static const int *as_zmk_activity_state_changed(const int *event) {
    return *event == 2 ? event : NULL;
}
static int zmk_activity_get_state(void) { return ZMK_ACTIVITY_IDLE; }
static void zmk_battery_start_reporting(void) { }
static int battery_timer;
static void k_timer_stop(int *timer) { assert(timer == &battery_timer); timer_stops++; }
static void update_battery(struct k_work *work) { (void)work; battery_updates++; }
static K_WORK_DEFINE(battery_work, update_battery);
/* ACTUAL_PERSIST */
/* ACTUAL_LISTENER */
static void usb_event(void) {
    const int event = 1;
    in_usb_event = true;
    assert(battery_event_listener(&event) == 0);
    in_usb_event = false;
}
static void drain_lowprio(void) {
    if (pending) {
        struct k_work *work = pending;
        pending = NULL;
        work->queued = false;
        work->handler(work);
    }
}
int main(void) {
    powered = true;
    have_last_cell = true;
    last_cell_soc = 72;
    settings_locked = true;
    usb_event();
    usb_event(); /* Repeated enumeration events coalesce. */
    if (blocked_system_saves) {
        fprintf(stderr, "USB battery save blocks the system queue while main owns settings\n");
        return 1;
    }
    assert(saves == 0);
    /* The USB listener returned, so HCI work can run and main can finish load. */
    settings_locked = false;
    drain_lowprio();
    assert(saves == 1 && saved_soc == 72);

    last_cell_soc = 65;
    usb_event();
    assert(saves == 1);
    drain_lowprio();
    assert(saves == 2 && saved_soc == 65);

    have_last_cell = false;
    usb_event();
    drain_lowprio();
    assert(saves == 2); /* Never persist an unknown/zero default reading. */

    powered = false;
    usb_event();
    drain_lowprio();
    assert(battery_updates == 1 && saves == 2);
    const int idle_event = 2;
    assert(battery_event_listener(&idle_event) == 0 && timer_stops == 1);
    return 0;
}
"""


class BatteryPersistenceTests(unittest.TestCase):
    def test_usb_event_leaves_system_queue_free_during_settings_load(self):
        source = (Path(__file__).resolve().parents[3] / "src/battery.c").read_text()
        start = source.index("static void persist_cell_soc(")
        end = source.index("static int batt_settings_set(", start)
        listener_start = source.index("static int battery_event_listener(")
        listener_end = source.index("\nZMK_LISTENER(", listener_start)
        harness = HARNESS.replace("/* ACTUAL_PERSIST */", source[start:end])
        harness = harness.replace("/* ACTUAL_LISTENER */", source[listener_start:listener_end])
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "test_battery_persistence")
            subprocess.run(
                [os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                 "-x", "c", "-", "-o", executable],
                input=harness, text=True, check=True, timeout=30,
            )
            subprocess.run([executable], check=True, timeout=3)


if __name__ == "__main__":
    unittest.main()
