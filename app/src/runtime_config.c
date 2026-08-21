/*
 * Copyright (c) 2026 The ZMK Next Contributors
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <zephyr/sys/util.h>

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

static void
fill_capability_fingerprint(uint8_t fingerprint[ZMK_RUNTIME_CAPABILITY_FINGERPRINT_SIZE]) {
    const uint16_t limits[] = {
        CONFIG_ZMK_RUNTIME_MAX_OBJECTS,         CONFIG_ZMK_RUNTIME_MAX_COMBOS,
        CONFIG_ZMK_RUNTIME_MAX_COMBO_KEYS,      CONFIG_ZMK_RUNTIME_MAX_MACRO_STEPS,
        CONFIG_ZMK_RUNTIME_MAX_PERSISTED_BYTES,
    };

    memset(fingerprint, 0, ZMK_RUNTIME_CAPABILITY_FINGERPRINT_SIZE);
    fingerprint[0] = 'Z';
    fingerprint[1] = 'N';
    fingerprint[2] = 'R';
    fingerprint[3] = 'C';
    fingerprint[4] = ZMK_RUNTIME_CONFIG_PERSISTENCE_SCHEMA_VERSION;

    for (size_t i = 0; i < ARRAY_SIZE(limits); i++) {
        fingerprint[5 + 2 * i] = limits[i] & 0xFF;
        fingerprint[6 + 2 * i] = limits[i] >> 8;
    }
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

    if (staged_config.update.active) {
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
    staged_config.update.validated = ret == 0;

    return ret;
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

int zmk_runtime_config_abort_update(uint32_t update_id) {
    if (!staged_config.update.active || staged_config.update.id != update_id) {
        return -ENOENT;
    }

    memset(&staged_config.update, 0, sizeof(staged_config.update));
    return 0;
}

size_t zmk_runtime_config_max_update_chunk_bytes(void) {
    return CONFIG_ZMK_RUNTIME_CONFIG_RPC_MAX_CHUNK_BYTES;
}
