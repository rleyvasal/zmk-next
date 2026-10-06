"""Check the actual main startup barrier with settings-lock mocks."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#define IS_ENABLED(option) option
#define LOG_INF(...) ((void)0)
static bool settings_locked, journal_ready, writer_queued;
static int init_result, load_result, init_calls, load_calls, hook_calls;
int settings_subsys_init(void) { init_calls++; return init_result; }
int settings_load(void) {
    load_calls++;
    settings_locked=true;
    /* Bluetooth's synchronous startup commands still need the system queue. */
    assert(!journal_ready && !writer_queued);
    settings_locked=false;
    return load_result;
}
void zmk_settings_loaded(void) {
    assert(!settings_locked && init_calls==1 && load_calls==1 && load_result==0);
    journal_ready=true; writer_queued=true; hook_calls++;
}
/* ACTUAL_MAIN */
static void check(int init_err, int load_err, int expected) {
    init_result=init_err; load_result=load_err;
    init_calls=load_calls=hook_calls=0;
    settings_locked=journal_ready=writer_queued=false;
    assert(firmware_main()==0);
    assert(hook_calls==expected);
    assert(writer_queued==(expected==1));
    assert(init_calls==CONFIG_SETTINGS);
    assert(load_calls==(CONFIG_SETTINGS && init_err==0));
}
int main(void) {
    check(0,0,CONFIG_SETTINGS);
    check(0,-5,0);
    check(-5,0,0);
    return 0;
}
"""


class SettingsLoadedTests(unittest.TestCase):
    def test_journal_hook_runs_only_after_successful_unlocked_load(self):
        source = (Path(__file__).resolve().parents[3] / "src/main.c").read_text()
        self.assertIn("__weak void zmk_settings_loaded(void) {}", source)
        main = source[source.index("int main(void) {"):]
        main = main.replace("int main(void)", "static int firmware_main(void)", 1)
        with tempfile.TemporaryDirectory() as directory:
            for settings in (0, 1):
                with self.subTest(settings=settings):
                    executable = str(Path(directory) / f"test_startup_{settings}")
                    subprocess.run(
                        [os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                         f"-DCONFIG_SETTINGS={settings}", "-x", "c", "-", "-o", executable],
                        input=HARNESS.replace("/* ACTUAL_MAIN */", main),
                        text=True, check=True, timeout=30,
                    )
                    subprocess.run([executable], check=True, timeout=3)


if __name__ == "__main__":
    unittest.main()
