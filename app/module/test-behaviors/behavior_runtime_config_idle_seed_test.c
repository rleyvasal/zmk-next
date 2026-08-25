/*
 * Copyright (c) 2026 The ZMK Next Contributors
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_runtime_config_idle_seed_test

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk/runtime_config.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if (IS_ENABLED(CONFIG_ZMK_RUNTIME_CONFIG)) && (DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT))

/*
 * Requests activation of a trivial (empty-content) generation without
 * forcing it, unlike the mod-morph and power-loss tests, which both
 * bypass idle-gating on purpose since it isn't what they're testing. This
 * is the one real, unforced activation request this test category exists
 * to exercise - content doesn't matter here, only the timing.
 *
 * Real activation runs on Zephyr's own delayed work queue
 * (zmk_runtime_config_activation_work in runtime_config_activation.c),
 * rescheduled for CONFIG_ZMK_RUNTIME_CONFIG_ACTIVATION_IDLE_MS every time
 * a key releases with nothing else held and a generation pending, and
 * cancelled outright on every press - driven by this test's keymap's
 * kscan mock event delays (see native_sim.keymap), not by anything in
 * this file directly.
 */
static int on_binding_pressed(struct zmk_behavior_binding *binding,
                              struct zmk_behavior_binding_event event) {
    struct zmk_runtime_config_activation_status activation;
    uint32_t update_id;
    int ret;

    zmk_runtime_config_get_activation_status(&activation);
    ret = zmk_runtime_config_stage_stock_update(activation.active_generation, &update_id);
    if (ret != 0) {
        LOG_INF("RCFG_IDLE_TEST seed stage error ret=%d", ret);
        return 0;
    }

    ret = zmk_runtime_config_prepare_pending_update(update_id, 1);
    if (ret != 0) {
        LOG_INF("RCFG_IDLE_TEST seed prepare error ret=%d", ret);
        (void)zmk_runtime_config_abort_update(update_id);
        return 0;
    }

    ret = zmk_runtime_config_request_activation(1);
    if (ret != 0) {
        LOG_INF("RCFG_IDLE_TEST seed request error ret=%d", ret);
    }

    (void)zmk_runtime_config_abort_update(update_id);
    return 0;
}

static int on_binding_released(struct zmk_behavior_binding *binding,
                               struct zmk_behavior_binding_event event) {
    return 0;
}

static const struct behavior_driver_api runtime_config_idle_seed_test_driver_api = {
    .binding_pressed = on_binding_pressed, .binding_released = on_binding_released};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &runtime_config_idle_seed_test_driver_api);

#endif
