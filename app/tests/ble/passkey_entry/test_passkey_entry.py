"""Exercise the actual passkey entry key handler with native mocks."""

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
#define LOG_DBG(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#define ZMK_EV_EVENT_BUBBLE 0
#define ZMK_EV_EVENT_HANDLED 1
#define PASSKEY_DIGITS 6
#define HID_USAGE_KEY_KEYBOARD_ESCAPE 0x29
#define HID_USAGE_KEY_KEYBOARD_RETURN_ENTER 0x28
#define HID_USAGE_KEY_KEYBOARD_RETURN 0x9e
#define HID_USAGE_KEY_KEYBOARD_DELETE_BACKSPACE 0x2a
#define HID_USAGE_KEY_KEYBOARD_1_AND_EXCLAMATION 0x1e
#define HID_USAGE_KEY_KEYBOARD_0_AND_RIGHT_PARENTHESIS 0x27
#define HID_USAGE_KEY_KEYPAD_1_AND_END 0x59
#define HID_USAGE_KEY_KEYPAD_0_AND_INSERT 0x62
typedef uint32_t zmk_key_t;
struct zmk_keycode_state_changed { zmk_key_t keycode; bool state; };
struct bt_conn { int refs; };
static struct bt_conn host = {1};
static struct bt_conn *auth_passkey_entry_conn;
static uint8_t passkey_entries[PASSKEY_DIGITS];
static uint8_t passkey_entry_count;
static int submitted = -1, cancels, submit_err;
static void bt_conn_unref(struct bt_conn *conn) { assert(conn->refs > 1); conn->refs--; }
static int bt_conn_auth_passkey_entry(struct bt_conn *conn, unsigned int passkey) {
    assert(conn == &host); if (submit_err) return submit_err; submitted = (int)passkey; return 0;
}
static int bt_conn_auth_cancel(struct bt_conn *conn) { assert(conn == &host); cancels++; return 0; }
/* ACTUAL_NUMERIC */
/* ACTUAL_HANDLER */
static int key(zmk_key_t k) {
    struct zmk_keycode_state_changed press = {k, true}, release = {k, false};
    assert(zmk_ble_handle_key_user(&press) != ZMK_EV_EVENT_BUBBLE || !auth_passkey_entry_conn);
    return zmk_ble_handle_key_user(&release);
}
static zmk_key_t digit(int d) {
    return d == 0 ? HID_USAGE_KEY_KEYBOARD_0_AND_RIGHT_PARENTHESIS
                  : HID_USAGE_KEY_KEYBOARD_1_AND_EXCLAMATION + d - 1;
}
static void start(void) { host.refs = 2; auth_passkey_entry_conn = &host; passkey_entry_count = 0; }
int main(void) {
    /* No entry in progress: keys go to the keymap as usual. */
    assert(key(digit(1)) == ZMK_EV_EVENT_BUBBLE);

    /* Enter with too few digits waits for the rest instead of submitting. */
    start();
    key(digit(0)); key(digit(6));
    assert(key(HID_USAGE_KEY_KEYBOARD_RETURN_ENTER) == ZMK_EV_EVENT_HANDLED);
    assert(submitted == -1 && auth_passkey_entry_conn == &host && passkey_entry_count == 2);

    /* Backspace corrects a digit; a seventh digit is ignored. */
    key(digit(0)); key(digit(9));
    key(HID_USAGE_KEY_KEYBOARD_DELETE_BACKSPACE);
    assert(passkey_entry_count == 3);
    key(digit(6)); key(digit(0)); key(HID_USAGE_KEY_KEYPAD_1_AND_END + 5);
    key(digit(1));
    assert(passkey_entry_count == 6);
    assert(key(HID_USAGE_KEY_KEYBOARD_RETURN) == ZMK_EV_EVENT_HANDLED);
    assert(submitted == 60606 && auth_passkey_entry_conn == NULL && host.refs == 1);

    /* Backspace on an empty entry does nothing. */
    start();
    key(HID_USAGE_KEY_KEYBOARD_DELETE_BACKSPACE);
    assert(passkey_entry_count == 0);

    /* A failed submit keeps the entry so it can be retried. */
    for (int i = 0; i < 6; i++) key(digit(1));
    submit_err = -22; submitted = -1;
    key(HID_USAGE_KEY_KEYBOARD_RETURN_ENTER);
    assert(submitted == -1 && auth_passkey_entry_conn == &host && passkey_entry_count == 6);
    submit_err = 0;
    key(HID_USAGE_KEY_KEYBOARD_RETURN_ENTER);
    assert(submitted == 111111 && auth_passkey_entry_conn == NULL);

    /* Escape cancels pairing; the cancel callback releases the entry. */
    start();
    key(HID_USAGE_KEY_KEYBOARD_ESCAPE);
    assert(cancels == 1);
    return 0;
}
"""


def _function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}\n", start) + 3]


class PasskeyEntryTests(unittest.TestCase):
    def test_six_digits_backspace_and_retry(self):
        source = (Path(__file__).resolve().parents[3] / "src/ble.c").read_text()
        numeric = _function(source, "static bool zmk_ble_numeric_usage_to_value(")
        handler = _function(source, "static bool zmk_ble_submit_passkey(void) {") + _function(
            source, "static int zmk_ble_handle_key_user(")
        harness = HARNESS.replace("/* ACTUAL_NUMERIC */", numeric).replace(
            "/* ACTUAL_HANDLER */", handler)
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "test_passkey_entry")
            subprocess.run(
                [os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                 "-x", "c", "-", "-o", executable],
                input=harness, text=True, check=True, timeout=30,
            )
            subprocess.run([executable], check=True, timeout=3)


if __name__ == "__main__":
    unittest.main()
