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
    struct zmk_runtime_keymap_override
        keymap_overrides[CONFIG_ZMK_RUNTIME_MAX_KEYMAP_OVERRIDES];
    struct zmk_behavior_binding bindings[CONFIG_ZMK_RUNTIME_MAX_KEYMAP_OVERRIDES];
} pending_config, active_config;

#define RUNTIME_CONFIG_PERSISTED_PAYLOAD_MAGIC 0x5A4E4B4FU
#define RUNTIME_CONFIG_PERSISTED_PAYLOAD_VERSION 1U

struct runtime_config_persisted_payload_header {
    uint32_t magic;
    uint16_t version;
    uint16_t keymap_override_count;
} __packed;

struct runtime_config_persisted_keymap_override {
    uint8_t layer_id;
    uint16_t key_position;
    zmk_behavior_local_id_t behavior_id;
    uint32_t param1;
    uint32_t param2;
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
    memset(staged_config.pool.keymap_overrides, 0, sizeof(staged_config.pool.keymap_overrides));
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
    if (ret == 0 && snapshot->keymap_override_count != staged_config.pool.keymap_override_count) {
        if (result) {
            result->valid = false;
            result->error = ZMK_RUNTIME_CONFIG_ERROR_INVALID_ARGUMENT;
        }
        ret = -EINVAL;
    }
    if (ret == 0) {
        for (size_t i = 0; i < staged_config.pool.keymap_override_count; i++) {
            const struct zmk_runtime_keymap_override *override =
                &staged_config.pool.keymap_overrides[i];
            const char *behavior_name =
                zmk_behavior_find_behavior_name_from_local_id(override->behavior_id);
            struct zmk_behavior_binding binding = {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
                .local_id = override->behavior_id,
#endif
                .behavior_dev = behavior_name,
                .param1 = override->param1,
                .param2 = override->param2,
            };

            if (override->layer_id >= ZMK_KEYMAP_LAYERS_LEN ||
                override->key_position >= ZMK_KEYMAP_LEN || !behavior_name ||
                zmk_behavior_validate_binding(&binding) != 0) {
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

int zmk_runtime_config_get_validated_uploaded_snapshot(uint32_t update_id,
                                                       const uint8_t **snapshot_bytes,
                                                       size_t *snapshot_size) {
    int ret = zmk_runtime_config_get_uploaded_snapshot(update_id, snapshot_bytes, snapshot_size);

    if (ret != 0) {
        return ret;
    }

    return staged_config.update.validated ? 0 : -EINVAL;
}

int zmk_runtime_config_get_persistable_update(uint32_t update_id, const uint8_t **snapshot_bytes,
                                              size_t *snapshot_size) {
    struct runtime_config_persisted_payload_header header = {
        .magic = RUNTIME_CONFIG_PERSISTED_PAYLOAD_MAGIC,
        .version = RUNTIME_CONFIG_PERSISTED_PAYLOAD_VERSION,
        .keymap_override_count = staged_config.pool.keymap_override_count,
    };
    size_t payload_size;
    int ret;

    ret = zmk_runtime_config_get_validated_uploaded_snapshot(update_id, snapshot_bytes, snapshot_size);
    if (ret != 0) {
        return ret;
    }

    payload_size = sizeof(header) +
                   header.keymap_override_count *
                       sizeof(struct runtime_config_persisted_keymap_override);
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
            .behavior_id = source->behavior_id,
            .param1 = source->param1,
            .param2 = source->param2,
        };

        memcpy(staged_config.pool.serialized_bytes +
                   sizeof(header) + i * sizeof(destination),
               &destination, sizeof(destination));
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

static int prepare_keymap_overlay(
    const struct zmk_runtime_keymap_override *overrides, size_t count, uint32_t generation) {
    if (count > CONFIG_ZMK_RUNTIME_MAX_KEYMAP_OVERRIDES) {
        return -ENOSPC;
    }

    memset(&pending_config, 0, sizeof(pending_config));
    pending_config.generation = generation;
    pending_config.keymap_override_count = count;

    for (size_t i = 0; i < count; i++) {
        const char *behavior_name =
            zmk_behavior_find_behavior_name_from_local_id(overrides[i].behavior_id);

        if (overrides[i].layer_id >= ZMK_KEYMAP_LAYERS_LEN ||
            overrides[i].key_position >= ZMK_KEYMAP_LEN) {
            return -EINVAL;
        }

        if (!behavior_name) {
            return -ENODEV;
        }

        pending_config.keymap_overrides[i] = overrides[i];
        pending_config.bindings[i] = (struct zmk_behavior_binding){
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
            .local_id = overrides[i].behavior_id,
#endif
            .behavior_dev = behavior_name,
            .param1 = overrides[i].param1,
            .param2 = overrides[i].param2,
        };
        if (zmk_behavior_validate_binding(&pending_config.bindings[i]) != 0) {
            return -EINVAL;
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

    return prepare_keymap_overlay(staged_config.pool.keymap_overrides,
                                  staged_config.pool.keymap_override_count, generation);
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
                                       sizeof(struct runtime_config_persisted_keymap_override);
    if (header.magic != RUNTIME_CONFIG_PERSISTED_PAYLOAD_MAGIC ||
        header.version != RUNTIME_CONFIG_PERSISTED_PAYLOAD_VERSION ||
        header.keymap_override_count > CONFIG_ZMK_RUNTIME_MAX_KEYMAP_OVERRIDES ||
        expected_size != snapshot_size) {
        return -EBADMSG;
    }

    for (size_t i = 0; i < header.keymap_override_count; i++) {
        struct runtime_config_persisted_keymap_override source;

        memcpy(&source, snapshot_bytes + sizeof(header) + i * sizeof(source), sizeof(source));
        staged_config.pool.keymap_overrides[i] = (struct zmk_runtime_keymap_override){
            .layer_id = source.layer_id,
            .key_position = source.key_position,
            .behavior_id = source.behavior_id,
            .param1 = source.param1,
            .param2 = source.param2,
        };
    }

    staged_config.pool.keymap_override_count = header.keymap_override_count;
    return prepare_keymap_overlay(staged_config.pool.keymap_overrides,
                                  staged_config.pool.keymap_override_count, generation);
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

size_t zmk_runtime_config_max_update_chunk_bytes(void) {
    return CONFIG_ZMK_RUNTIME_CONFIG_RPC_MAX_CHUNK_BYTES;
}
