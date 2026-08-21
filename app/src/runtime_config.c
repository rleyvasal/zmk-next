/*
 * Copyright (c) 2026 The ZMK Next Contributors
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include <drivers/behavior.h>

#include <zmk/keymap.h>
#include <zmk/matrix.h>
#include <zmk/runtime_config.h>

void zmk_combo_runtime_config_refresh(void);

static struct {
    bool present;
    struct zmk_runtime_config_snapshot snapshot;
    struct zmk_runtime_config_pool pool;
    struct {
        bool active;
        uint32_t id;
        size_t expected_size;
        size_t received_size;
        bool validated;
    } update;
    uint32_t next_update_id;
} staged_config;

static struct {
    bool present;
    uint32_t generation;
    uint16_t keymap_override_count;
    uint16_t object_count;
    uint16_t combo_count;
    uint16_t macro_step_count;
    uint16_t tap_dance_action_count;
    struct zmk_runtime_keymap_override
        keymap_overrides[CONFIG_ZMK_RUNTIME_MAX_KEYMAP_OVERRIDES];
    struct zmk_runtime_object_slot objects[CONFIG_ZMK_RUNTIME_MAX_OBJECTS];
    struct zmk_runtime_combo_slot combos[CONFIG_ZMK_RUNTIME_MAX_COMBOS];
    struct zmk_runtime_macro_step macro_steps[CONFIG_ZMK_RUNTIME_MAX_MACRO_STEPS];
    struct zmk_runtime_tap_dance_action
        tap_dance_actions[CONFIG_ZMK_RUNTIME_MAX_TAP_DANCE_ACTIONS];
    struct zmk_behavior_binding bindings[CONFIG_ZMK_RUNTIME_MAX_KEYMAP_OVERRIDES];
} pending_config, active_config;

#define RUNTIME_CONFIG_PERSISTED_PAYLOAD_MAGIC 0x5A4E4B4FU
#define RUNTIME_CONFIG_PERSISTED_PAYLOAD_VERSION 6U

struct runtime_config_persisted_payload_header {
    uint32_t magic;
    uint16_t version;
    uint16_t keymap_override_count;
    uint16_t object_count;
    uint16_t combo_count;
    uint16_t macro_step_count;
    uint16_t tap_dance_action_count;
} __packed;

struct runtime_config_persisted_action_ref {
    uint8_t kind;
    zmk_behavior_local_id_t local_id;
    uint32_t param1;
    uint32_t param2;
    zmk_runtime_object_id_t object_id;
} __packed;

struct runtime_config_persisted_keymap_override {
    uint8_t layer_id;
    uint16_t key_position;
    struct runtime_config_persisted_action_ref action;
} __packed;

struct runtime_config_persisted_mod_morph {
    uint32_t modifiers;
    struct runtime_config_persisted_action_ref normal_action;
    struct runtime_config_persisted_action_ref morphed_action;
} __packed;

struct runtime_config_persisted_macro {
    uint16_t step_offset;
    uint16_t step_count;
} __packed;

struct runtime_config_persisted_hold_tap {
    struct runtime_config_persisted_action_ref tap_action;
    struct runtime_config_persisted_action_ref hold_action;
    uint8_t flavor;
    uint32_t tapping_term_ms;
    uint32_t quick_tap_ms;
    uint32_t require_prior_idle_ms;
} __packed;

struct runtime_config_persisted_tap_dance {
    uint16_t action_offset;
    uint16_t action_count;
    uint32_t tapping_term_ms;
} __packed;

struct runtime_config_persisted_object {
    zmk_runtime_object_id_t id;
    uint8_t type;
    union {
        struct runtime_config_persisted_mod_morph mod_morph;
        struct runtime_config_persisted_macro macro;
        struct runtime_config_persisted_hold_tap hold_tap;
        struct runtime_config_persisted_tap_dance tap_dance;
    } data;
} __packed;

struct runtime_config_persisted_combo {
    zmk_runtime_object_id_t id;
    uint8_t key_count;
    uint32_t timeout_ms;
    uint32_t require_prior_idle_ms;
    bool slow_release;
    uint16_t positions[CONFIG_ZMK_RUNTIME_MAX_COMBO_KEYS];
    struct runtime_config_persisted_action_ref output;
} __packed;

struct runtime_config_persisted_macro_step {
    uint8_t type;
    union {
        struct runtime_config_persisted_action_ref action;
        uint32_t duration_ms;
    } data;
} __packed;

struct runtime_config_persisted_tap_dance_action {
    struct runtime_config_persisted_action_ref tap_action;
    struct runtime_config_persisted_action_ref hold_action;
} __packed;

static void
fill_capability_fingerprint(uint8_t fingerprint[ZMK_RUNTIME_CAPABILITY_FINGERPRINT_SIZE]) {
    memset(fingerprint, 0, ZMK_RUNTIME_CAPABILITY_FINGERPRINT_SIZE);
    fingerprint[0] = 'Z';
    fingerprint[1] = 'N';
    fingerprint[2] = 'R';
    fingerprint[3] = 'C';
    fingerprint[4] = ZMK_RUNTIME_CONFIG_PERSISTENCE_SCHEMA_VERSION;
    fingerprint[5] = CONFIG_ZMK_RUNTIME_MAX_OBJECTS;
    fingerprint[6] = CONFIG_ZMK_RUNTIME_MAX_COMBOS;
    fingerprint[7] = CONFIG_ZMK_RUNTIME_MAX_COMBO_KEYS;
    fingerprint[8] = CONFIG_ZMK_RUNTIME_MAX_MACRO_STEPS & 0xFF;
    fingerprint[9] = CONFIG_ZMK_RUNTIME_MAX_MACRO_STEPS >> 8;
    fingerprint[10] = CONFIG_ZMK_RUNTIME_MAX_TAP_DANCE_ACTIONS & 0xFF;
    fingerprint[11] = CONFIG_ZMK_RUNTIME_MAX_TAP_DANCE_ACTIONS >> 8;
    fingerprint[12] = CONFIG_ZMK_RUNTIME_MAX_PERSISTED_BYTES & 0xFF;
    fingerprint[13] = CONFIG_ZMK_RUNTIME_MAX_PERSISTED_BYTES >> 8;
    fingerprint[14] = CONFIG_ZMK_RUNTIME_MAX_KEYMAP_OVERRIDES & 0xFF;
    fingerprint[15] = CONFIG_ZMK_RUNTIME_MAX_KEYMAP_OVERRIDES >> 8;
}

void zmk_runtime_config_get_capabilities(struct zmk_runtime_capabilities *capabilities) {
    if (!capabilities) {
        return;
    }

    memset(capabilities, 0, sizeof(*capabilities));
    capabilities->persistence_schema_version = ZMK_RUNTIME_CONFIG_PERSISTENCE_SCHEMA_VERSION;
    fill_capability_fingerprint(capabilities->capability_fingerprint);
    capabilities->max_objects = CONFIG_ZMK_RUNTIME_MAX_OBJECTS;
    capabilities->max_combos = CONFIG_ZMK_RUNTIME_MAX_COMBOS;
    capabilities->max_combo_keys = CONFIG_ZMK_RUNTIME_MAX_COMBO_KEYS;
    capabilities->max_macro_steps = CONFIG_ZMK_RUNTIME_MAX_MACRO_STEPS;
    capabilities->max_tap_dance_actions = CONFIG_ZMK_RUNTIME_MAX_TAP_DANCE_ACTIONS;
    capabilities->max_persisted_bytes = CONFIG_ZMK_RUNTIME_MAX_PERSISTED_BYTES;
    capabilities->max_keymap_overrides = CONFIG_ZMK_RUNTIME_MAX_KEYMAP_OVERRIDES;
}

void zmk_runtime_config_init_empty_snapshot(struct zmk_runtime_config_snapshot *snapshot) {
    struct zmk_runtime_capabilities capabilities;

    if (!snapshot) {
        return;
    }

    zmk_runtime_config_get_capabilities(&capabilities);
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->persistence_schema_version = capabilities.persistence_schema_version;
    memcpy(snapshot->capability_fingerprint, capabilities.capability_fingerprint,
           sizeof(snapshot->capability_fingerprint));
}

static const struct zmk_runtime_object_slot *find_object(
    const struct zmk_runtime_object_slot *objects, size_t count,
    zmk_runtime_object_id_t object_id) {
    if (object_id == ZMK_RUNTIME_OBJECT_ID_INVALID) {
        return NULL;
    }

    for (size_t i = 0; i < count; i++) {
        if (objects[i].id == object_id) {
            return &objects[i];
        }
    }

    return NULL;
}

static int compiled_action_to_binding(const struct zmk_runtime_action_ref *action,
                                      struct zmk_behavior_binding *binding) {
    const char *behavior_name;

    if (!action || !binding || action->kind != ZMK_RUNTIME_ACTION_COMPILED_BEHAVIOR ||
        action->data.compiled.local_id == 0U) {
        return -EINVAL;
    }

    behavior_name =
        zmk_behavior_find_behavior_name_from_local_id(action->data.compiled.local_id);
    if (!behavior_name) {
        return -ENODEV;
    }

    /* Runtime objects must use the typed ActionRef variant so their IDs are validated. */
    if (strcmp(behavior_name, ZMK_RUNTIME_DISPATCH_BEHAVIOR_NAME) == 0) {
        return -EINVAL;
    }

    *binding = (struct zmk_behavior_binding){
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
        .local_id = action->data.compiled.local_id,
#endif
        .behavior_dev = behavior_name,
        .param1 = action->data.compiled.param1,
        .param2 = action->data.compiled.param2,
    };

    return zmk_behavior_validate_binding(binding);
}

static int object_action_to_binding(zmk_runtime_object_id_t object_id,
                                    struct zmk_behavior_binding *binding) {
    const char *behavior_name = ZMK_RUNTIME_DISPATCH_BEHAVIOR_NAME;

    if (object_id == ZMK_RUNTIME_OBJECT_ID_INVALID || !binding ||
        !zmk_behavior_get_binding(behavior_name)) {
        return -ENODEV;
    }

    *binding = (struct zmk_behavior_binding){
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
        .local_id = zmk_behavior_get_local_id(behavior_name),
#endif
        .behavior_dev = behavior_name,
        .param1 = object_id,
    };

    return zmk_behavior_validate_binding(binding);
}

static int action_to_binding(const struct zmk_runtime_action_ref *action,
                             const struct zmk_runtime_object_slot *objects, size_t object_count,
                             bool allow_runtime_object, struct zmk_behavior_binding *binding) {
    if (!action || !binding) {
        return -EINVAL;
    }

    if (action->kind == ZMK_RUNTIME_ACTION_COMPILED_BEHAVIOR) {
        return compiled_action_to_binding(action, binding);
    }

    if (allow_runtime_object && action->kind == ZMK_RUNTIME_ACTION_OBJECT &&
        find_object(objects, object_count, action->data.object_id)) {
        return object_action_to_binding(action->data.object_id, binding);
    }

    return -EINVAL;
}

static bool action_refs_equal(const struct zmk_runtime_action_ref *left,
                              const struct zmk_runtime_action_ref *right) {
    if (!left || !right || left->kind != right->kind) {
        return false;
    }

    if (left->kind == ZMK_RUNTIME_ACTION_COMPILED_BEHAVIOR) {
        return left->data.compiled.local_id == right->data.compiled.local_id &&
               left->data.compiled.param1 == right->data.compiled.param1 &&
               left->data.compiled.param2 == right->data.compiled.param2;
    }

    return left->kind == ZMK_RUNTIME_ACTION_OBJECT &&
           left->data.object_id == right->data.object_id;
}

static int validate_macro_step(const struct zmk_runtime_macro_step *step) {
    struct zmk_behavior_binding binding;

    if (!step) {
        return -EINVAL;
    }

    switch (step->type) {
    case ZMK_RUNTIME_MACRO_STEP_TAP:
    case ZMK_RUNTIME_MACRO_STEP_PRESS:
    case ZMK_RUNTIME_MACRO_STEP_RELEASE:
        return action_to_binding(&step->data.action, NULL, 0U, false, &binding);
    case ZMK_RUNTIME_MACRO_STEP_WAIT:
    case ZMK_RUNTIME_MACRO_STEP_PAUSE_UNTIL_RELEASE:
        return 0;
    default:
        return -EINVAL;
    }
}

static int validate_macro_press_release_balance(const struct zmk_runtime_macro_step *steps,
                                                size_t step_count) {
    for (size_t i = 0; i < step_count; i++) {
        size_t presses = 0U;
        size_t releases = 0U;

        if (steps[i].type != ZMK_RUNTIME_MACRO_STEP_PRESS &&
            steps[i].type != ZMK_RUNTIME_MACRO_STEP_RELEASE) {
            continue;
        }

        for (size_t previous = 0; previous <= i; previous++) {
            if (!action_refs_equal(&steps[i].data.action, &steps[previous].data.action)) {
                continue;
            }

            if (steps[previous].type == ZMK_RUNTIME_MACRO_STEP_PRESS) {
                presses++;
            } else if (steps[previous].type == ZMK_RUNTIME_MACRO_STEP_RELEASE) {
                releases++;
            }
        }

        if (releases > presses) {
            return -EINVAL;
        }
    }

    for (size_t i = 0; i < step_count; i++) {
        size_t presses = 0U;
        size_t releases = 0U;

        if (steps[i].type != ZMK_RUNTIME_MACRO_STEP_PRESS) {
            continue;
        }

        for (size_t candidate = 0; candidate < step_count; candidate++) {
            if (!action_refs_equal(&steps[i].data.action, &steps[candidate].data.action)) {
                continue;
            }

            if (steps[candidate].type == ZMK_RUNTIME_MACRO_STEP_PRESS) {
                presses++;
            } else if (steps[candidate].type == ZMK_RUNTIME_MACRO_STEP_RELEASE) {
                releases++;
            }
        }

        if (presses != releases) {
            return -EINVAL;
        }
    }

    return 0;
}

static int validate_object(const struct zmk_runtime_object_slot *object,
                           const struct zmk_runtime_macro_step *macro_steps,
                           size_t macro_step_count,
                           const struct zmk_runtime_tap_dance_action *tap_dance_actions,
                           size_t tap_dance_action_count) {
    struct zmk_behavior_binding binding;

    if (!object || object->id == ZMK_RUNTIME_OBJECT_ID_INVALID) {
        return -EINVAL;
    }

    switch (object->type) {
    case ZMK_RUNTIME_OBJECT_TYPE_MOD_MORPH:
        if (object->data.mod_morph.modifiers == 0U ||
            action_to_binding(&object->data.mod_morph.normal_action, NULL, 0U, false,
                              &binding) != 0 ||
            action_to_binding(&object->data.mod_morph.morphed_action, NULL, 0U, false,
                              &binding) != 0) {
            return -EINVAL;
        }
        return 0;
    case ZMK_RUNTIME_OBJECT_TYPE_MACRO: {
        size_t offset = object->data.macro.step_offset;
        size_t count = object->data.macro.step_count;

        if (!macro_steps || count == 0U || offset > macro_step_count ||
            count > macro_step_count - offset) {
            return -EINVAL;
        }

        for (size_t i = 0; i < count; i++) {
            if (validate_macro_step(&macro_steps[offset + i]) != 0) {
                return -EINVAL;
            }
        }

        return validate_macro_press_release_balance(&macro_steps[offset], count);
    }
    case ZMK_RUNTIME_OBJECT_TYPE_HOLD_TAP: {
        const struct zmk_runtime_hold_tap_config *config = &object->data.hold_tap;

        if (config->flavor < ZMK_RUNTIME_HOLD_TAP_FLAVOR_HOLD_PREFERRED ||
            config->flavor > ZMK_RUNTIME_HOLD_TAP_FLAVOR_TAP_UNLESS_INTERRUPTED ||
            config->tapping_term_ms == 0U || config->tapping_term_ms > INT32_MAX ||
            config->quick_tap_ms > INT32_MAX || config->require_prior_idle_ms > INT32_MAX ||
            action_to_binding(&config->tap_action, NULL, 0U, false, &binding) != 0 ||
            action_to_binding(&config->hold_action, NULL, 0U, false, &binding) != 0) {
            return -EINVAL;
        }

        return 0;
    }
    case ZMK_RUNTIME_OBJECT_TYPE_TAP_DANCE: {
        const struct zmk_runtime_tap_dance_config *config = &object->data.tap_dance;

        if (!tap_dance_actions || config->action_count == 0U ||
            config->action_offset > tap_dance_action_count ||
            config->action_count > tap_dance_action_count - config->action_offset ||
            config->tapping_term_ms == 0U || config->tapping_term_ms > INT32_MAX) {
            return -EINVAL;
        }

        for (size_t i = 0; i < config->action_count; i++) {
            const struct zmk_runtime_tap_dance_action *action =
                &tap_dance_actions[config->action_offset + i];

            if (action_to_binding(&action->tap_action, NULL, 0U, false, &binding) != 0 ||
                action_to_binding(&action->hold_action, NULL, 0U, false, &binding) != 0) {
                return -EINVAL;
            }
        }

        return 0;
    }
    default:
        return -ENOTSUP;
    }
}

static bool macro_ranges_overlap(const struct zmk_runtime_macro_config *left,
                                 const struct zmk_runtime_macro_config *right) {
    size_t left_start = left->step_offset;
    size_t left_end = left_start + left->step_count;
    size_t right_start = right->step_offset;
    size_t right_end = right_start + right->step_count;

    return left_start < right_end && right_start < left_end;
}

static bool tap_dance_ranges_overlap(const struct zmk_runtime_tap_dance_config *left,
                                     const struct zmk_runtime_tap_dance_config *right) {
    size_t left_start = left->action_offset;
    size_t left_end = left_start + left->action_count;
    size_t right_start = right->action_offset;
    size_t right_end = right_start + right->action_count;

    return left_start < right_end && right_start < left_end;
}

static int validate_objects(const struct zmk_runtime_object_slot *objects, size_t count,
                            const struct zmk_runtime_macro_step *macro_steps,
                            size_t macro_step_count,
                            const struct zmk_runtime_tap_dance_action *tap_dance_actions,
                            size_t tap_dance_action_count) {
    size_t referenced_macro_steps = 0U;
    size_t referenced_tap_dance_actions = 0U;

    if ((!objects && count != 0U) || (!macro_steps && macro_step_count != 0U) ||
        (!tap_dance_actions && tap_dance_action_count != 0U) ||
        count > CONFIG_ZMK_RUNTIME_MAX_OBJECTS ||
        macro_step_count > CONFIG_ZMK_RUNTIME_MAX_MACRO_STEPS ||
        tap_dance_action_count > CONFIG_ZMK_RUNTIME_MAX_TAP_DANCE_ACTIONS) {
        return -EINVAL;
    }

    for (size_t i = 0; i < count; i++) {
        int ret = validate_object(&objects[i], macro_steps, macro_step_count, tap_dance_actions,
                                  tap_dance_action_count);
        if (ret != 0) {
            return ret;
        }

        for (size_t previous = 0; previous < i; previous++) {
            if (objects[previous].id == objects[i].id) {
                return -EEXIST;
            }

            if (objects[i].type == ZMK_RUNTIME_OBJECT_TYPE_MACRO &&
                objects[previous].type == ZMK_RUNTIME_OBJECT_TYPE_MACRO &&
                macro_ranges_overlap(&objects[i].data.macro, &objects[previous].data.macro)) {
                return -EINVAL;
            }

            if (objects[i].type == ZMK_RUNTIME_OBJECT_TYPE_TAP_DANCE &&
                objects[previous].type == ZMK_RUNTIME_OBJECT_TYPE_TAP_DANCE &&
                tap_dance_ranges_overlap(&objects[i].data.tap_dance,
                                         &objects[previous].data.tap_dance)) {
                return -EINVAL;
            }
        }

        if (objects[i].type == ZMK_RUNTIME_OBJECT_TYPE_MACRO) {
            referenced_macro_steps += objects[i].data.macro.step_count;
        }
        if (objects[i].type == ZMK_RUNTIME_OBJECT_TYPE_TAP_DANCE) {
            referenced_tap_dance_actions += objects[i].data.tap_dance.action_count;
        }
    }

    return referenced_macro_steps == macro_step_count &&
                   referenced_tap_dance_actions == tap_dance_action_count
               ? 0
               : -EINVAL;
}

static int validate_combo(const struct zmk_runtime_combo_slot *combo,
                          const struct zmk_runtime_object_slot *objects, size_t object_count) {
    struct zmk_behavior_binding binding;

    if (!combo || combo->id == ZMK_RUNTIME_OBJECT_ID_INVALID || combo->key_count == 0U ||
        combo->key_count > CONFIG_ZMK_RUNTIME_MAX_COMBO_KEYS || combo->timeout_ms == 0U ||
        combo->timeout_ms > INT32_MAX || combo->require_prior_idle_ms > INT32_MAX ||
        action_to_binding(&combo->output, objects, object_count, true, &binding) != 0) {
        return -EINVAL;
    }

    for (size_t i = 0; i < combo->key_count; i++) {
        if (combo->positions[i] >= ZMK_KEYMAP_LEN) {
            return -EINVAL;
        }

        for (size_t previous = 0; previous < i; previous++) {
            if (combo->positions[previous] == combo->positions[i]) {
                return -EEXIST;
            }
        }
    }

    return 0;
}

static int validate_combos(const struct zmk_runtime_combo_slot *combos, size_t combo_count,
                           const struct zmk_runtime_object_slot *objects, size_t object_count) {
    if ((!combos && combo_count != 0U) || combo_count > CONFIG_ZMK_RUNTIME_MAX_COMBOS) {
        return -EINVAL;
    }

    for (size_t i = 0; i < combo_count; i++) {
        int ret = validate_combo(&combos[i], objects, object_count);

        if (ret != 0) {
            return ret;
        }

        for (size_t previous = 0; previous < i; previous++) {
            if (combos[previous].id == combos[i].id) {
                return -EEXIST;
            }
        }
    }

    return 0;
}

int zmk_runtime_config_stage_snapshot(const struct zmk_runtime_config_snapshot *snapshot,
                                      struct zmk_runtime_config_validation_result *result) {
    int ret = zmk_runtime_config_validate_snapshot(snapshot, result);

    if (ret != 0) {
        return ret;
    }

    staged_config.snapshot = *snapshot;
    staged_config.present = true;
    return 0;
}

const struct zmk_runtime_config_snapshot *zmk_runtime_config_staged_snapshot(void) {
    return staged_config.present ? &staged_config.snapshot : NULL;
}

int zmk_runtime_config_begin_update(uint32_t expected_active_generation, size_t snapshot_size,
                                    uint32_t *update_id) {
    struct zmk_runtime_config_activation_status activation;

    if (!update_id || snapshot_size == 0U) {
        return -EINVAL;
    }

    zmk_runtime_config_get_activation_status(&activation);
    if (expected_active_generation != activation.active_generation) {
        return -ESTALE;
    }

    if (staged_config.update.active || pending_config.present) {
        return -EBUSY;
    }

    if (snapshot_size > sizeof(staged_config.pool.serialized_bytes)) {
        return -ENOSPC;
    }

    staged_config.next_update_id++;
    if (staged_config.next_update_id == 0U) {
        staged_config.next_update_id++;
    }

    staged_config.update.active = true;
    staged_config.update.id = staged_config.next_update_id;
    staged_config.update.expected_size = snapshot_size;
    staged_config.update.received_size = 0U;
    staged_config.pool.keymap_override_count = 0U;
    staged_config.pool.object_count = 0U;
    staged_config.pool.combo_count = 0U;
    staged_config.pool.macro_step_count = 0U;
    staged_config.pool.tap_dance_action_count = 0U;
    memset(staged_config.pool.keymap_overrides, 0, sizeof(staged_config.pool.keymap_overrides));
    memset(staged_config.pool.objects, 0, sizeof(staged_config.pool.objects));
    memset(staged_config.pool.combos, 0, sizeof(staged_config.pool.combos));
    memset(staged_config.pool.macro_steps, 0, sizeof(staged_config.pool.macro_steps));
    memset(staged_config.pool.tap_dance_actions, 0,
           sizeof(staged_config.pool.tap_dance_actions));
    *update_id = staged_config.update.id;

    return 0;
}

int zmk_runtime_config_upload_update_chunk(uint32_t update_id, size_t offset, const uint8_t *chunk,
                                           size_t chunk_size, size_t *accepted_bytes,
                                           size_t *next_offset) {
    if (!accepted_bytes || !next_offset || !chunk || chunk_size == 0U) {
        return -EINVAL;
    }

    if (!staged_config.update.active || staged_config.update.id != update_id) {
        return -ENOENT;
    }

    if (chunk_size > CONFIG_ZMK_RUNTIME_CONFIG_RPC_MAX_CHUNK_BYTES) {
        return -EMSGSIZE;
    }

    if (offset != staged_config.update.received_size) {
        return -EINVAL;
    }

    if (chunk_size > staged_config.update.expected_size - offset) {
        return -EFBIG;
    }

    memcpy(&staged_config.pool.serialized_bytes[offset], chunk, chunk_size);
    staged_config.update.received_size += chunk_size;
    *accepted_bytes = chunk_size;
    *next_offset = staged_config.update.received_size;

    return 0;
}

int zmk_runtime_config_get_uploaded_snapshot(uint32_t update_id, const uint8_t **snapshot_bytes,
                                             size_t *snapshot_size) {
    if (!snapshot_bytes || !snapshot_size) {
        return -EINVAL;
    }

    if (!staged_config.update.active || staged_config.update.id != update_id) {
        return -ENOENT;
    }

    if (staged_config.update.received_size != staged_config.update.expected_size) {
        return -EAGAIN;
    }

    *snapshot_bytes = staged_config.pool.serialized_bytes;
    *snapshot_size = staged_config.update.expected_size;

    return 0;
}

int zmk_runtime_config_stage_uploaded_snapshot(
    uint32_t update_id, const struct zmk_runtime_config_snapshot *snapshot,
    struct zmk_runtime_config_validation_result *result) {
    const uint8_t *serialized_snapshot;
    size_t serialized_size;
    int ret;

    ret =
        zmk_runtime_config_get_uploaded_snapshot(update_id, &serialized_snapshot, &serialized_size);
    if (ret != 0) {
        return ret;
    }

    (void)serialized_snapshot;
    (void)serialized_size;

    ret = zmk_runtime_config_stage_snapshot(snapshot, result);
    if (ret == 0 && (snapshot->keymap_override_count != staged_config.pool.keymap_override_count ||
                     snapshot->object_count != staged_config.pool.object_count ||
                     snapshot->combo_count != staged_config.pool.combo_count ||
                     snapshot->macro_step_count != staged_config.pool.macro_step_count ||
                     snapshot->tap_dance_action_count !=
                         staged_config.pool.tap_dance_action_count)) {
        if (result) {
            result->valid = false;
            result->error = ZMK_RUNTIME_CONFIG_ERROR_INVALID_ARGUMENT;
        }
        ret = -EINVAL;
    }

    if (ret == 0) {
        ret = validate_objects(staged_config.pool.objects, staged_config.pool.object_count,
                               staged_config.pool.macro_steps,
                               staged_config.pool.macro_step_count,
                               staged_config.pool.tap_dance_actions,
                               staged_config.pool.tap_dance_action_count);
        if (ret != 0 && result) {
            result->valid = false;
            result->error = ZMK_RUNTIME_CONFIG_ERROR_INVALID_ARGUMENT;
        }
    }

    if (ret == 0) {
        ret = validate_combos(staged_config.pool.combos, staged_config.pool.combo_count,
                              staged_config.pool.objects, staged_config.pool.object_count);
        if (ret != 0 && result) {
            result->valid = false;
            result->error = ZMK_RUNTIME_CONFIG_ERROR_INVALID_ARGUMENT;
        }
    }

    if (ret == 0) {
        for (size_t i = 0; i < staged_config.pool.keymap_override_count; i++) {
            const struct zmk_runtime_keymap_override *override =
                &staged_config.pool.keymap_overrides[i];
            struct zmk_behavior_binding binding;

            if (override->layer_id >= ZMK_KEYMAP_LAYERS_LEN ||
                override->key_position >= ZMK_KEYMAP_LEN ||
                action_to_binding(&override->action, staged_config.pool.objects,
                                  staged_config.pool.object_count, true, &binding) != 0) {
                if (result) {
                    result->valid = false;
                    result->error = ZMK_RUNTIME_CONFIG_ERROR_INVALID_ARGUMENT;
                }
                ret = -EINVAL;
                break;
            }

            for (size_t previous = 0; previous < i; previous++) {
                const struct zmk_runtime_keymap_override *prior =
                    &staged_config.pool.keymap_overrides[previous];

                if (prior->layer_id == override->layer_id &&
                    prior->key_position == override->key_position) {
                    if (result) {
                        result->valid = false;
                        result->error = ZMK_RUNTIME_CONFIG_ERROR_INVALID_ARGUMENT;
                    }
                    ret = -EEXIST;
                    break;
                }
            }

            if (ret != 0) {
                break;
            }
        }
    }
    staged_config.update.validated = ret == 0;

    return ret;
}

int zmk_runtime_config_set_staged_keymap_overrides(
    uint32_t update_id, const struct zmk_runtime_keymap_override *overrides, size_t count) {
    if ((!overrides && count != 0U) || count > CONFIG_ZMK_RUNTIME_MAX_KEYMAP_OVERRIDES) {
        return -EINVAL;
    }

    if (!staged_config.update.active || staged_config.update.id != update_id) {
        return -ENOENT;
    }

    if (count != 0U) {
        memcpy(staged_config.pool.keymap_overrides, overrides,
               count * sizeof(staged_config.pool.keymap_overrides[0]));
    }
    staged_config.pool.keymap_override_count = count;
    return 0;
}

int zmk_runtime_config_append_staged_keymap_override(
    uint32_t update_id, const struct zmk_runtime_keymap_override *override) {
    uint16_t index;

    if (!override) {
        return -EINVAL;
    }

    if (!staged_config.update.active || staged_config.update.id != update_id) {
        return -ENOENT;
    }

    index = staged_config.pool.keymap_override_count;
    if (index >= CONFIG_ZMK_RUNTIME_MAX_KEYMAP_OVERRIDES) {
        return -ENOSPC;
    }

    staged_config.pool.keymap_overrides[index] = *override;
    staged_config.pool.keymap_override_count++;
    return 0;
}

int zmk_runtime_config_set_staged_objects(uint32_t update_id,
                                          const struct zmk_runtime_object_slot *objects,
                                          size_t count) {
    if ((!objects && count != 0U) || count > CONFIG_ZMK_RUNTIME_MAX_OBJECTS) {
        return -EINVAL;
    }

    if (!staged_config.update.active || staged_config.update.id != update_id) {
        return -ENOENT;
    }

    if (count != 0U) {
        memcpy(staged_config.pool.objects, objects,
               count * sizeof(staged_config.pool.objects[0]));
    }
    staged_config.pool.object_count = count;
    return 0;
}

int zmk_runtime_config_append_staged_object(uint32_t update_id,
                                            const struct zmk_runtime_object_slot *object) {
    uint16_t index;

    if (!object) {
        return -EINVAL;
    }

    if (!staged_config.update.active || staged_config.update.id != update_id) {
        return -ENOENT;
    }

    index = staged_config.pool.object_count;
    if (index >= CONFIG_ZMK_RUNTIME_MAX_OBJECTS) {
        return -ENOSPC;
    }

    staged_config.pool.objects[index] = *object;
    staged_config.pool.object_count++;
    return 0;
}

int zmk_runtime_config_set_staged_combos(uint32_t update_id,
                                         const struct zmk_runtime_combo_slot *combos,
                                         size_t count) {
    if ((!combos && count != 0U) || count > CONFIG_ZMK_RUNTIME_MAX_COMBOS) {
        return -EINVAL;
    }

    if (!staged_config.update.active || staged_config.update.id != update_id) {
        return -ENOENT;
    }

    if (count != 0U) {
        memcpy(staged_config.pool.combos, combos, count * sizeof(staged_config.pool.combos[0]));
    }
    staged_config.pool.combo_count = count;
    return 0;
}

int zmk_runtime_config_append_staged_combo(uint32_t update_id,
                                           const struct zmk_runtime_combo_slot *combo) {
    uint16_t index;

    if (!combo) {
        return -EINVAL;
    }

    if (!staged_config.update.active || staged_config.update.id != update_id) {
        return -ENOENT;
    }

    index = staged_config.pool.combo_count;
    if (index >= CONFIG_ZMK_RUNTIME_MAX_COMBOS) {
        return -ENOSPC;
    }

    staged_config.pool.combos[index] = *combo;
    staged_config.pool.combo_count++;
    return 0;
}

int zmk_runtime_config_set_staged_macro_steps(uint32_t update_id,
                                              const struct zmk_runtime_macro_step *steps,
                                              size_t count) {
    if ((!steps && count != 0U) || count > CONFIG_ZMK_RUNTIME_MAX_MACRO_STEPS) {
        return -EINVAL;
    }

    if (!staged_config.update.active || staged_config.update.id != update_id) {
        return -ENOENT;
    }

    if (count != 0U) {
        memcpy(staged_config.pool.macro_steps, steps,
               count * sizeof(staged_config.pool.macro_steps[0]));
    }
    staged_config.pool.macro_step_count = count;
    return 0;
}

int zmk_runtime_config_append_staged_macro_step(uint32_t update_id,
                                                const struct zmk_runtime_macro_step *step) {
    uint16_t index;

    if (!step) {
        return -EINVAL;
    }

    if (!staged_config.update.active || staged_config.update.id != update_id) {
        return -ENOENT;
    }

    index = staged_config.pool.macro_step_count;
    if (index >= CONFIG_ZMK_RUNTIME_MAX_MACRO_STEPS) {
        return -ENOSPC;
    }

    staged_config.pool.macro_steps[index] = *step;
    staged_config.pool.macro_step_count++;
    return 0;
}

int zmk_runtime_config_append_staged_tap_dance_action(
    uint32_t update_id, const struct zmk_runtime_tap_dance_action *action) {
    uint16_t index;

    if (!action) {
        return -EINVAL;
    }

    if (!staged_config.update.active || staged_config.update.id != update_id) {
        return -ENOENT;
    }

    index = staged_config.pool.tap_dance_action_count;
    if (index >= CONFIG_ZMK_RUNTIME_MAX_TAP_DANCE_ACTIONS) {
        return -ENOSPC;
    }

    staged_config.pool.tap_dance_actions[index] = *action;
    staged_config.pool.tap_dance_action_count++;
    return 0;
}

int zmk_runtime_config_get_validated_uploaded_snapshot(uint32_t update_id,
                                                       const uint8_t **snapshot_bytes,
                                                       size_t *snapshot_size) {
    int ret = zmk_runtime_config_get_uploaded_snapshot(update_id, snapshot_bytes, snapshot_size);

    if (ret != 0) {
        return ret;
    }

    return staged_config.update.validated ? 0 : -EINVAL;
}

static struct runtime_config_persisted_action_ref persist_action(
    const struct zmk_runtime_action_ref *action) {
    return (struct runtime_config_persisted_action_ref){
        .kind = action->kind,
        .local_id = action->kind == ZMK_RUNTIME_ACTION_COMPILED_BEHAVIOR
                        ? action->data.compiled.local_id
                        : 0U,
        .param1 = action->kind == ZMK_RUNTIME_ACTION_COMPILED_BEHAVIOR
                      ? action->data.compiled.param1
                      : 0U,
        .param2 = action->kind == ZMK_RUNTIME_ACTION_COMPILED_BEHAVIOR
                      ? action->data.compiled.param2
                      : 0U,
        .object_id = action->kind == ZMK_RUNTIME_ACTION_OBJECT ? action->data.object_id : 0U,
    };
}

static struct zmk_runtime_action_ref
restore_action(const struct runtime_config_persisted_action_ref *action) {
    if (action->kind == ZMK_RUNTIME_ACTION_COMPILED_BEHAVIOR) {
        return (struct zmk_runtime_action_ref){
            .kind = ZMK_RUNTIME_ACTION_COMPILED_BEHAVIOR,
            .data.compiled = {
                .local_id = action->local_id,
                .param1 = action->param1,
                .param2 = action->param2,
            },
        };
    }

    if (action->kind == ZMK_RUNTIME_ACTION_OBJECT) {
        return (struct zmk_runtime_action_ref){
            .kind = ZMK_RUNTIME_ACTION_OBJECT,
            .data.object_id = action->object_id,
        };
    }

    return (struct zmk_runtime_action_ref){0};
}

int zmk_runtime_config_get_persistable_update_size(uint32_t update_id, size_t *snapshot_size) {
    struct runtime_config_persisted_payload_header header = {
        .magic = RUNTIME_CONFIG_PERSISTED_PAYLOAD_MAGIC,
        .version = RUNTIME_CONFIG_PERSISTED_PAYLOAD_VERSION,
        .keymap_override_count = staged_config.pool.keymap_override_count,
        .object_count = staged_config.pool.object_count,
        .combo_count = staged_config.pool.combo_count,
        .macro_step_count = staged_config.pool.macro_step_count,
        .tap_dance_action_count = staged_config.pool.tap_dance_action_count,
    };
    if (!snapshot_size || !staged_config.update.active || staged_config.update.id != update_id ||
        !staged_config.update.validated) {
        return -EINVAL;
    }

    *snapshot_size = sizeof(header) +
                     header.keymap_override_count *
                         sizeof(struct runtime_config_persisted_keymap_override) +
                     header.object_count * sizeof(struct runtime_config_persisted_object) +
                     header.combo_count * sizeof(struct runtime_config_persisted_combo) +
                     header.macro_step_count * sizeof(struct runtime_config_persisted_macro_step) +
                     header.tap_dance_action_count *
                         sizeof(struct runtime_config_persisted_tap_dance_action);
    return *snapshot_size > sizeof(staged_config.pool.serialized_bytes) ? -ENOSPC : 0;
}

int zmk_runtime_config_get_persistable_update(uint32_t update_id, const uint8_t **snapshot_bytes,
                                              size_t *snapshot_size) {
    struct runtime_config_persisted_payload_header header = {
        .magic = RUNTIME_CONFIG_PERSISTED_PAYLOAD_MAGIC,
        .version = RUNTIME_CONFIG_PERSISTED_PAYLOAD_VERSION,
        .keymap_override_count = staged_config.pool.keymap_override_count,
        .object_count = staged_config.pool.object_count,
        .combo_count = staged_config.pool.combo_count,
        .macro_step_count = staged_config.pool.macro_step_count,
        .tap_dance_action_count = staged_config.pool.tap_dance_action_count,
    };
    size_t payload_size;
    int ret;

    if (!snapshot_bytes) {
        return -EINVAL;
    }

    ret = zmk_runtime_config_get_persistable_update_size(update_id, &payload_size);
    if (ret != 0) {
        return ret;
    }

    if (payload_size > sizeof(staged_config.pool.serialized_bytes)) {
        return -ENOSPC;
    }

    memcpy(staged_config.pool.serialized_bytes, &header, sizeof(header));
    for (size_t i = 0; i < header.keymap_override_count; i++) {
        const struct zmk_runtime_keymap_override *source =
            &staged_config.pool.keymap_overrides[i];
        struct runtime_config_persisted_keymap_override destination = {
            .layer_id = source->layer_id,
            .key_position = source->key_position,
            .action = persist_action(&source->action),
        };

        memcpy(staged_config.pool.serialized_bytes +
                   sizeof(header) + i * sizeof(destination),
               &destination, sizeof(destination));
    }

    for (size_t i = 0; i < header.object_count; i++) {
        const struct zmk_runtime_object_slot *source = &staged_config.pool.objects[i];
        struct runtime_config_persisted_object destination = {
            .id = source->id,
            .type = source->type,
        };
        size_t offset = sizeof(header) +
                        header.keymap_override_count *
                            sizeof(struct runtime_config_persisted_keymap_override) +
                        i * sizeof(destination);

        switch (source->type) {
        case ZMK_RUNTIME_OBJECT_TYPE_MOD_MORPH:
            destination.data.mod_morph = (struct runtime_config_persisted_mod_morph){
                .modifiers = source->data.mod_morph.modifiers,
                .normal_action = persist_action(&source->data.mod_morph.normal_action),
                .morphed_action = persist_action(&source->data.mod_morph.morphed_action),
            };
            break;
        case ZMK_RUNTIME_OBJECT_TYPE_MACRO:
            destination.data.macro = (struct runtime_config_persisted_macro){
                .step_offset = source->data.macro.step_offset,
                .step_count = source->data.macro.step_count,
            };
            break;
        case ZMK_RUNTIME_OBJECT_TYPE_HOLD_TAP:
            destination.data.hold_tap = (struct runtime_config_persisted_hold_tap){
                .tap_action = persist_action(&source->data.hold_tap.tap_action),
                .hold_action = persist_action(&source->data.hold_tap.hold_action),
                .flavor = source->data.hold_tap.flavor,
                .tapping_term_ms = source->data.hold_tap.tapping_term_ms,
                .quick_tap_ms = source->data.hold_tap.quick_tap_ms,
                .require_prior_idle_ms = source->data.hold_tap.require_prior_idle_ms,
            };
            break;
        case ZMK_RUNTIME_OBJECT_TYPE_TAP_DANCE:
            destination.data.tap_dance = (struct runtime_config_persisted_tap_dance){
                .action_offset = source->data.tap_dance.action_offset,
                .action_count = source->data.tap_dance.action_count,
                .tapping_term_ms = source->data.tap_dance.tapping_term_ms,
            };
            break;
        default:
            return -EINVAL;
        }

        memcpy(staged_config.pool.serialized_bytes + offset, &destination, sizeof(destination));
    }

    for (size_t i = 0; i < header.combo_count; i++) {
        const struct zmk_runtime_combo_slot *source = &staged_config.pool.combos[i];
        struct runtime_config_persisted_combo destination = {
            .id = source->id,
            .key_count = source->key_count,
            .timeout_ms = source->timeout_ms,
            .require_prior_idle_ms = source->require_prior_idle_ms,
            .slow_release = source->slow_release,
            .output = persist_action(&source->output),
        };
        size_t offset = sizeof(header) +
                        header.keymap_override_count *
                            sizeof(struct runtime_config_persisted_keymap_override) +
                        header.object_count * sizeof(struct runtime_config_persisted_object) +
                        i * sizeof(destination);

        memcpy(destination.positions, source->positions, sizeof(destination.positions));
        memcpy(staged_config.pool.serialized_bytes + offset, &destination, sizeof(destination));
    }

    for (size_t i = 0; i < header.macro_step_count; i++) {
        const struct zmk_runtime_macro_step *source = &staged_config.pool.macro_steps[i];
        struct runtime_config_persisted_macro_step destination = {
            .type = source->type,
        };
        size_t offset = sizeof(header) +
                        header.keymap_override_count *
                            sizeof(struct runtime_config_persisted_keymap_override) +
                        header.object_count * sizeof(struct runtime_config_persisted_object) +
                        header.combo_count * sizeof(struct runtime_config_persisted_combo) +
                        i * sizeof(destination);

        if (source->type == ZMK_RUNTIME_MACRO_STEP_TAP ||
            source->type == ZMK_RUNTIME_MACRO_STEP_PRESS ||
            source->type == ZMK_RUNTIME_MACRO_STEP_RELEASE) {
            destination.data.action = persist_action(&source->data.action);
        } else if (source->type == ZMK_RUNTIME_MACRO_STEP_WAIT) {
            destination.data.duration_ms = source->data.duration_ms;
        }

        memcpy(staged_config.pool.serialized_bytes + offset, &destination, sizeof(destination));
    }

    for (size_t i = 0; i < header.tap_dance_action_count; i++) {
        const struct zmk_runtime_tap_dance_action *source =
            &staged_config.pool.tap_dance_actions[i];
        struct runtime_config_persisted_tap_dance_action destination = {
            .tap_action = persist_action(&source->tap_action),
            .hold_action = persist_action(&source->hold_action),
        };
        size_t offset = sizeof(header) +
                        header.keymap_override_count *
                            sizeof(struct runtime_config_persisted_keymap_override) +
                        header.object_count * sizeof(struct runtime_config_persisted_object) +
                        header.combo_count * sizeof(struct runtime_config_persisted_combo) +
                        header.macro_step_count *
                            sizeof(struct runtime_config_persisted_macro_step) +
                        i * sizeof(destination);

        memcpy(staged_config.pool.serialized_bytes + offset, &destination, sizeof(destination));
    }

    *snapshot_bytes = staged_config.pool.serialized_bytes;
    *snapshot_size = payload_size;
    return 0;
}

int zmk_runtime_config_abort_update(uint32_t update_id) {
    if (!staged_config.update.active || staged_config.update.id != update_id) {
        return -ENOENT;
    }

    memset(&staged_config.update, 0, sizeof(staged_config.update));
    return 0;
}

static int prepare_runtime_config(const struct zmk_runtime_keymap_override *overrides, size_t count,
                                  const struct zmk_runtime_object_slot *objects, size_t object_count,
                                  const struct zmk_runtime_combo_slot *combos, size_t combo_count,
                                  const struct zmk_runtime_macro_step *macro_steps,
                                  size_t macro_step_count,
                                  const struct zmk_runtime_tap_dance_action *tap_dance_actions,
                                  size_t tap_dance_action_count, uint32_t generation) {
    if (count > CONFIG_ZMK_RUNTIME_MAX_KEYMAP_OVERRIDES ||
        object_count > CONFIG_ZMK_RUNTIME_MAX_OBJECTS ||
        combo_count > CONFIG_ZMK_RUNTIME_MAX_COMBOS ||
        macro_step_count > CONFIG_ZMK_RUNTIME_MAX_MACRO_STEPS ||
        tap_dance_action_count > CONFIG_ZMK_RUNTIME_MAX_TAP_DANCE_ACTIONS) {
        return -ENOSPC;
    }

    if (validate_objects(objects, object_count, macro_steps, macro_step_count, tap_dance_actions,
                         tap_dance_action_count) != 0 ||
        validate_combos(combos, combo_count, objects, object_count) != 0) {
        return -EINVAL;
    }

    memset(&pending_config, 0, sizeof(pending_config));
    pending_config.generation = generation;
    pending_config.keymap_override_count = count;
    pending_config.object_count = object_count;
    pending_config.combo_count = combo_count;
    pending_config.macro_step_count = macro_step_count;
    pending_config.tap_dance_action_count = tap_dance_action_count;
    if (object_count != 0U) {
        memcpy(pending_config.objects, objects, object_count * sizeof(pending_config.objects[0]));
    }
    if (macro_step_count != 0U) {
        memcpy(pending_config.macro_steps, macro_steps,
               macro_step_count * sizeof(pending_config.macro_steps[0]));
    }
    if (combo_count != 0U) {
        memcpy(pending_config.combos, combos, combo_count * sizeof(pending_config.combos[0]));
    }
    if (tap_dance_action_count != 0U) {
        memcpy(pending_config.tap_dance_actions, tap_dance_actions,
               tap_dance_action_count * sizeof(pending_config.tap_dance_actions[0]));
    }

    for (size_t i = 0; i < count; i++) {
        if (overrides[i].layer_id >= ZMK_KEYMAP_LAYERS_LEN ||
            overrides[i].key_position >= ZMK_KEYMAP_LEN) {
            return -EINVAL;
        }

        pending_config.keymap_overrides[i] = overrides[i];
        int ret = action_to_binding(&overrides[i].action, objects, object_count, true,
                                    &pending_config.bindings[i]);
        if (ret != 0) {
            return ret;
        }

        for (size_t previous = 0; previous < i; previous++) {
            if (overrides[previous].layer_id == overrides[i].layer_id &&
                overrides[previous].key_position == overrides[i].key_position) {
                return -EEXIST;
            }
        }
    }

    pending_config.present = true;
    return 0;
}

int zmk_runtime_config_prepare_pending_update(uint32_t update_id, uint32_t generation) {
    if (generation == 0U || !staged_config.update.active || staged_config.update.id != update_id ||
        !staged_config.update.validated) {
        return -EINVAL;
    }

    return prepare_runtime_config(staged_config.pool.keymap_overrides,
                                  staged_config.pool.keymap_override_count,
                                  staged_config.pool.objects, staged_config.pool.object_count,
                                  staged_config.pool.combos, staged_config.pool.combo_count,
                                  staged_config.pool.macro_steps,
                                  staged_config.pool.macro_step_count,
                                  staged_config.pool.tap_dance_actions,
                                  staged_config.pool.tap_dance_action_count,
                                  generation);
}

int zmk_runtime_config_prepare_persisted_generation(const uint8_t *snapshot_bytes,
                                                     size_t snapshot_size, uint32_t generation) {
    struct runtime_config_persisted_payload_header header;
    size_t expected_size;

    if (!snapshot_bytes || snapshot_size < sizeof(header) || generation == 0U) {
        return -EINVAL;
    }

    memcpy(&header, snapshot_bytes, sizeof(header));
    expected_size = sizeof(header) + header.keymap_override_count *
                                       sizeof(struct runtime_config_persisted_keymap_override) +
                    header.object_count * sizeof(struct runtime_config_persisted_object) +
                    header.combo_count * sizeof(struct runtime_config_persisted_combo) +
                    header.macro_step_count * sizeof(struct runtime_config_persisted_macro_step) +
                    header.tap_dance_action_count *
                        sizeof(struct runtime_config_persisted_tap_dance_action);
    if (header.magic != RUNTIME_CONFIG_PERSISTED_PAYLOAD_MAGIC ||
        header.version != RUNTIME_CONFIG_PERSISTED_PAYLOAD_VERSION ||
        header.keymap_override_count > CONFIG_ZMK_RUNTIME_MAX_KEYMAP_OVERRIDES ||
        header.object_count > CONFIG_ZMK_RUNTIME_MAX_OBJECTS ||
        header.combo_count > CONFIG_ZMK_RUNTIME_MAX_COMBOS ||
        header.macro_step_count > CONFIG_ZMK_RUNTIME_MAX_MACRO_STEPS ||
        header.tap_dance_action_count > CONFIG_ZMK_RUNTIME_MAX_TAP_DANCE_ACTIONS ||
        expected_size != snapshot_size) {
        return -EBADMSG;
    }

    for (size_t i = 0; i < header.keymap_override_count; i++) {
        struct runtime_config_persisted_keymap_override source;

        memcpy(&source, snapshot_bytes + sizeof(header) + i * sizeof(source), sizeof(source));
        staged_config.pool.keymap_overrides[i] = (struct zmk_runtime_keymap_override){
            .layer_id = source.layer_id,
            .key_position = source.key_position,
            .action = restore_action(&source.action),
        };
    }

    for (size_t i = 0; i < header.object_count; i++) {
        struct runtime_config_persisted_object source;
        size_t offset = sizeof(header) +
                        header.keymap_override_count *
                            sizeof(struct runtime_config_persisted_keymap_override) +
                        i * sizeof(source);

        memcpy(&source, snapshot_bytes + offset, sizeof(source));
        staged_config.pool.objects[i] = (struct zmk_runtime_object_slot){
            .id = source.id,
            .type = source.type,
        };

        switch (source.type) {
        case ZMK_RUNTIME_OBJECT_TYPE_MOD_MORPH:
            staged_config.pool.objects[i].data.mod_morph = (struct zmk_runtime_mod_morph_config){
                .modifiers = source.data.mod_morph.modifiers,
                .normal_action = restore_action(&source.data.mod_morph.normal_action),
                .morphed_action = restore_action(&source.data.mod_morph.morphed_action),
            };
            break;
        case ZMK_RUNTIME_OBJECT_TYPE_MACRO:
            staged_config.pool.objects[i].data.macro = (struct zmk_runtime_macro_config){
                .step_offset = source.data.macro.step_offset,
                .step_count = source.data.macro.step_count,
            };
            break;
        case ZMK_RUNTIME_OBJECT_TYPE_HOLD_TAP:
            staged_config.pool.objects[i].data.hold_tap =
                (struct zmk_runtime_hold_tap_config){
                    .tap_action = restore_action(&source.data.hold_tap.tap_action),
                    .hold_action = restore_action(&source.data.hold_tap.hold_action),
                    .flavor = source.data.hold_tap.flavor,
                    .tapping_term_ms = source.data.hold_tap.tapping_term_ms,
                    .quick_tap_ms = source.data.hold_tap.quick_tap_ms,
                    .require_prior_idle_ms = source.data.hold_tap.require_prior_idle_ms,
                };
            break;
        case ZMK_RUNTIME_OBJECT_TYPE_TAP_DANCE:
            staged_config.pool.objects[i].data.tap_dance =
                (struct zmk_runtime_tap_dance_config){
                    .action_offset = source.data.tap_dance.action_offset,
                    .action_count = source.data.tap_dance.action_count,
                    .tapping_term_ms = source.data.tap_dance.tapping_term_ms,
                };
            break;
        default:
            return -EBADMSG;
        }
    }

    for (size_t i = 0; i < header.combo_count; i++) {
        struct runtime_config_persisted_combo source;
        size_t offset = sizeof(header) +
                        header.keymap_override_count *
                            sizeof(struct runtime_config_persisted_keymap_override) +
                        header.object_count * sizeof(struct runtime_config_persisted_object) +
                        i * sizeof(source);

        memcpy(&source, snapshot_bytes + offset, sizeof(source));
        staged_config.pool.combos[i] = (struct zmk_runtime_combo_slot){
            .id = source.id,
            .key_count = source.key_count,
            .timeout_ms = source.timeout_ms,
            .require_prior_idle_ms = source.require_prior_idle_ms,
            .slow_release = source.slow_release,
            .output = restore_action(&source.output),
        };
        memcpy(staged_config.pool.combos[i].positions, source.positions,
               sizeof(staged_config.pool.combos[i].positions));
    }

    for (size_t i = 0; i < header.macro_step_count; i++) {
        struct runtime_config_persisted_macro_step source;
        size_t offset = sizeof(header) +
                        header.keymap_override_count *
                            sizeof(struct runtime_config_persisted_keymap_override) +
                        header.object_count * sizeof(struct runtime_config_persisted_object) +
                        header.combo_count * sizeof(struct runtime_config_persisted_combo) +
                        i * sizeof(source);

        memcpy(&source, snapshot_bytes + offset, sizeof(source));
        staged_config.pool.macro_steps[i] = (struct zmk_runtime_macro_step){
            .type = source.type,
        };

        if (source.type == ZMK_RUNTIME_MACRO_STEP_TAP ||
            source.type == ZMK_RUNTIME_MACRO_STEP_PRESS ||
            source.type == ZMK_RUNTIME_MACRO_STEP_RELEASE) {
            staged_config.pool.macro_steps[i].data.action = restore_action(&source.data.action);
        } else if (source.type == ZMK_RUNTIME_MACRO_STEP_WAIT) {
            staged_config.pool.macro_steps[i].data.duration_ms = source.data.duration_ms;
        } else if (source.type != ZMK_RUNTIME_MACRO_STEP_PAUSE_UNTIL_RELEASE) {
            return -EBADMSG;
        }
    }

    for (size_t i = 0; i < header.tap_dance_action_count; i++) {
        struct runtime_config_persisted_tap_dance_action source;
        size_t offset = sizeof(header) +
                        header.keymap_override_count *
                            sizeof(struct runtime_config_persisted_keymap_override) +
                        header.object_count * sizeof(struct runtime_config_persisted_object) +
                        header.combo_count * sizeof(struct runtime_config_persisted_combo) +
                        header.macro_step_count *
                            sizeof(struct runtime_config_persisted_macro_step) +
                        i * sizeof(source);

        memcpy(&source, snapshot_bytes + offset, sizeof(source));
        staged_config.pool.tap_dance_actions[i] = (struct zmk_runtime_tap_dance_action){
            .tap_action = restore_action(&source.tap_action),
            .hold_action = restore_action(&source.hold_action),
        };
    }

    staged_config.pool.keymap_override_count = header.keymap_override_count;
    staged_config.pool.object_count = header.object_count;
    staged_config.pool.combo_count = header.combo_count;
    staged_config.pool.macro_step_count = header.macro_step_count;
    staged_config.pool.tap_dance_action_count = header.tap_dance_action_count;
    return prepare_runtime_config(staged_config.pool.keymap_overrides,
                                  staged_config.pool.keymap_override_count,
                                  staged_config.pool.objects, staged_config.pool.object_count,
                                  staged_config.pool.combos, staged_config.pool.combo_count,
                                  staged_config.pool.macro_steps,
                                  staged_config.pool.macro_step_count,
                                  staged_config.pool.tap_dance_actions,
                                  staged_config.pool.tap_dance_action_count,
                                  generation);
}

int zmk_runtime_config_activate_pending_generation(uint32_t generation) {
    if (!pending_config.present || pending_config.generation != generation) {
        return -ENOENT;
    }

    active_config = pending_config;
    memset(&pending_config, 0, sizeof(pending_config));
    zmk_combo_runtime_config_refresh();
    return 0;
}

const struct zmk_behavior_binding *
zmk_runtime_config_get_keymap_override(uint8_t layer_id, uint16_t key_position) {
    if (!active_config.present) {
        return NULL;
    }

    for (size_t i = 0; i < active_config.keymap_override_count; i++) {
        if (active_config.keymap_overrides[i].layer_id == layer_id &&
            active_config.keymap_overrides[i].key_position == key_position) {
            return &active_config.bindings[i];
        }
    }

    return NULL;
}

const struct zmk_runtime_object_slot *
zmk_runtime_config_get_active_object(zmk_runtime_object_id_t object_id) {
    if (!active_config.present) {
        return NULL;
    }

    return find_object(active_config.objects, active_config.object_count, object_id);
}

const struct zmk_runtime_combo_slot *zmk_runtime_config_get_active_combo(size_t index) {
    if (!active_config.present || index >= active_config.combo_count) {
        return NULL;
    }

    return &active_config.combos[index];
}

size_t zmk_runtime_config_get_active_combo_count(void) {
    return active_config.present ? active_config.combo_count : 0U;
}

int zmk_runtime_config_get_active_macro_steps(
    zmk_runtime_object_id_t object_id, const struct zmk_runtime_macro_step **steps,
    size_t *step_count) {
    const struct zmk_runtime_object_slot *object;
    size_t offset;
    size_t count;

    if (!steps || !step_count) {
        return -EINVAL;
    }

    object = zmk_runtime_config_get_active_object(object_id);
    if (!object || object->type != ZMK_RUNTIME_OBJECT_TYPE_MACRO) {
        return -ENOENT;
    }

    offset = object->data.macro.step_offset;
    count = object->data.macro.step_count;
    if (count == 0U || offset > active_config.macro_step_count ||
        count > active_config.macro_step_count - offset) {
        return -EINVAL;
    }

    *steps = &active_config.macro_steps[offset];
    *step_count = count;
    return 0;
}

int zmk_runtime_config_get_active_tap_dance_actions(
    zmk_runtime_object_id_t object_id, const struct zmk_runtime_tap_dance_action **actions,
    size_t *action_count, uint32_t *tapping_term_ms) {
    const struct zmk_runtime_object_slot *object;
    size_t offset;
    size_t count;

    if (!actions || !action_count || !tapping_term_ms) {
        return -EINVAL;
    }

    object = zmk_runtime_config_get_active_object(object_id);
    if (!object || object->type != ZMK_RUNTIME_OBJECT_TYPE_TAP_DANCE) {
        return -ENOENT;
    }

    offset = object->data.tap_dance.action_offset;
    count = object->data.tap_dance.action_count;
    if (count == 0U || offset > active_config.tap_dance_action_count ||
        count > active_config.tap_dance_action_count - offset ||
        object->data.tap_dance.tapping_term_ms == 0U) {
        return -EINVAL;
    }

    *actions = &active_config.tap_dance_actions[offset];
    *action_count = count;
    *tapping_term_ms = object->data.tap_dance.tapping_term_ms;
    return 0;
}

int zmk_runtime_config_action_ref_to_binding(const struct zmk_runtime_action_ref *action,
                                             struct zmk_behavior_binding *binding) {
    return action_to_binding(action, active_config.objects, active_config.object_count, true,
                             binding);
}

size_t zmk_runtime_config_max_update_chunk_bytes(void) {
    return CONFIG_ZMK_RUNTIME_CONFIG_RPC_MAX_CHUNK_BYTES;
}
