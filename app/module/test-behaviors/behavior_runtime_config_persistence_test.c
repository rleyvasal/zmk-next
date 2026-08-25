/*
 * Copyright (c) 2026 The ZMK Next Contributors
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_runtime_config_persistence_test

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk/runtime_config.h>
#include <zmk/runtime_config_test_hooks.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if (IS_ENABLED(CONFIG_ZMK_RUNTIME_CONFIG)) &&                                                     \
    (IS_ENABLED(CONFIG_ZMK_RUNTIME_CONFIG_TEST_HOOKS)) &&                                          \
    (DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT))

/*
 * 60 overrides at ~17-21 bytes each comfortably spans several of the
 * default 256-byte Settings chunks, so "0 chunks", "1 chunk", and "every
 * chunk but no manifest" are genuinely distinct states, not degenerate
 * single-chunk cases. Overrides use layers 1.. (never layer 0, the default
 * layer this behavior itself is bound on) at 4 positions per layer, so the
 * keymap below needs at least 1 + ceil(60/4) = 16 layers.
 */
#define TEST_OVERRIDE_COUNT 60
#define TEST_OVERRIDES_PER_LAYER 4

static void report(const char *scenario, uint32_t generation, bool has_snapshot) {
    LOG_INF("RCFG_PERSIST_TEST %s generation=%u has_snapshot=%d", scenario, generation,
            (int)has_snapshot);
}

static void report_error(const char *scenario, int ret) {
    LOG_INF("RCFG_PERSIST_TEST %s error ret=%d", scenario, ret);
}

static int stage_overrides(uint32_t update_id, size_t count) {
    /* This behavior's own DT node name doubles as its label below, so
     * there's no ambiguity about which one zmk_behavior_get_local_id()
     * resolves against - matches ZMK_RUNTIME_DISPATCH_BEHAVIOR_NAME's own
     * runtime_object.dtsi precedent of using identical label/node-name. */
    zmk_behavior_local_id_t local_id = zmk_behavior_get_local_id("rcfg_persist_test");

    for (size_t i = 0; i < count; i++) {
        struct zmk_runtime_keymap_override override = {
            .layer_id = (uint8_t)(i / TEST_OVERRIDES_PER_LAYER + 1),
            .key_position = (uint16_t)(i % TEST_OVERRIDES_PER_LAYER),
            .action =
                {
                    .kind = ZMK_RUNTIME_ACTION_COMPILED_BEHAVIOR,
                    .data.compiled = {.local_id = local_id, .param1 = 0, .param2 = 0},
                },
        };
        int ret = zmk_runtime_config_append_staged_keymap_override(update_id, &override);

        if (ret != 0) {
            return ret;
        }
    }

    return 0;
}

/*
 * Stages `override_count` keymap overrides and validates them, leaving the
 * update ready for zmk_runtime_config_persist_update() or a truncated-
 * persist test hook call. Mirrors zmk_runtime_config_stage_stock_update()'s
 * own begin/stage/validate pattern, but for non-empty caller-supplied
 * content - bypasses the chunk-upload wire simulation (a single dummy byte
 * satisfies the "fully uploaded" bookkeeping check) since nothing here is
 * testing the wire format, only the persistence layer beneath it.
 */
static int stage_test_snapshot(size_t override_count, uint32_t *update_id) {
    struct zmk_runtime_config_snapshot header;
    struct zmk_runtime_config_validation_result result;
    struct zmk_runtime_config_activation_status activation;
    uint8_t dummy_byte = 0;
    size_t accepted;
    size_t next_offset;
    int ret;

    /* begin_update() checks expected_active_generation for strict equality
     * against whatever is really active right now - the "0 means accept
     * whichever is active" convenience described in the protocol is
     * implemented in the Studio RPC glue this test bypasses, not here. */
    zmk_runtime_config_get_activation_status(&activation);
    ret = zmk_runtime_config_begin_update(activation.active_generation, 1, update_id);
    if (ret != 0) {
        return ret;
    }

    ret = zmk_runtime_config_upload_update_chunk(*update_id, 0, &dummy_byte, 1, &accepted,
                                                 &next_offset);
    if (ret != 0) {
        (void)zmk_runtime_config_abort_update(*update_id);
        return ret;
    }

    ret = stage_overrides(*update_id, override_count);
    if (ret != 0) {
        (void)zmk_runtime_config_abort_update(*update_id);
        return ret;
    }

    zmk_runtime_config_init_empty_snapshot(&header);
    header.keymap_override_count = override_count;

    ret = zmk_runtime_config_stage_uploaded_snapshot(*update_id, &header, &result);
    if (ret != 0) {
        (void)zmk_runtime_config_abort_update(*update_id);
        return ret;
    }

    return 0;
}

static void report_reload(const char *scenario) {
    struct zmk_runtime_config_persistence_status status;

    zmk_runtime_config_test_reload();
    zmk_runtime_config_get_persistence_status(&status);
    if (status.has_persisted_snapshot) {
        /* select_newest_valid_slot() (run from inside test_reload()'s
         * settings_load(), exactly like a real boot) unconditionally
         * re-marks whatever it selects as pending, even when it's the
         * same generation already active. Clear it now, the same way
         * real idle-triggered activation eventually would, so the next
         * stage's begin_update() isn't blocked by a redundant pending
         * re-mark that has nothing to do with this test's own persists. */
        (void)zmk_runtime_config_activate_pending_generation(status.persisted_generation);
    }
    report(scenario, status.persisted_generation, status.has_persisted_snapshot);
}

static int on_binding_pressed(struct zmk_behavior_binding *binding,
                              struct zmk_behavior_binding_event event) {
    uint32_t update_id;
    uint32_t generation;
    int ret;

    /* Baseline: a real, complete persist, the normal way. */
    ret = stage_test_snapshot(TEST_OVERRIDE_COUNT, &update_id);
    if (ret != 0) {
        report_error("stage_baseline", ret);
        return 0;
    }
    ret = zmk_runtime_config_persist_update(update_id, &generation);
    if (ret != 0) {
        report_error("persist_baseline", ret);
        return 0;
    }
    /* persist_update() only requests activation; it stays pending until
     * real idle time passes, which won't happen synchronously here (this
     * behavior's own trigger key is still physically down the whole time).
     * report_reload() below clears it - this test is about persistence
     * recovery, not idle-gating, which is its own separate, not-yet-written
     * test category. */
    report_reload("after_baseline");

    /* Zero chunks written, no manifest: the new slot was touched only to
     * invalidate its stale manifest. The old slot must still win. */
    ret = stage_test_snapshot(TEST_OVERRIDE_COUNT, &update_id);
    if (ret != 0) {
        report_error("stage_zero_chunks", ret);
        return 0;
    }
    ret = zmk_runtime_config_test_persist_truncated(update_id, 0, false);
    if (ret != 0) {
        report_error("truncate_zero_chunks", ret);
    }
    report_reload("after_zero_chunks");

    /* One of several chunks written, no manifest: the new slot is
     * incomplete. The old slot must still win. */
    ret = stage_test_snapshot(TEST_OVERRIDE_COUNT, &update_id);
    if (ret != 0) {
        report_error("stage_one_chunk", ret);
        return 0;
    }
    ret = zmk_runtime_config_test_persist_truncated(update_id, 1, false);
    if (ret != 0) {
        report_error("truncate_one_chunk", ret);
    }
    report_reload("after_one_chunk");

    /* Every chunk written, manifest withheld: the closest-to-success case
     * that must still be safely rejected. The old slot must still win. */
    ret = stage_test_snapshot(TEST_OVERRIDE_COUNT, &update_id);
    if (ret != 0) {
        report_error("stage_all_chunks_no_manifest", ret);
        return 0;
    }
    ret = zmk_runtime_config_test_persist_truncated(update_id, SIZE_MAX, false);
    if (ret != 0) {
        report_error("truncate_all_chunks_no_manifest", ret);
    }
    report_reload("after_all_chunks_no_manifest");

    /* A second real, complete persist proves the mechanism can still
     * genuinely succeed - the three rejections above hold a torn write
     * back, not the persistence layer itself. */
    ret = stage_test_snapshot(TEST_OVERRIDE_COUNT, &update_id);
    if (ret != 0) {
        report_error("stage_second_real", ret);
        return 0;
    }
    ret = zmk_runtime_config_persist_update(update_id, &generation);
    if (ret != 0) {
        report_error("persist_second_real", ret);
        return 0;
    }
    report_reload("after_second_real");

    return 0;
}

static int on_binding_released(struct zmk_behavior_binding *binding,
                               struct zmk_behavior_binding_event event) {
    return 0;
}

static const struct behavior_driver_api runtime_config_persistence_test_driver_api = {
    .binding_pressed = on_binding_pressed, .binding_released = on_binding_released};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &runtime_config_persistence_test_driver_api);

#endif
