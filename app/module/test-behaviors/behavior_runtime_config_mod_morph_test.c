/*
 * Copyright (c) 2026 The ZMK Next Contributors
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_runtime_config_mod_morph_test

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk/runtime_config.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if (IS_ENABLED(CONFIG_ZMK_RUNTIME_CONFIG)) && (DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT))

/*
 * Seeds one runtime mod-morph object (id 1: normal action &kp A, morphed
 * action &kp B, keyed off a modifier this test never holds) and activates
 * it synchronously, so the separate &rt 1 key elsewhere in this test's
 * keymap dispatches through a real, live runtime object when pressed.
 *
 * Bypasses real Settings persistence entirely (this test is about live
 * dispatch correctness, not reboot survival - that's covered by
 * app/tests/runtime-config/power-loss) and forces activation directly via
 * zmk_runtime_config_activate_pending_generation() rather than waiting on
 * real idle-gating, which is its own separate, not-yet-written test
 * category. Only tests the un-morphed path: the exact HID log format when
 * a modifier is genuinely held (register/unregister reference counting
 * across several distinct log lines, confirmed by reading a real
 * app/tests/modifiers snapshot) is too stateful to hand-derive with
 * confidence here - left as a follow-on once that format has a directly
 * comparable precedent to build from instead of a guess.
 */
static int on_binding_pressed(struct zmk_behavior_binding *binding,
                              struct zmk_behavior_binding_event event) {
    zmk_behavior_local_id_t kp_local_id = zmk_behavior_get_local_id("key_press");
    uint32_t update_id;
    struct zmk_runtime_config_activation_status activation;
    struct zmk_runtime_config_snapshot header;
    struct zmk_runtime_config_validation_result result;
    struct zmk_runtime_object_slot object = {
        .id = 1,
        .type = ZMK_RUNTIME_OBJECT_TYPE_MOD_MORPH,
        .data.mod_morph =
            {
                .modifiers = 0x02, /* left shift bit; never held in this test */
                .normal_action =
                    {
                        .kind = ZMK_RUNTIME_ACTION_COMPILED_BEHAVIOR,
                        .data.compiled = {.local_id = kp_local_id, .param1 = 0x04, .param2 = 0},
                    },
                .morphed_action =
                    {
                        .kind = ZMK_RUNTIME_ACTION_COMPILED_BEHAVIOR,
                        .data.compiled = {.local_id = kp_local_id, .param1 = 0x05, .param2 = 0},
                    },
            },
    };
    int ret;

    zmk_runtime_config_get_activation_status(&activation);
    ret = zmk_runtime_config_begin_update(activation.active_generation, 1, &update_id);
    if (ret != 0) {
        LOG_INF("RCFG_MOD_MORPH_TEST begin error ret=%d", ret);
        return 0;
    }

    ret = zmk_runtime_config_append_staged_object(update_id, &object);
    if (ret != 0) {
        LOG_INF("RCFG_MOD_MORPH_TEST append error ret=%d", ret);
        (void)zmk_runtime_config_abort_update(update_id);
        return 0;
    }

    zmk_runtime_config_init_empty_snapshot(&header);
    header.object_count = 1;

    ret = zmk_runtime_config_stage_uploaded_snapshot(update_id, &header, &result);
    if (ret != 0) {
        LOG_INF("RCFG_MOD_MORPH_TEST stage error ret=%d", ret);
        (void)zmk_runtime_config_abort_update(update_id);
        return 0;
    }

    ret = zmk_runtime_config_prepare_pending_update(update_id, 1);
    if (ret != 0) {
        LOG_INF("RCFG_MOD_MORPH_TEST prepare error ret=%d", ret);
        (void)zmk_runtime_config_abort_update(update_id);
        return 0;
    }

    ret = zmk_runtime_config_activate_pending_generation(1);
    if (ret != 0) {
        LOG_INF("RCFG_MOD_MORPH_TEST activate error ret=%d", ret);
        return 0;
    }

    (void)zmk_runtime_config_abort_update(update_id);
    LOG_INF("RCFG_MOD_MORPH_TEST seeded");
    return 0;
}

static int on_binding_released(struct zmk_behavior_binding *binding,
                               struct zmk_behavior_binding_event event) {
    return 0;
}

static const struct behavior_driver_api runtime_config_mod_morph_test_driver_api = {
    .binding_pressed = on_binding_pressed, .binding_released = on_binding_released};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &runtime_config_mod_morph_test_driver_api);

#endif
