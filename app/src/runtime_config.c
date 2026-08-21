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
    struct zmk_runtime_keymap_override
        keymap_overrides[CONFIG_ZMK_RUNTIME_MAX_KEYMAP_OVERRIDES];
    struct zmk_runtime_object_slot objects[CONFIG_ZMK_RUNTIME_MAX_OBJECTS];
    struct zmk_behavior_binding bindings[CONFIG_ZMK_RUNTIME_MAX_KEYMAP_OVERRIDES];
} pending_config, active_config;

#define RUNTIME_CONFIG_PERSISTED_PAYLOAD_MAGIC 0x5A4E4B4FU
#define RUNTIME_CONFIG_PERSISTED_PAYLOAD_VERSION 2U

struct runtime_config_persisted_payload_header {
    uint32_t magic;
    uint16_t version;
    uint16_t keymap_override_count;
    uint16_t object_count;
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
    zmk_runtime_object_id_t id;
    uint32_t modifiers;
    struct runtime_config_persisted_action_ref normal_action;
    struct runtime_config_persisted_action_ref morphed_action;
} __packed;

static void
fill_capability_fingerprint(uint8_t fingerprint[ZMK_RUNTIME_CAPABILITY_FINGERPRINT_SIZE]) {
    const uint16_t limits[] = {
        CONFIG_ZMK_RUNTIME_MAX_OBJECTS,         CONFIG_ZMK_RUNTIME_MAX_COMBOS,
        CONFIG_ZMK_RUNTIME_MAX_COMBO_KEYS,      CONFIG_ZMK_RUNTIME_MAX_MACRO_STEPS,
        CONFIG_ZMK_RUNTIME_MAX_PERSISTED_BYTES, CONFIG_ZMK_RUNTIME_MAX_KEYMAP_OVERRIDES,
    };

    memset(fingerprint, 0, ZMK_RUNTIME_CAPABILITY_FINGERPRINT_SIZE);
    fingerprint[0] = 'Z';
    fingerprint[1] = 'N';
    fingerprint[2] = 'R';
    fingerprint[3] = 'C';
    fingerprint[4] = ZMK_RUNTIME_CONFIG_PERSISTENCE_SCHEMA_VERSION;

    fingerprint[5] = limits[0] & 0xFF;
    fingerprint[6] = limits[0] >> 8;
    fingerprint[7] = limits[1] & 0xFF;
    fingerprint[8] = limits[1] >> 8;
    fingerprint[9] = limits[2];
    fingerprint[10] = limits[3] & 0xFF;
    fingerprint[11] = limits[3] >> 8;
    fingerprint[12] = limits[4] & 0xFF;
    fingerprint[13] = limits[4] >> 8;
    fingerprint[14] = limits[5] & 0xFF;
    fingerprint[15] = limits[5] >> 8;
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

static int validate_object(const struct zmk_runtime_object_slot *object) {
    struct zmk_behavior_binding binding;

    if (!object || object->id == ZMK_RUNTIME_OBJECT_ID_INVALID ||
        object->type != ZMK_RUNTIME_OBJECT_TYPE_MOD_MORPH ||
        object->data.mod_morph.modifiers == 0U) {
        return -EINVAL;
    }

    if (action_to_binding(&object->data.mod_morph.normal_action, NULL, 0U, false, &binding) != 0 ||
        action_to_binding(&object->data.mod_morph.morphed_action, NULL, 0U, false, &binding) !=
            0) {
        return -EINVAL;
    }

    return 0;
}

static int validate_objects(const struct zmk_runtime_object_slot *objects, size_t count) {
    if ((!objects && count != 0U) || count > CONFIG_ZMK_RUNTIME_MAX_OBJECTS) {
        return -EINVAL;
    }

    for (size_t i = 0; i < count; i++) {
        if (validate_object(&objects[i]) != 0) {
            return -EINVAL;
        }

        for (size_t previous = 0; previous < i; previous++) {
            if (objects[previous].id == objects[i].id) {
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
    memset(staged_config.pool.keymap_overrides, 0, sizeof(staged_config.pool.keymap_overrides));
    memset(staged_config.pool.objects, 0, sizeof(staged_config.pool.objects));
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
                     snapshot->object_count != staged_config.pool.object_count)) {
        if (result) {
            result->valid = false;
            result->error = ZMK_RUNTIME_CONFIG_ERROR_INVALID_ARGUMENT;
        }
        ret = -EINVAL;
    }

    if (ret == 0) {
        ret = validate_objects(staged_config.pool.objects, staged_config.pool.object_count);
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
    };
    if (!snapshot_size || !staged_config.update.active || staged_config.update.id != update_id ||
        !staged_config.update.validated) {
        return -EINVAL;
    }

    *snapshot_size = sizeof(header) +
                     header.keymap_override_count *
                         sizeof(struct runtime_config_persisted_keymap_override) +
                     header.object_count * sizeof(struct runtime_config_persisted_mod_morph);
    return *snapshot_size > sizeof(staged_config.pool.serialized_bytes) ? -ENOSPC : 0;
}

int zmk_runtime_config_get_persistable_update(uint32_t update_id, const uint8_t **snapshot_bytes,
                                              size_t *snapshot_size) {
    struct runtime_config_persisted_payload_header header = {
        .magic = RUNTIME_CONFIG_PERSISTED_PAYLOAD_MAGIC,
        .version = RUNTIME_CONFIG_PERSISTED_PAYLOAD_VERSION,
        .keymap_override_count = staged_config.pool.keymap_override_count,
        .object_count = staged_config.pool.object_count,
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
        struct runtime_config_persisted_mod_morph destination = {
            .id = source->id,
            .modifiers = source->data.mod_morph.modifiers,
            .normal_action = persist_action(&source->data.mod_morph.normal_action),
            .morphed_action = persist_action(&source->data.mod_morph.morphed_action),
        };
        size_t offset = sizeof(header) +
                        header.keymap_override_count *
                            sizeof(struct runtime_config_persisted_keymap_override) +
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
                                  uint32_t generation) {
    if (count > CONFIG_ZMK_RUNTIME_MAX_KEYMAP_OVERRIDES ||
        object_count > CONFIG_ZMK_RUNTIME_MAX_OBJECTS) {
        return -ENOSPC;
    }

    if (validate_objects(objects, object_count) != 0) {
        return -EINVAL;
    }

    memset(&pending_config, 0, sizeof(pending_config));
    pending_config.generation = generation;
    pending_config.keymap_override_count = count;
    pending_config.object_count = object_count;
    if (object_count != 0U) {
        memcpy(pending_config.objects, objects, object_count * sizeof(pending_config.objects[0]));
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
                    header.object_count * sizeof(struct runtime_config_persisted_mod_morph);
    if (header.magic != RUNTIME_CONFIG_PERSISTED_PAYLOAD_MAGIC ||
        header.version != RUNTIME_CONFIG_PERSISTED_PAYLOAD_VERSION ||
        header.keymap_override_count > CONFIG_ZMK_RUNTIME_MAX_KEYMAP_OVERRIDES ||
        header.object_count > CONFIG_ZMK_RUNTIME_MAX_OBJECTS ||
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
        struct runtime_config_persisted_mod_morph source;
        size_t offset = sizeof(header) +
                        header.keymap_override_count *
                            sizeof(struct runtime_config_persisted_keymap_override) +
                        i * sizeof(source);

        memcpy(&source, snapshot_bytes + offset, sizeof(source));
        staged_config.pool.objects[i] = (struct zmk_runtime_object_slot){
            .id = source.id,
            .type = ZMK_RUNTIME_OBJECT_TYPE_MOD_MORPH,
            .data.mod_morph = {
                .modifiers = source.modifiers,
                .normal_action = restore_action(&source.normal_action),
                .morphed_action = restore_action(&source.morphed_action),
            },
        };
    }

    staged_config.pool.keymap_override_count = header.keymap_override_count;
    staged_config.pool.object_count = header.object_count;
    return prepare_runtime_config(staged_config.pool.keymap_overrides,
                                  staged_config.pool.keymap_override_count,
                                  staged_config.pool.objects, staged_config.pool.object_count,
                                  generation);
}

int zmk_runtime_config_activate_pending_generation(uint32_t generation) {
    if (!pending_config.present || pending_config.generation != generation) {
        return -ENOENT;
    }

    active_config = pending_config;
    memset(&pending_config, 0, sizeof(pending_config));
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

int zmk_runtime_config_action_ref_to_binding(const struct zmk_runtime_action_ref *action,
                                             struct zmk_behavior_binding *binding) {
    return action_to_binding(action, active_config.objects, active_config.object_count, true,
                             binding);
}

size_t zmk_runtime_config_max_update_chunk_bytes(void) {
    return CONFIG_ZMK_RUNTIME_CONFIG_RPC_MAX_CHUNK_BYTES;
}
