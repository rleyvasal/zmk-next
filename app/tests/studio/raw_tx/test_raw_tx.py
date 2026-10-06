"""Native regression test for the real diagnostic transmitter; no USB hardware needed."""

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
#define K_NO_WAIT 0
#define K_FOREVER -1
#define FRAMING_SOF 0xab
#define FRAMING_ESC 0xac
#define FRAMING_EOF 0xad
struct ring_buf { uint8_t data[32]; size_t head, count; };
static struct ring_buf rpc_tx_buf;
static int rpc_transport_mutex, notifications;
static size_t notified_bytes;
static bool locked;
static int k_mutex_lock(int *mutex, int timeout) {
    (void)mutex;
    assert(timeout == K_NO_WAIT);
    if (locked) { return -EBUSY; }
    locked = true;
    return 0;
}
static void k_mutex_unlock(int *mutex) { (void)mutex; assert(locked); locked = false; }
static size_t ring_buf_capacity_get(struct ring_buf *ring) { return sizeof(ring->data); }
static size_t ring_buf_space_get(struct ring_buf *ring) { return sizeof(ring->data) - ring->count; }
static size_t ring_buf_put(struct ring_buf *ring, const uint8_t *data, size_t len) {
    assert(len <= ring_buf_space_get(ring));
    for (size_t i = 0; i < len; i++) {
        ring->data[(ring->head + ring->count++) % sizeof(ring->data)] = data[i];
    }
    return len;
}
static void notify(struct ring_buf *ring, size_t written, bool done, void *user) {
    assert(ring == &rpc_tx_buf && done && user == NULL);
    notifications++;
    notified_bytes = written;
}
struct transport {
    void *(*tx_user_data)(void);
    void (*tx_notify)(struct ring_buf *, size_t, bool, void *);
};
static struct transport uart_transport = { .tx_notify = notify };
static struct transport *selected_transport = &uart_transport;
/* ACTUAL_TRANSMITTER */
static void reset(void) {
    memset(&rpc_tx_buf, 0, sizeof(rpc_tx_buf));
    notifications = 0; notified_bytes = 0; locked = false;
    selected_transport = &uart_transport;
}
int main(void) {
    const uint8_t simple[] = {'L', 'x'};
    const uint8_t escaped[] = {FRAMING_SOF, FRAMING_ESC, FRAMING_EOF};
    const uint8_t expected[] = {0xab, 0xac, 0xab, 0xac, 0xac, 0xac, 0xad, 0xad};
    reset();
    assert(zmk_rpc_tx_raw_payload(NULL, 1) == -EINVAL);
    assert(zmk_rpc_tx_raw_payload(simple, 0) == -EINVAL);
    assert(!locked && rpc_tx_buf.count == 0);

    reset(); locked = true;
    assert(zmk_rpc_tx_raw_payload(simple, sizeof(simple)) == -EAGAIN);
    assert(locked && notifications == 0 && rpc_tx_buf.count == 0);

    reset(); selected_transport = NULL;
    assert(zmk_rpc_tx_raw_payload(simple, sizeof(simple)) == -ENOTCONN);
    assert(!locked && rpc_tx_buf.count == 0);

    reset(); memset(rpc_tx_buf.data, 0x55, sizeof(rpc_tx_buf.data));
    rpc_tx_buf.count = 29;
    assert(zmk_rpc_tx_raw_payload(simple, sizeof(simple)) == -EAGAIN);
    assert(!locked && rpc_tx_buf.count == 29 && notifications == 0);
    for (size_t i = 0; i < sizeof(rpc_tx_buf.data); i++) { assert(rpc_tx_buf.data[i] == 0x55); }

    /* All escaped bytes exceed capacity, even though unescaped bytes fit. */
    reset(); uint8_t too_large[31]; memset(too_large, FRAMING_SOF, sizeof(too_large));
    assert(zmk_rpc_tx_raw_payload(too_large, 16) == -EMSGSIZE);
    assert(zmk_rpc_tx_raw_payload(too_large, sizeof(too_large)) == -EMSGSIZE);
    assert(!locked && rpc_tx_buf.count == 0 && notifications == 0);

    /* Exact fit, with the frame wrapping around the ring boundary. */
    reset(); rpc_tx_buf.head = 27; rpc_tx_buf.count = 24;
    assert(zmk_rpc_tx_raw_payload(escaped, sizeof(escaped)) == 0);
    assert(!locked && rpc_tx_buf.count == 32 && notifications == 1 && notified_bytes == 8);
    for (size_t i = 0; i < sizeof(expected); i++) {
        assert(rpc_tx_buf.data[(27 + 24 + i) % 32] == expected[i]);
    }

    /* Stopped reader: thousands of rejected frames must return, not spin. */
    for (int i = 0; i < 10000; i++) {
        assert(zmk_rpc_tx_raw_payload(simple, sizeof(simple)) == -EAGAIN);
    }
    assert(!locked && rpc_tx_buf.count == 32 && notifications == 1);
    rpc_tx_buf.head = 0; rpc_tx_buf.count = 0;
    assert(zmk_rpc_tx_raw_payload(simple, sizeof(simple)) == 0);
    assert(!locked && notifications == 2 && notified_bytes == 4);
    const uint8_t plain[] = {FRAMING_SOF, 'L', 'x', FRAMING_EOF};
    assert(memcmp(rpc_tx_buf.data, plain, sizeof(plain)) == 0);
    return 0;
}
"""


class RawTxTests(unittest.TestCase):
    def test_real_transmitter_never_waits_or_writes_partial_frames(self):
        app = Path(__file__).resolve().parents[3]
        source = (app / "src/studio/rpc.c").read_text()
        start = source.index("int zmk_rpc_tx_raw_payload(")
        end = source.index("\nstatic void rpc_main(", start)
        harness = HARNESS.replace("/* ACTUAL_TRANSMITTER */", source[start:end])
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "test_raw_tx")
            subprocess.run(
                [os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror", "-x", "c", "-", "-o", executable],
                input=harness, text=True, check=True, timeout=30,
            )
            subprocess.run([executable], check=True, timeout=3)


if __name__ == "__main__":
    unittest.main()
