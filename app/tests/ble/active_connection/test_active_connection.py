"""Exercise the actual active-profile lookup with native connection mocks."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


HARNESS = r"""
#include <assert.h>
#include <stddef.h>
#include <stdbool.h>
#define BT_CONN_ROLE_PERIPHERAL 1
#define BT_CONN_STATE_CONNECTED 1
#define BT_CONN_TYPE_LE 1
#define BT_ID_DEFAULT 0
#define LOG_WRN(...) ((void)0)
typedef int bt_addr_le_t;
static const bt_addr_le_t any = -1;
#define BT_ADDR_LE_ANY (&any)
struct bt_conn { int role, state, error, profile, refs; };
struct bt_conn_info { int role, state; };
static int active_profile = 2, address = 2, scans;
static struct bt_conn *lookup, *peers[4];
static bt_addr_le_t *zmk_ble_active_profile_addr(void) { return &address; }
static int bt_addr_le_cmp(const bt_addr_le_t *a, const bt_addr_le_t *b) { return *a != *b; }
static struct bt_conn *bt_conn_ref(struct bt_conn *conn) { conn->refs++; return conn; }
static void bt_conn_unref(struct bt_conn *conn) { assert(conn->refs > 1); conn->refs--; }
static struct bt_conn *bt_conn_lookup_addr_le(int id, const bt_addr_le_t *addr) {
    assert(id == 0 && *addr == address); return lookup ? bt_conn_ref(lookup) : NULL;
}
static int bt_conn_get_info(struct bt_conn *conn, struct bt_conn_info *info) {
    info->role = conn->role; info->state = conn->state; return conn->error;
}
static const int *bt_conn_get_dst(struct bt_conn *conn) { return &conn->profile; }
static int zmk_ble_profile_index(const int *addr) { return *addr; }
static void bt_conn_foreach(int type, void (*callback)(struct bt_conn *, void *), void *data) {
    assert(type == BT_CONN_TYPE_LE); scans++;
    for (int i = 0; i < 4; i++) { if (peers[i]) callback(peers[i], data); }
}
/* ACTUAL_LOOKUP */
int main(void) {
    struct bt_conn host = {1, 1, 0, 2, 1};
    lookup = &host;
    assert(zmk_ble_active_profile_conn() == &host && host.refs == 2 && scans == 0);
    bt_conn_unref(&host);
    address = -1;
    assert(zmk_ble_active_profile_conn() == NULL && host.refs == 1 && scans == 0);
    address = 2;

    struct bt_conn live = {1, 1, 0, 2, 1};
    struct bt_conn split = {0, 1, 0, 2, 1};
    struct bt_conn other = {1, 1, 0, 0, 1};
    struct bt_conn later = {1, 1, 0, 2, 1};
    peers[0] = &split; peers[1] = &other; peers[2] = &live; peers[3] = &later;
    /* Connecting, disconnecting, wrong role, and failed info lookup. */
    for (int mode = 0; mode < 4; mode++) {
        host.state = mode == 0 ? 0 : mode == 1 ? 2 : 1;
        host.role = mode == 2 ? 0 : 1; host.error = mode == 3 ? -1 : 0;
        assert(zmk_ble_active_profile_conn() == &live);
        assert(host.refs == 1 && live.refs == 2);
        assert(split.refs == 1 && other.refs == 1 && later.refs == 1);
        bt_conn_unref(&live);
    }
    lookup = NULL;
    assert(zmk_ble_active_profile_conn() == &live && live.refs == 2);
    bt_conn_unref(&live);
    live.state = 2; later.error = -1;
    assert(zmk_ble_active_profile_conn() == NULL && live.refs == 1 && later.refs == 1);
    lookup = &host;
    assert(zmk_ble_active_profile_conn() == NULL && host.refs == 1);
    return 0;
}
"""


class ActiveConnectionTests(unittest.TestCase):
    def test_stale_lookup_falls_back_without_leaking_references(self):
        source = (Path(__file__).resolve().parents[3] / "src/ble.c").read_text()
        start = source.index("static void active_profile_conn_foreach(")
        end = source.index("\nchar *zmk_ble_active_profile_name(", start)
        harness = HARNESS.replace("/* ACTUAL_LOOKUP */", source[start:end])
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "test_active_connection")
            subprocess.run(
                [os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                 "-x", "c", "-", "-o", executable],
                input=harness, text=True, check=True, timeout=30,
            )
            subprocess.run([executable], check=True, timeout=3)


if __name__ == "__main__":
    unittest.main()
