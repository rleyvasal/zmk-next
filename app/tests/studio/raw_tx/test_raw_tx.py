"""Native regression tests for Studio raw frames (send and receive); no USB hardware needed."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


HARNESS = r"""
#include <assert.h>
#define IS_ENABLED(option) option
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define K_NO_WAIT 0
#define K_FOREVER -1
#define FRAMING_SOF 0xab
#define FRAMING_ESC 0xac
#define FRAMING_EOF 0xad
#define ZMK_RPC_RAW_FRAME_PREFIX_VALID(p)                                                         \
    (((p) & 0x07) == 3 || ((p) & 0x07) == 4 || ((p) & 0x07) == 6 || ((p) & 0x07) == 7)
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
    assert(zmk_rpc_send_raw_frame(NULL, 1) == -EINVAL);
    assert(zmk_rpc_send_raw_frame(simple, 0) == -EINVAL);
    assert(!locked && rpc_tx_buf.count == 0);

    /* 'B' (wire type 2) could start a protobuf message, so it is refused. */
    const uint8_t protobuf_like[] = {'B', 'x'};
    assert(zmk_rpc_send_raw_frame(protobuf_like, sizeof(protobuf_like)) == -EINVAL);
    assert(!locked && rpc_tx_buf.count == 0 && notifications == 0);

    reset(); locked = true;
    assert(zmk_rpc_send_raw_frame(simple, sizeof(simple)) == -EAGAIN);
    assert(locked && notifications == 0 && rpc_tx_buf.count == 0);

    reset(); selected_transport = NULL;
    assert(zmk_rpc_send_raw_frame(simple, sizeof(simple)) == -ENOTCONN);
    assert(!locked && rpc_tx_buf.count == 0);

    reset(); memset(rpc_tx_buf.data, 0x55, sizeof(rpc_tx_buf.data));
    rpc_tx_buf.count = 29;
    assert(zmk_rpc_send_raw_frame(simple, sizeof(simple)) == -EAGAIN);
    assert(!locked && rpc_tx_buf.count == 29 && notifications == 0);
    for (size_t i = 0; i < sizeof(rpc_tx_buf.data); i++) { assert(rpc_tx_buf.data[i] == 0x55); }

    /* All escaped bytes exceed capacity, even though unescaped bytes fit. */
    reset(); uint8_t too_large[31]; memset(too_large, FRAMING_SOF, sizeof(too_large));
    assert(zmk_rpc_send_raw_frame(too_large, 16) == -EMSGSIZE);
    assert(zmk_rpc_send_raw_frame(too_large, sizeof(too_large)) == -EMSGSIZE);
    assert(!locked && rpc_tx_buf.count == 0 && notifications == 0);

    /* Exact fit, with the frame wrapping around the ring boundary. */
    reset(); rpc_tx_buf.head = 27; rpc_tx_buf.count = 24;
    assert(zmk_rpc_send_raw_frame(escaped, sizeof(escaped)) == 0);
    assert(!locked && rpc_tx_buf.count == 32 && notifications == 1 && notified_bytes == 8);
    for (size_t i = 0; i < sizeof(expected); i++) {
        assert(rpc_tx_buf.data[(27 + 24 + i) % 32] == expected[i]);
    }

    /* Stopped reader: thousands of rejected frames must return, not spin. */
    for (int i = 0; i < 10000; i++) {
        assert(zmk_rpc_send_raw_frame(simple, sizeof(simple)) == -EAGAIN);
    }
    assert(!locked && rpc_tx_buf.count == 32 && notifications == 1);
    rpc_tx_buf.head = 0; rpc_tx_buf.count = 0;
    assert(zmk_rpc_send_raw_frame(simple, sizeof(simple)) == 0);
    assert(!locked && notifications == 2 && notified_bytes == 4);
    const uint8_t plain[] = {FRAMING_SOF, 'L', 'x', FRAMING_EOF};
    assert(memcmp(rpc_tx_buf.data, plain, sizeof(plain)) == 0);
    return 0;
}
"""


RX_HARNESS = r"""
#include <assert.h>
#define IS_ENABLED(option) option
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define LOG_WRN(...)
#define LOG_ERR(...)
#define K_FOREVER -1
#define ZMK_RPC_RAW_FRAME_RX_MAX 32
#define ZMK_RPC_RAW_FRAME_PREFIX_VALID(p)                                                         \
    (((p) & 0x07) == 3 || ((p) & 0x07) == 4 || ((p) & 0x07) == 6 || ((p) & 0x07) == 7)
/* ACTUAL_FRAMING */
typedef struct { size_t bytes_left; } pb_istream_t;
struct ring_buf { uint8_t data[256]; size_t head, count; };
static struct ring_buf rpc_rx_buf;
static enum studio_framing_state rpc_framing_state;
static int sem_waits;
static uint32_t ring_buf_get_claim(struct ring_buf *r, uint8_t **data, uint32_t size) {
    uint32_t n = r->count < size ? (uint32_t)r->count : size;
    *data = &r->data[r->head];
    return n;
}
static void ring_buf_get_finish(struct ring_buf *r, uint32_t n) { r->head += n; r->count -= n; }
static void k_sem_take(int *sem, int timeout) { (void)sem; (void)timeout; sem_waits++; assert(0); }
static int rpc_rx_sem;
typedef void (*zmk_rpc_raw_frame_handler_func)(const uint8_t *payload, size_t len);
struct zmk_rpc_raw_frame_handler { uint8_t prefix; zmk_rpc_raw_frame_handler_func func; };
static uint8_t got[64];
static size_t got_len;
static int calls_c, calls_d;
static void on_c(const uint8_t *p, size_t n) { memcpy(got, p, n); got_len = n; calls_c++; }
static void on_d(const uint8_t *p, size_t n) { memcpy(got, p, n); got_len = n; calls_d++; }
static const struct zmk_rpc_raw_frame_handler handlers[] = {{'C', on_c}, {'D', on_d}};
#define STRUCT_SECTION_FOREACH(type, var)                                                          \
    for (const struct type *var = handlers; var < handlers + 2; var++)
/* ACTUAL_RECEIVER */
static void feed(const uint8_t *bytes, size_t n) {
    memset(&rpc_rx_buf, 0, sizeof(rpc_rx_buf));
    memcpy(rpc_rx_buf.data, bytes, n);
    rpc_rx_buf.count = n;
}
/* What rpc_main does for one frame: read until the decoder stops, then reset per-frame state. */
static size_t read_frame(uint8_t *out, size_t chunk) {
    pb_istream_t stream = {SIZE_MAX};
    size_t total = 0;
    for (;;) {
        uint32_t before = (uint32_t)rpc_rx_buf.count;
        bool more = rpc_read_cb(&stream, out + total, chunk);
        (void)before;
        if (!more) break;
        total += chunk;
    }
    rpc_framing_state = FRAMING_STATE_IDLE;
#if CONFIG_ZMK_STUDIO_RPC_RAW_FRAMES
    rpc_frame_started = false;
#endif
    return total;
}
#if CONFIG_ZMK_STUDIO_RPC_RAW_FRAMES
int main(void) {
    uint8_t out[64];

    /* 'C' '1': goes to the 'C' handler, never to the protobuf decoder. */
    const uint8_t ctl_on[] = {0xab, 'C', '1', 0xad};
    feed(ctl_on, sizeof(ctl_on));
    read_frame(out, 1);
    assert(rpc_raw_frame && rpc_raw_len == 2);
    rpc_raw_frame = false; dispatch_raw_frame();
    assert(calls_c == 1 && got_len == 2 && got[0] == 'C' && got[1] == '1');

    /* Read in chunks bigger than the frame, as nanopb may: still captured whole. */
    const uint8_t dump[] = {0xab, 'D', '1', 0xad};
    feed(dump, sizeof(dump));
    read_frame(out, 16);
    assert(rpc_raw_frame && rpc_raw_len == 2);
    rpc_raw_frame = false; dispatch_raw_frame();
    assert(calls_d == 1 && got[0] == 'D' && got[1] == '1');

    /* Escaped payload bytes are unescaped before the handler sees them. */
    const uint8_t esc[] = {0xab, 'C', 0xac, 0xab, 0xad};
    feed(esc, sizeof(esc));
    read_frame(out, 1);
    assert(rpc_raw_frame && rpc_raw_len == 2 && rpc_raw_buf[1] == 0xab);
    rpc_raw_frame = false;

    /* A protobuf request (first byte 0x08: field 1, varint) is not raw, even when a later byte
     * is a registered prefix and even when it arrives one byte per read. */
    const uint8_t req[] = {0xab, 0x08, 'C', 0x1a, 'D', 0xad};
    feed(req, sizeof(req));
    size_t n = read_frame(out, 1);
    assert(!rpc_raw_frame && n == 4 && out[0] == 0x08 && out[1] == 'C' && out[3] == 'D');

    /* An unregistered but valid prefix ('G') is left to the decoder too. */
    const uint8_t unknown[] = {0xab, 'G', '1', 0xad};
    feed(unknown, sizeof(unknown));
    n = read_frame(out, 1);
    assert(!rpc_raw_frame && n == 2 && out[0] == 'G');

    /* Oversized raw frames are truncated, not overflowed. */
    uint8_t big[2 + 40];
    big[0] = 0xab; big[1] = 'C';
    memset(big + 2, 'x', 39);
    big[41] = 0xad;
    feed(big, sizeof(big));
    read_frame(out, 1);
    assert(rpc_raw_frame && rpc_raw_len == ZMK_RPC_RAW_FRAME_RX_MAX);
    assert(sem_waits == 0);
    return 0;
}
#else
/* Option off: every frame, prefixed or not, goes to the protobuf decoder as before. */
int main(void) {
    uint8_t out[64];
    const uint8_t ctl_on[] = {0xab, 'C', '1', 0xad};
    feed(ctl_on, sizeof(ctl_on));
    size_t n = read_frame(out, 1);
    assert(n == 2 && out[0] == 'C' && out[1] == '1' && calls_c == 0 && calls_d == 0);
    const uint8_t req[] = {0xab, 0x08, 'C', 0xad};
    feed(req, sizeof(req));
    n = read_frame(out, 1);
    assert(n == 2 && out[0] == 0x08 && out[1] == 'C');
    assert(sem_waits == 0);
    return 0;
}
#endif
"""


def _compile_and_run(source, name, raw_frames=1):
    with tempfile.TemporaryDirectory() as directory:
        executable = str(Path(directory) / name)
        subprocess.run(
            [os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
             "-Wno-unused-parameter", "-Wno-unused-variable", "-Wno-sign-compare",
             f"-DCONFIG_ZMK_STUDIO_RPC_RAW_FRAMES={raw_frames}", "-x", "c", "-", "-o", executable],
            input=source, text=True, check=True, timeout=30,
        )
        subprocess.run([executable], check=True, timeout=3)


class RawTxTests(unittest.TestCase):
    def test_real_transmitter_never_waits_or_writes_partial_frames(self):
        app = Path(__file__).resolve().parents[3]
        source = (app / "src/studio/rpc.c").read_text()
        start = source.index("/* Device-to-host raw frames. */")
        end = source.index("\nstatic void rpc_main(", start)
        _compile_and_run(HARNESS.replace("/* ACTUAL_TRANSMITTER */", source[start:end]), "test_raw_tx")

    def test_transmitter_is_left_out_when_the_option_is_off(self):
        app = Path(__file__).resolve().parents[3]
        source = (app / "src/studio/rpc.c").read_text()
        start = source.index("/* Device-to-host raw frames. */")
        end = source.index("\nstatic void rpc_main(", start)
        out = subprocess.run(
            [os.environ.get("CC", "cc"), "-E", "-P", "-DIS_ENABLED(x)=x",
             "-DCONFIG_ZMK_STUDIO_RPC_RAW_FRAMES=0", "-x", "c", "-"],
            input=source[start:end], text=True, check=True, capture_output=True, timeout=30,
        ).stdout
        self.assertNotIn("zmk_rpc_send_raw_frame", out)


class RawRxTests(unittest.TestCase):
    def test_real_receiver_routes_raw_frames_and_leaves_rpc_alone(self):
        app = Path(__file__).resolve().parents[3]
        source = (app / "src/studio/rpc.c").read_text()
        start = source.index("/* Host-to-device raw frame state.")
        end = source.index("\nstatic pb_istream_t pb_istream_for_rx_ring_buf", start)
        framing = (app / "src/studio/msg_framing.c").read_text()
        framing = framing[framing.index("static bool process_byte_err_state"):]
        header = (app / "src/studio/msg_framing.h").read_text()
        header = header[header.index("enum studio_framing_state {"):]
        harness = RX_HARNESS.replace("/* ACTUAL_FRAMING */", header + framing)
        harness = harness.replace("/* ACTUAL_RECEIVER */", source[start:end])
        for raw_frames in (1, 0):
            with self.subTest(raw_frames=raw_frames):
                _compile_and_run(harness, f"test_raw_rx_{raw_frames}", raw_frames)


if __name__ == "__main__":
    unittest.main()
