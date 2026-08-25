/*
 * Copyright (c) 2026 The ZMK Next Contributors
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_runtime_config_validate_test

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk/runtime_config.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if (IS_ENABLED(CONFIG_ZMK_RUNTIME_CONFIG)) && (DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT))

/*
 * Drives zmk_runtime_config_validate_snapshot() through every rejection path
 * it implements (schema version, capability fingerprint, and each of the 5
 * resource-limit counters) plus the NULL-argument and known-good cases, and
 * logs one deterministic line per case. There is no ztest harness anywhere
 * in this tree; this mirrors the existing app/tests/studio convention of
 * driving real logic from a test-only behavior and diffing captured log
 * lines against a golden snapshot instead.
 */
/*
 * Logs whether the call failed (ret != 0) rather than the raw errno value:
 * the exact negative-errno constants (-EINVAL, -EXDEV, ...) aren't part of
 * this function's documented contract and could differ by libc, so pinning
 * a golden snapshot to them would be testing an incidental detail instead
 * of the real one (result->valid / result->error).
 */
static void report(const char *scenario, int ret,
                    const struct zmk_runtime_config_validation_result *result) {
    int failed = ret != 0;

    if (result) {
        LOG_INF("RCFG_VALIDATE_TEST %s failed=%d valid=%d error=%d", scenario, failed,
                (int)result->valid, (int)result->error);
    } else {
        LOG_INF("RCFG_VALIDATE_TEST %s failed=%d valid=n/a error=n/a", scenario, failed);
    }
}

static int on_binding_pressed(struct zmk_behavior_binding *binding,
                              struct zmk_behavior_binding_event event) {
    struct zmk_runtime_config_snapshot snapshot;
    struct zmk_runtime_config_validation_result result;
    struct zmk_runtime_capabilities capabilities;
    int ret;

    zmk_runtime_config_get_capabilities(&capabilities);

    zmk_runtime_config_init_empty_snapshot(&snapshot);
    ret = zmk_runtime_config_validate_snapshot(&snapshot, &result);
    report("valid_empty", ret, &result);

    zmk_runtime_config_init_empty_snapshot(&snapshot);
    snapshot.persistence_schema_version = (uint16_t)(capabilities.persistence_schema_version + 1);
    ret = zmk_runtime_config_validate_snapshot(&snapshot, &result);
    report("schema_version_mismatch", ret, &result);

    zmk_runtime_config_init_empty_snapshot(&snapshot);
    snapshot.capability_fingerprint[0] ^= 0xFFU;
    ret = zmk_runtime_config_validate_snapshot(&snapshot, &result);
    report("capability_fingerprint_mismatch", ret, &result);

    zmk_runtime_config_init_empty_snapshot(&snapshot);
    snapshot.keymap_override_count = (uint16_t)(capabilities.max_keymap_overrides + 1);
    ret = zmk_runtime_config_validate_snapshot(&snapshot, &result);
    report("keymap_override_count_over_limit", ret, &result);

    zmk_runtime_config_init_empty_snapshot(&snapshot);
    snapshot.object_count = (uint16_t)(capabilities.max_objects + 1);
    ret = zmk_runtime_config_validate_snapshot(&snapshot, &result);
    report("object_count_over_limit", ret, &result);

    zmk_runtime_config_init_empty_snapshot(&snapshot);
    snapshot.combo_count = (uint16_t)(capabilities.max_combos + 1);
    ret = zmk_runtime_config_validate_snapshot(&snapshot, &result);
    report("combo_count_over_limit", ret, &result);

    zmk_runtime_config_init_empty_snapshot(&snapshot);
    snapshot.macro_step_count = (uint16_t)(capabilities.max_macro_steps + 1);
    ret = zmk_runtime_config_validate_snapshot(&snapshot, &result);
    report("macro_step_count_over_limit", ret, &result);

    zmk_runtime_config_init_empty_snapshot(&snapshot);
    snapshot.tap_dance_action_count = (uint16_t)(capabilities.max_tap_dance_actions + 1);
    ret = zmk_runtime_config_validate_snapshot(&snapshot, &result);
    report("tap_dance_action_count_over_limit", ret, &result);

    zmk_runtime_config_init_empty_snapshot(&snapshot);
    ret = zmk_runtime_config_validate_snapshot(&snapshot, NULL);
    report("null_result", ret, NULL);

    ret = zmk_runtime_config_validate_snapshot(NULL, &result);
    report("null_snapshot", ret, &result);

    return 0;
}

static int on_binding_released(struct zmk_behavior_binding *binding,
                               struct zmk_behavior_binding_event event) {
    return 0;
}

static const struct behavior_driver_api runtime_config_validate_test_driver_api = {
    .binding_pressed = on_binding_pressed,
    .binding_released = on_binding_released};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &runtime_config_validate_test_driver_api);

#endif
