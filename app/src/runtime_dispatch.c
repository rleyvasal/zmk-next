/*
 * Copyright (c) 2026 The ZMK Next Contributors
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/hid.h>
#include <zmk/matrix.h>
#include <zmk/behavior_hold_tap.h>
#include <zmk/behavior_tap_dance.h>
#include <zmk/runtime_config.h>
#include <zmk/runtime_dispatch.h>
#include <zmk/virtual_key_position.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

enum runtime_dispatch_invocation_type {
    RUNTIME_DISPATCH_INVOCATION_NONE,
    RUNTIME_DISPATCH_INVOCATION_MOD_MORPH,
    RUNTIME_DISPATCH_INVOCATION_MACRO,
};

struct runtime_macro_invocation {
    bool work_initialized;
    bool paused;
    bool tap_release_pending;
    uint16_t next_step;
    struct zmk_behavior_binding_event event;
    struct zmk_behavior_binding tap_binding;
    struct k_work_delayable work;
};

struct runtime_dispatch_invocation {
    bool active;
    enum runtime_dispatch_invocation_type type;
    zmk_runtime_object_id_t object_id;
    struct zmk_behavior_binding selected_binding;
    struct runtime_macro_invocation macro;
};

#define RUNTIME_DISPATCH_INVOCATION_COUNT                                                   \
    (ZMK_KEYMAP_LEN + ZMK_KEYMAP_SENSORS_LEN + ZMK_COMBOS_LEN + CONFIG_ZMK_RUNTIME_MAX_COMBOS)

static struct runtime_dispatch_invocation invocations[RUNTIME_DISPATCH_INVOCATION_COUNT];

static void macro_work_handler(struct k_work *work);

static void clear_invocation(struct runtime_dispatch_invocation *invocation) {
    invocation->active = false;
    invocation->type = RUNTIME_DISPATCH_INVOCATION_NONE;
    invocation->object_id = ZMK_RUNTIME_OBJECT_ID_INVALID;
    memset(&invocation->selected_binding, 0, sizeof(invocation->selected_binding));
    invocation->macro.paused = false;
    invocation->macro.tap_release_pending = false;
    invocation->macro.next_step = 0U;
    memset(&invocation->macro.event, 0, sizeof(invocation->macro.event));
    memset(&invocation->macro.tap_binding, 0, sizeof(invocation->macro.tap_binding));
}

static int dispatch_mod_morph(const struct zmk_runtime_object_slot *object,
                              struct zmk_behavior_binding_event event, bool pressed) {
    struct runtime_dispatch_invocation *invocation;
    struct zmk_behavior_binding selected_binding;
    const struct zmk_runtime_action_ref *selected_action;
    int ret;

    if (event.position >= RUNTIME_DISPATCH_INVOCATION_COUNT) {
        return -EINVAL;
    }

    invocation = &invocations[event.position];
    if (!pressed) {
        if (!invocation->active || invocation->type != RUNTIME_DISPATCH_INVOCATION_MOD_MORPH ||
            invocation->object_id != object->id) {
            return -ENOENT;
        }

        selected_binding = invocation->selected_binding;
        clear_invocation(invocation);
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
    invocation->type = RUNTIME_DISPATCH_INVOCATION_MOD_MORPH;
    invocation->object_id = object->id;
    invocation->selected_binding = selected_binding;
    ret = zmk_behavior_invoke_binding(&selected_binding, event, true);
    if (ret < 0) {
        clear_invocation(invocation);
        (void)zmk_runtime_config_activation_blocker_release();
    }

    return ret;
}

static void finish_macro(struct runtime_dispatch_invocation *invocation) {
    clear_invocation(invocation);
    (void)zmk_runtime_config_activation_blocker_release();
}

static int schedule_macro(struct runtime_dispatch_invocation *invocation, uint32_t duration_ms) {
    int ret = k_work_schedule(&invocation->macro.work, K_MSEC(duration_ms));

    return ret < 0 ? ret : 0;
}

static int continue_macro(struct runtime_dispatch_invocation *invocation) {
    const struct zmk_runtime_macro_step *steps;
    size_t step_count;
    int ret;

    ret = zmk_runtime_config_get_active_macro_steps(invocation->object_id, &steps, &step_count);
    if (ret != 0) {
        return ret;
    }

    if (invocation->macro.tap_release_pending) {
        ret = zmk_behavior_invoke_binding(&invocation->macro.tap_binding,
                                          invocation->macro.event, false);
        if (ret < 0) {
            return ret;
        }
        invocation->macro.tap_release_pending = false;
        memset(&invocation->macro.tap_binding, 0, sizeof(invocation->macro.tap_binding));
    }

    while (invocation->macro.next_step < step_count) {
        const struct zmk_runtime_macro_step *step =
            &steps[invocation->macro.next_step];
        struct zmk_behavior_binding binding;

        switch (step->type) {
        case ZMK_RUNTIME_MACRO_STEP_TAP:
            ret = zmk_runtime_config_action_ref_to_binding(&step->data.action, &binding);
            if (ret != 0) {
                return ret;
            }

            ret = zmk_behavior_invoke_binding(&binding, invocation->macro.event, true);
            if (ret < 0) {
                return ret;
            }

            invocation->macro.next_step++;
            invocation->macro.tap_binding = binding;
            invocation->macro.tap_release_pending = true;
            return schedule_macro(invocation, CONFIG_ZMK_MACRO_DEFAULT_TAP_MS);
        case ZMK_RUNTIME_MACRO_STEP_PRESS:
            ret = zmk_runtime_config_action_ref_to_binding(&step->data.action, &binding);
            if (ret != 0) {
                return ret;
            }

            ret = zmk_behavior_invoke_binding(&binding, invocation->macro.event, true);
            if (ret < 0) {
                return ret;
            }

            invocation->macro.next_step++;
            break;
        case ZMK_RUNTIME_MACRO_STEP_RELEASE:
            ret = zmk_runtime_config_action_ref_to_binding(&step->data.action, &binding);
            if (ret != 0) {
                return ret;
            }

            ret = zmk_behavior_invoke_binding(&binding, invocation->macro.event, false);
            if (ret < 0) {
                return ret;
            }

            invocation->macro.next_step++;
            break;
        case ZMK_RUNTIME_MACRO_STEP_WAIT:
            invocation->macro.next_step++;
            if (step->data.duration_ms != 0U) {
                return schedule_macro(invocation, step->data.duration_ms);
            }
            break;
        case ZMK_RUNTIME_MACRO_STEP_PAUSE_UNTIL_RELEASE:
            invocation->macro.next_step++;
            invocation->macro.paused = true;
            return 0;
        default:
            return -EINVAL;
        }
    }

    finish_macro(invocation);
    return 0;
}

static void macro_work_handler(struct k_work *work) {
    struct k_work_delayable *delayed_work = k_work_delayable_from_work(work);
    struct runtime_macro_invocation *macro =
        CONTAINER_OF(delayed_work, struct runtime_macro_invocation, work);
    struct runtime_dispatch_invocation *invocation =
        CONTAINER_OF(macro, struct runtime_dispatch_invocation, macro);

    if (!invocation->active || invocation->type != RUNTIME_DISPATCH_INVOCATION_MACRO ||
        invocation->macro.paused) {
        return;
    }

    if (continue_macro(invocation) < 0) {
        LOG_ERR("Runtime macro %u stopped after a behavior error", invocation->object_id);
        finish_macro(invocation);
    }
}

static int dispatch_macro(const struct zmk_runtime_object_slot *object,
                          struct zmk_behavior_binding_event event, bool pressed) {
    struct runtime_dispatch_invocation *invocation;
    const struct zmk_runtime_macro_step *steps;
    size_t step_count;
    int ret;

    if (event.position >= RUNTIME_DISPATCH_INVOCATION_COUNT) {
        return -EINVAL;
    }

    invocation = &invocations[event.position];
    if (!pressed) {
        if (!invocation->active || invocation->type != RUNTIME_DISPATCH_INVOCATION_MACRO ||
            invocation->object_id != object->id || !invocation->macro.paused) {
            return ZMK_BEHAVIOR_OPAQUE;
        }

        invocation->macro.paused = false;
        ret = continue_macro(invocation);
        if (ret < 0) {
            LOG_ERR("Runtime macro %u stopped after a behavior error", object->id);
            finish_macro(invocation);
            return ret;
        }

        return ZMK_BEHAVIOR_OPAQUE;
    }

    if (invocation->active) {
        return -EBUSY;
    }

    ret = zmk_runtime_config_get_active_macro_steps(object->id, &steps, &step_count);
    if (ret != 0 || !steps || step_count == 0U) {
        return ret != 0 ? ret : -EINVAL;
    }

    ret = zmk_runtime_config_activation_blocker_acquire();
    if (ret != 0) {
        return ret;
    }

    if (!invocation->macro.work_initialized) {
        k_work_init_delayable(&invocation->macro.work, macro_work_handler);
        invocation->macro.work_initialized = true;
    }

    invocation->active = true;
    invocation->type = RUNTIME_DISPATCH_INVOCATION_MACRO;
    invocation->object_id = object->id;
    invocation->macro.event = event;
    invocation->macro.next_step = 0U;
    invocation->macro.paused = false;
    invocation->macro.tap_release_pending = false;
    ret = continue_macro(invocation);
    if (ret < 0) {
        finish_macro(invocation);
        return ret;
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int dispatch_hold_tap(const struct zmk_runtime_object_slot *object,
                             struct zmk_behavior_binding_event event, bool pressed) {
    const struct zmk_runtime_hold_tap_config *config = &object->data.hold_tap;
    struct zmk_behavior_binding tap_binding;
    struct zmk_behavior_binding hold_binding;
    int ret;

    if (!pressed) {
        return zmk_behavior_hold_tap_runtime_released(event);
    }

    ret = zmk_runtime_config_action_ref_to_binding(&config->tap_action, &tap_binding);
    if (ret != 0) {
        return ret;
    }

    ret = zmk_runtime_config_action_ref_to_binding(&config->hold_action, &hold_binding);
    if (ret != 0) {
        return ret;
    }

    return zmk_behavior_hold_tap_runtime_pressed(config, &tap_binding, &hold_binding, event);
}

static int dispatch_tap_dance(const struct zmk_runtime_object_slot *object,
                              struct zmk_behavior_binding_event event, bool pressed) {
    const struct zmk_runtime_tap_dance_config *config = &object->data.tap_dance;
    const struct zmk_runtime_tap_dance_action *actions;
    size_t action_count;
    uint32_t tapping_term_ms;
    int ret;

    if (!pressed) {
        return zmk_behavior_tap_dance_runtime_released(event);
    }

    ret = zmk_runtime_config_get_active_tap_dance_actions(object->id, &actions, &action_count,
                                                           &tapping_term_ms);
    if (ret != 0 || action_count != config->action_count ||
        tapping_term_ms != config->tapping_term_ms) {
        return ret != 0 ? ret : -EINVAL;
    }

    return zmk_behavior_tap_dance_runtime_pressed(config, actions, event);
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
    case ZMK_RUNTIME_OBJECT_TYPE_MACRO:
        return dispatch_macro(object, event, pressed);
    case ZMK_RUNTIME_OBJECT_TYPE_HOLD_TAP:
        return dispatch_hold_tap(object, event, pressed);
    case ZMK_RUNTIME_OBJECT_TYPE_TAP_DANCE:
        return dispatch_tap_dance(object, event, pressed);
    default:
        return -ENOTSUP;
    }
}
