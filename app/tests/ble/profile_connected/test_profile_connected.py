"""Exercise the actual profile connection check with native connection mocks."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


HARNESS = r"""
#include <assert.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#define BT_CONN_ROLE_PERIPHERAL 1
#define BT_CONN_STATE_CONNECTED 1
#define BT_CONN_TYPE_LE 1
#define BT_ID_DEFAULT 0
#define ZMK_BLE_PROFILE_COUNT 3
typedef int bt_addr_le_t;
static const bt_addr_le_t any = -1;
#define BT_ADDR_LE_ANY (&any)
struct bt_conn { int role, state, error, profile, refs; };
struct bt_conn_info { int role, state; };
struct zmk_ble_profile { bt_addr_le_t peer; };
static struct zmk_ble_profile profiles[ZMK_BLE_PROFILE_COUNT] = {{0}, {1}, {-1}};
static struct bt_conn *lookup, *peers[3];
static int bt_addr_le_cmp(const bt_addr_le_t *a, const bt_addr_le_t *b) { return *a != *b; }
static struct bt_conn *bt_conn_lookup_addr_le(int id, const bt_addr_le_t *addr) {
    assert(id == 0);
    if (lookup && lookup->profile == *addr) { lookup->refs++; return lookup; }
    return NULL;
}
static void bt_conn_unref(struct bt_conn *conn) { assert(conn->refs > 1); conn->refs--; }
static int bt_conn_get_info(struct bt_conn *conn, struct bt_conn_info *info) {
    info->role = conn->role; info->state = conn->state; return conn->error;
}
static const int *bt_conn_get_dst(struct bt_conn *conn) { return &conn->profile; }
static int zmk_ble_profile_index(const int *addr) { return *addr; }
static void bt_conn_foreach(int type, void (*callback)(struct bt_conn *, void *), void *data) {
    assert(type == BT_CONN_TYPE_LE);
    for (int i = 0; i < 3; i++) { if (peers[i]) callback(peers[i], data); }
}
/* ACTUAL_CHECK */
int main(void) {
    struct bt_conn host = {1, 1, 0, 1, 1};
    lookup = &host;
    assert(zmk_ble_profile_is_connected(1) && host.refs == 1);
    assert(!zmk_ble_profile_is_connected(0));
    assert(!zmk_ble_profile_is_connected(2)); /* open profile */
    assert(!zmk_ble_profile_is_connected(3)); /* out of range */

    /* The identity lookup returns a stale object: connecting, disconnecting,
     * wrong role, or failed info. Without a live RPA connection it is false. */
    for (int mode = 0; mode < 4; mode++) {
        host.state = mode == 0 ? 0 : mode == 1 ? 2 : 1;
        host.role = mode == 2 ? 0 : 1; host.error = mode == 3 ? -1 : 0;
        assert(!zmk_ble_profile_is_connected(1) && host.refs == 1);
    }

    /* A live connection resolving to the profile counts; split or other
     * profiles' connections do not. */
    struct bt_conn split = {0, 1, 0, 1, 1};
    struct bt_conn other = {1, 1, 0, 0, 1};
    struct bt_conn live = {1, 1, 0, 1, 1};
    peers[0] = &split; peers[1] = &other;
    assert(!zmk_ble_profile_is_connected(1));
    peers[2] = &live;
    assert(zmk_ble_profile_is_connected(1) && host.refs == 1 && live.refs == 1);
    live.state = 2;
    assert(!zmk_ble_profile_is_connected(1));
    return 0;
}
"""


class ProfileConnectedTests(unittest.TestCase):
    def test_stale_lookup_falls_back_to_live_connection(self):
        source = (Path(__file__).resolve().parents[3] / "src/ble.c").read_text()
        start = source.index("static void profile_connected_foreach(")
        end = source.index("\n}\n", source.index("bool zmk_ble_profile_is_connected(")) + 3
        harness = HARNESS.replace("/* ACTUAL_CHECK */", source[start:end])
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "test_profile_connected")
            subprocess.run(
                [os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                 "-x", "c", "-", "-o", executable],
                input=harness, text=True, check=True, timeout=30,
            )
            subprocess.run([executable], check=True, timeout=3)


if __name__ == "__main__":
    unittest.main()
