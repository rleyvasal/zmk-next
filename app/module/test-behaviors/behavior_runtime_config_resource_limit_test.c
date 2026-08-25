/*
 * Copyright (c) 2026 The ZMK Next Contributors
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_runtime_config_resource_limit_test

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk/runtime_config.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if (IS_ENABLED(CONFIG_ZMK_RUNTIME_CONFIG)) && (DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT))

/*
 * Fills each of the 5 staged-content pools (keymap overrides, objects,
 * combos, macro steps, tap-dance actions) to its exact compiled capacity
 * via the same append_staged_* functions a real client uses one item at a
 * time, then attempts one more append past the limit. Every append_staged_*
 * function is a simple bounds-check-then-copy with no content validation
 * (confirmed by reading all 5 implementations directly) - the fixed-size
 * pool arrays behind them are the actual thing standing between a client
 * bug and an out-of-bounds write, so this is a real memory-safety check,
 * not just a protocol-level one. Content doesn't need to be meaningfully
 * valid - only zmk_runtime_config_validate_snapshot() and
 * stage_uploaded_snapshot()'s deeper cross-reference checks care about
 * that, neither of which this test calls.
 *
 * Logs whether the overflow attempt was rejected (ret != 0) rather than
 * its raw errno, same discipline as every other test-only behavior here:
 * the exact negative-errno constant isn't part of the documented contract.
 */
static void report(const char *pool, bool all_filled, bool overflow_rejected) {
    LOG_INF("RCFG_RESOURCE_TEST %s all_filled=%d overflow_rejected=%d", pool, (int)all_filled,
            (int)overflow_rejected);
}

static int on_binding_pressed(struct zmk_behavior_binding *binding,
                              struct zmk_behavior_binding_event event) {
    uint32_t update_id;
    int ret;
    bool all_filled;
    int overflow_ret;

    /* --- keymap overrides --- */
    ret = zmk_runtime_config_begin_update(0, 1, &update_id);
    if (ret != 0) {
        LOG_INF("RCFG_RESOURCE_TEST keymap_overrides begin_error ret=%d", ret);
        goto objects_section;
    }
    all_filled = true;
    for (size_t i = 0; i < CONFIG_ZMK_RUNTIME_MAX_KEYMAP_OVERRIDES; i++) {
        struct zmk_runtime_keymap_override override = {
            .layer_id = 0, .key_position = (uint16_t)i, .action = {0}};
        if (zmk_runtime_config_append_staged_keymap_override(update_id, &override) != 0) {
            all_filled = false;
            break;
        }
    }
    {
        struct zmk_runtime_keymap_override overflow_item = {0};
        overflow_ret = zmk_runtime_config_append_staged_keymap_override(update_id, &overflow_item);
    }
    report("keymap_overrides", all_filled, overflow_ret != 0);
    (void)zmk_runtime_config_abort_update(update_id);

objects_section:
    /* --- objects --- */
    ret = zmk_runtime_config_begin_update(0, 1, &update_id);
    if (ret != 0) {
        LOG_INF("RCFG_RESOURCE_TEST objects begin_error ret=%d", ret);
        goto combos_section;
    }
    all_filled = true;
    for (size_t i = 0; i < CONFIG_ZMK_RUNTIME_MAX_OBJECTS; i++) {
        struct zmk_runtime_object_slot object = {.id = (uint32_t)(i + 1), .type = 0};
        if (zmk_runtime_config_append_staged_object(update_id, &object) != 0) {
            all_filled = false;
            break;
        }
    }
    {
        struct zmk_runtime_object_slot overflow_item = {.id = 1, .type = 0};
        overflow_ret = zmk_runtime_config_append_staged_object(update_id, &overflow_item);
    }
    report("objects", all_filled, overflow_ret != 0);
    (void)zmk_runtime_config_abort_update(update_id);

combos_section:
    /* --- combos --- */
    ret = zmk_runtime_config_begin_update(0, 1, &update_id);
    if (ret != 0) {
        LOG_INF("RCFG_RESOURCE_TEST combos begin_error ret=%d", ret);
        goto macro_steps_section;
    }
    all_filled = true;
    for (size_t i = 0; i < CONFIG_ZMK_RUNTIME_MAX_COMBOS; i++) {
        struct zmk_runtime_combo_slot combo = {.id = (uint32_t)(i + 1)};
        if (zmk_runtime_config_append_staged_combo(update_id, &combo) != 0) {
            all_filled = false;
            break;
        }
    }
    {
        struct zmk_runtime_combo_slot overflow_item = {.id = 1};
        overflow_ret = zmk_runtime_config_append_staged_combo(update_id, &overflow_item);
    }
    report("combos", all_filled, overflow_ret != 0);
    (void)zmk_runtime_config_abort_update(update_id);

macro_steps_section:
    /* --- macro steps --- */
    ret = zmk_runtime_config_begin_update(0, 1, &update_id);
    if (ret != 0) {
        LOG_INF("RCFG_RESOURCE_TEST macro_steps begin_error ret=%d", ret);
        goto tap_dance_actions_section;
    }
    all_filled = true;
    for (size_t i = 0; i < CONFIG_ZMK_RUNTIME_MAX_MACRO_STEPS; i++) {
        struct zmk_runtime_macro_step step = {.type = ZMK_RUNTIME_MACRO_STEP_WAIT,
                                              .data.duration_ms = 0};
        if (zmk_runtime_config_append_staged_macro_step(update_id, &step) != 0) {
            all_filled = false;
            break;
        }
    }
    {
        struct zmk_runtime_macro_step overflow_item = {.type = ZMK_RUNTIME_MACRO_STEP_WAIT,
                                                       .data.duration_ms = 0};
        overflow_ret = zmk_runtime_config_append_staged_macro_step(update_id, &overflow_item);
    }
    report("macro_steps", all_filled, overflow_ret != 0);
    (void)zmk_runtime_config_abort_update(update_id);

tap_dance_actions_section:
    /* --- tap-dance actions --- */
    ret = zmk_runtime_config_begin_update(0, 1, &update_id);
    if (ret != 0) {
        LOG_INF("RCFG_RESOURCE_TEST tap_dance_actions begin_error ret=%d", ret);
        return 0;
    }
    all_filled = true;
    for (size_t i = 0; i < CONFIG_ZMK_RUNTIME_MAX_TAP_DANCE_ACTIONS; i++) {
        struct zmk_runtime_tap_dance_action action = {0};
        if (zmk_runtime_config_append_staged_tap_dance_action(update_id, &action) != 0) {
            all_filled = false;
            break;
        }
    }
    {
        struct zmk_runtime_tap_dance_action overflow_item = {0};
        overflow_ret = zmk_runtime_config_append_staged_tap_dance_action(update_id, &overflow_item);
    }
    report("tap_dance_actions", all_filled, overflow_ret != 0);
    (void)zmk_runtime_config_abort_update(update_id);

    return 0;
}

static int on_binding_released(struct zmk_behavior_binding *binding,
                               struct zmk_behavior_binding_event event) {
    return 0;
}

static const struct behavior_driver_api runtime_config_resource_limit_test_driver_api = {
    .binding_pressed = on_binding_pressed, .binding_released = on_binding_released};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &runtime_config_resource_limit_test_driver_api);

#endif
