"""zmk_battery_lipo_mv_to_pct() follows the LiPo curve, and both converters can use it."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

APP = Path(__file__).resolve().parents[3]

HARNESS = r"""
#include <assert.h>
#include <zmk/battery_curve.h>

int main(void) {
    /* Clamped at both ends. */
    assert(zmk_battery_lipo_mv_to_pct(0) == 0);
    assert(zmk_battery_lipo_mv_to_pct(3270) == 0);
    assert(zmk_battery_lipo_mv_to_pct(4200) == 100);
    assert(zmk_battery_lipo_mv_to_pct(4350) == 100);

    /* On the chart's points. */
    assert(zmk_battery_lipo_mv_to_pct(3610) == 5);
    assert(zmk_battery_lipo_mv_to_pct(3690) == 10);
    assert(zmk_battery_lipo_mv_to_pct(3730) == 20);
    assert(zmk_battery_lipo_mv_to_pct(3840) == 50);
    assert(zmk_battery_lipo_mv_to_pct(4020) == 80);

    /* Interpolated between them. */
    assert(zmk_battery_lipo_mv_to_pct(3650) == 7);
    assert(zmk_battery_lipo_mv_to_pct(3440) == 2);
    assert(zmk_battery_lipo_mv_to_pct(4180) == 98);

    /* Never decreasing, never past 100. */
    int previous = 0;
    for (int mv = 3000; mv <= 4400; mv++) {
        int pct = zmk_battery_lipo_mv_to_pct((int16_t)mv);
        assert(pct >= previous && pct <= 100);
        previous = pct;
    }

    /* Near empty it reads lower than the straight line from 3.45 V to 4.2 V. */
    for (int mv = 3500; mv <= 3750; mv++) {
        assert(zmk_battery_lipo_mv_to_pct((int16_t)mv) < mv * 2 / 15 - 459);
    }
    return 0;
}
"""


class LipoCurveTests(unittest.TestCase):
    def test_curve(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "lipo_curve")
            subprocess.run(
                [os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                 "-I", str(APP / "include"), "-x", "c", "-", "-o", executable],
                input=HARNESS, text=True, check=True, timeout=30,
            )
            subprocess.run([executable], check=True, timeout=3)

    def test_both_converters_use_it_when_enabled(self):
        for path in ("module/drivers/sensor/battery/battery_common.c", "src/battery.c"):
            with self.subTest(path=path):
                source = (APP / path).read_text()
                self.assertIn("#include <zmk/battery_curve.h>", source)
                function = source[source.index("lithium_ion_mv_to_pct(int16_t bat_mv) {"):]
                function = function[: function.index("#endif\n}")]
                self.assertIn("#if IS_ENABLED(CONFIG_ZMK_BATTERY_LIPO_DISCHARGE_CURVE)", function)
                self.assertIn("return zmk_battery_lipo_mv_to_pct(bat_mv);", function)


if __name__ == "__main__":
    unittest.main()
