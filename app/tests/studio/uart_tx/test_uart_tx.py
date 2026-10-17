"""Exercise the actual Studio UART interrupt handler's TX path with native mocks."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define LOG_ERR(...) ((void)0)
#define MAX(a, b) ((a) > (b) ? (a) : (b))
struct device { int unused; };
struct ring_buf { uint8_t data[64]; uint32_t head, tail, size; };
static struct device dev;
static const struct device *const uart_dev = &dev;
static struct ring_buf tx, rx = {.size = 64};
static int fifo_room, fills, tx_disabled;
static uint8_t sent_bytes[64];
static uint32_t sent_len;
static uint32_t ring_buf_size_get(struct ring_buf *buf) { return buf->tail - buf->head; }
static bool ring_buf_is_empty(struct ring_buf *buf) { return ring_buf_size_get(buf) == 0; }
static uint32_t ring_buf_get_claim(struct ring_buf *buf, uint8_t **data, uint32_t size) {
    uint32_t len = ring_buf_size_get(buf);
    *data = &buf->data[buf->head];
    return len < size ? len : size;
}
static int ring_buf_get_finish(struct ring_buf *buf, uint32_t size) {
    assert(size <= ring_buf_size_get(buf));
    buf->head += size;
    return 0;
}
static uint32_t ring_buf_put_claim(struct ring_buf *buf, uint8_t **data, uint32_t size) {
    (void)buf; (void)data; (void)size; return 0;
}
static int ring_buf_put_finish(struct ring_buf *buf, uint32_t size) { (void)buf; (void)size; return 0; }
static struct ring_buf *zmk_rpc_get_rx_buf(void) { return &rx; }
static struct ring_buf *zmk_rpc_get_tx_buf(void) { return &tx; }
static void zmk_rpc_rx_notify(void) {}
static int uart_irq_update(const struct device *d) { (void)d; return 1; }
static int uart_irq_rx_ready(const struct device *d) { (void)d; return 0; }
static int uart_irq_tx_ready(const struct device *d) { (void)d; return 1; }
static int uart_fifo_read(const struct device *d, uint8_t *b, int len) {
    (void)d; (void)b; (void)len; return 0;
}
static void uart_irq_tx_disable(const struct device *d) { (void)d; tx_disabled++; }
static int uart_fifo_fill(const struct device *d, const uint8_t *b, int len) {
    (void)d;
    /* A fill after the host stopped draining would loop forever in the ISR. */
    assert(++fills < 16);
    int n = len < fifo_room ? len : fifo_room;
    memcpy(&sent_bytes[sent_len], b, n);
    sent_len += n; fifo_room -= n;
    return n;
}
/* ACTUAL_SERIAL_CB */
static void queue(const char *s) {
    memset(&tx, 0, sizeof(tx)); tx.size = 64;
    tx.tail = strlen(s); memcpy(tx.data, s, tx.tail);
    fills = tx_disabled = 0; sent_len = 0;
}
int main(void) {
    /* Room for everything: all bytes go out and TX interrupts stop. */
    queue("hello"); fifo_room = 64;
    serial_cb(&dev, NULL);
    assert(sent_len == 5 && !memcmp(sent_bytes, "hello", 5) && tx_disabled == 1);

    /* Host not draining: return with the bytes kept and TX interrupts still on. */
    queue("hello"); fifo_room = 2;
    serial_cb(&dev, NULL);
    assert(sent_len == 2 && ring_buf_size_get(&tx) == 3 && tx_disabled == 0);
    /* The next TX-ready interrupt sends the rest. */
    fills = 0; fifo_room = 64;
    serial_cb(&dev, NULL);
    assert(sent_len == 5 && !memcmp(sent_bytes, "hello", 5) && tx_disabled == 1);

    /* An error from the driver keeps the bytes. */
    queue("x"); fifo_room = 0;
    serial_cb(&dev, NULL);
    assert(ring_buf_size_get(&tx) == 1 && tx_disabled == 0);
    return 0;
}
"""


class UartTxTests(unittest.TestCase):
    def test_tx_stops_when_host_is_not_draining(self):
        source = (Path(__file__).resolve().parents[3] / "src/studio/uart_rpc_transport.c").read_text()
        start = source.index("static void serial_cb(")
        end = source.index("\n}\n", start) + 3
        harness = HARNESS.replace("/* ACTUAL_SERIAL_CB */", source[start:end])
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "test_uart_tx")
            subprocess.run(
                [os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                 "-x", "c", "-", "-o", executable],
                input=harness, text=True, check=True, timeout=30,
            )
            subprocess.run([executable], check=True, timeout=3)


if __name__ == "__main__":
    unittest.main()
