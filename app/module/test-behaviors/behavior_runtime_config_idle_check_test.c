/*
 * Copyright (c) 2026 The ZMK Next Contributors
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_runtime_config_idle_check_test

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk/runtime_config.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if (IS_ENABLED(CONFIG_ZMK_RUNTIME_CONFIG)) && (DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT))

/*
 * Logs the current activation status. physical_layouts.c calls
 * zmk_runtime_config_note_key_state() for every physical key transition
 * *before* dispatching to the bound behavior's press handler (confirmed by
 * reading it, not assumed), so this behavior's own press is already
 * counted in pressed_key_count by the time it reads status here.
 */
static int on_binding_pressed(struct zmk_behavior_binding *binding,
                              struct zmk_behavior_binding_event event) {
    struct zmk_runtime_config_activation_status status;

    zmk_runtime_config_get_activation_status(&status);
    LOG_INF("RCFG_IDLE_TEST status active_generation=%u pending_generation=%u "
            "pressed_key_count=%u",
            status.active_generation, status.pending_generation, status.pressed_key_count);
    return 0;
}

static int on_binding_released(struct zmk_behavior_binding *binding,
                               struct zmk_behavior_binding_event event) {
    return 0;
}

static const struct behavior_driver_api runtime_config_idle_check_test_driver_api = {
    .binding_pressed = on_binding_pressed, .binding_released = on_binding_released};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &runtime_config_idle_check_test_driver_api);

#endif
