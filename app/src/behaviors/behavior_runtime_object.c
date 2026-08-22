/*
 * Copyright (c) 2026 The ZMK Next Contributors
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_runtime_object

#include <zephyr/device.h>

#include <drivers/behavior.h>

#include <zmk/runtime_dispatch.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
static const struct behavior_parameter_value_metadata param_values[] = {{
    .display_name = "Runtime object ID",
    .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
    .range = {.min = 1, .max = CONFIG_ZMK_RUNTIME_MAX_OBJECTS},
}};

static const struct behavior_parameter_metadata_set param_metadata_set[] = {{
    .param1_values = param_values,
    .param1_values_len = ARRAY_SIZE(param_values),
}};

static const struct behavior_parameter_metadata metadata = {
    .sets_len = ARRAY_SIZE(param_metadata_set),
    .sets = param_metadata_set,
};
#endif

static int on_runtime_object_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    return zmk_runtime_dispatch_object(binding->param1, event, true);
}

static int on_runtime_object_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return zmk_runtime_dispatch_object(binding->param1, event, false);
}

static const struct behavior_driver_api runtime_object_driver_api = {
    .binding_pressed = on_runtime_object_pressed,
    .binding_released = on_runtime_object_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &runtime_object_driver_api);

#endif
