/*
 * Copyright (c) 2026 The ZMK Next Contributors
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>

#include <zmk/hid.h>
#include <zmk/matrix.h>
#include <zmk/runtime_config.h>
#include <zmk/runtime_dispatch.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct runtime_dispatch_invocation {
    bool active;
    zmk_runtime_object_id_t object_id;
    struct zmk_behavior_binding selected_binding;
};

static struct runtime_dispatch_invocation invocations[ZMK_KEYMAP_LEN];

static int dispatch_mod_morph(const struct zmk_runtime_object_slot *object,
                              struct zmk_behavior_binding_event event, bool pressed) {
    struct runtime_dispatch_invocation *invocation;
    struct zmk_behavior_binding selected_binding;
    const struct zmk_runtime_action_ref *selected_action;
    int ret;

    if (event.position >= ZMK_KEYMAP_LEN) {
        return -EINVAL;
    }

    invocation = &invocations[event.position];
    if (!pressed) {
        if (!invocation->active || invocation->object_id != object->id) {
            return -ENOENT;
        }

        selected_binding = invocation->selected_binding;
        memset(invocation, 0, sizeof(*invocation));
        ret = zmk_behavior_invoke_binding(&selected_binding, event, false);
        (void)zmk_runtime_config_activation_blocker_release();
        return ret;
    }

    if (invocation->active) {
        return -EBUSY;
    }

    selected_action = (zmk_hid_get_explicit_mods() & object->data.mod_morph.modifiers) != 0U
                          ? &object->data.mod_morph.morphed_action
                          : &object->data.mod_morph.normal_action;
    ret = zmk_runtime_config_action_ref_to_binding(selected_action, &selected_binding);
    if (ret != 0) {
        return ret;
    }

    ret = zmk_runtime_config_activation_blocker_acquire();
    if (ret != 0) {
        return ret;
    }

    invocation->active = true;
    invocation->object_id = object->id;
    invocation->selected_binding = selected_binding;
    ret = zmk_behavior_invoke_binding(&selected_binding, event, true);
    if (ret < 0) {
        memset(invocation, 0, sizeof(*invocation));
        (void)zmk_runtime_config_activation_blocker_release();
    }

    return ret;
}

int zmk_runtime_dispatch_object(zmk_runtime_object_id_t object_id,
                                struct zmk_behavior_binding_event event, bool pressed) {
    const struct zmk_runtime_object_slot *object =
        zmk_runtime_config_get_active_object(object_id);

    if (!object) {
        return -ENOENT;
    }

    switch (object->type) {
    case ZMK_RUNTIME_OBJECT_TYPE_MOD_MORPH:
        return dispatch_mod_morph(object, event, pressed);
    default:
        return -ENOTSUP;
    }
}
