/*
 * Copyright (c) 2026 The ZMK Next Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zmk/behavior.h>

#define ZMK_RUNTIME_CONFIG_PERSISTENCE_SCHEMA_VERSION 1U
#define ZMK_RUNTIME_CAPABILITY_FINGERPRINT_SIZE 16U
#define ZMK_RUNTIME_OBJECT_ID_INVALID 0U

typedef uint32_t zmk_runtime_object_id_t;

enum zmk_runtime_object_type {
    ZMK_RUNTIME_OBJECT_TYPE_NONE = 0,
    ZMK_RUNTIME_OBJECT_TYPE_MOD_MORPH = 1,
    ZMK_RUNTIME_OBJECT_TYPE_MACRO = 2,
    ZMK_RUNTIME_OBJECT_TYPE_HOLD_TAP = 3,
    ZMK_RUNTIME_OBJECT_TYPE_TAP_DANCE = 4,
};

enum zmk_runtime_action_kind {
    ZMK_RUNTIME_ACTION_NONE = 0,
    ZMK_RUNTIME_ACTION_COMPILED_BEHAVIOR = 1,
    ZMK_RUNTIME_ACTION_OBJECT = 2,
};

struct zmk_runtime_action_ref {
    enum zmk_runtime_action_kind kind;
    union {
        struct {
            zmk_behavior_local_id_t local_id;
            uint32_t param1;
            uint32_t param2;
        } compiled;
        zmk_runtime_object_id_t object_id;
    } data;
};

struct zmk_runtime_object_slot {
    zmk_runtime_object_id_t id;
    enum zmk_runtime_object_type type;
};

struct zmk_runtime_combo_slot {
    zmk_runtime_object_id_t id;
    uint8_t key_count;
    uint16_t positions[CONFIG_ZMK_RUNTIME_MAX_COMBO_KEYS];
    struct zmk_runtime_action_ref output;
};

enum zmk_runtime_macro_step_type {
    ZMK_RUNTIME_MACRO_STEP_NONE = 0,
    ZMK_RUNTIME_MACRO_STEP_TAP = 1,
    ZMK_RUNTIME_MACRO_STEP_PRESS = 2,
    ZMK_RUNTIME_MACRO_STEP_RELEASE = 3,
    ZMK_RUNTIME_MACRO_STEP_WAIT = 4,
    ZMK_RUNTIME_MACRO_STEP_PAUSE_UNTIL_RELEASE = 5,
};

struct zmk_runtime_macro_step {
    enum zmk_runtime_macro_step_type type;
    union {
        struct zmk_runtime_action_ref action;
        uint32_t duration_ms;
    } data;
};

struct zmk_runtime_config_snapshot {
    uint16_t persistence_schema_version;
    uint32_t generation;
    uint8_t capability_fingerprint[ZMK_RUNTIME_CAPABILITY_FINGERPRINT_SIZE];
    uint16_t keymap_override_count;
    uint16_t object_count;
    uint16_t combo_count;
    uint16_t macro_step_count;
};

struct zmk_runtime_config_pool {
    struct zmk_runtime_object_slot objects[CONFIG_ZMK_RUNTIME_MAX_OBJECTS];
    struct zmk_runtime_combo_slot combos[CONFIG_ZMK_RUNTIME_MAX_COMBOS];
    struct zmk_runtime_macro_step macro_steps[CONFIG_ZMK_RUNTIME_MAX_MACRO_STEPS];
    uint8_t serialized_bytes[CONFIG_ZMK_RUNTIME_MAX_PERSISTED_BYTES];
};

struct zmk_runtime_capabilities {
    uint16_t persistence_schema_version;
    uint8_t capability_fingerprint[ZMK_RUNTIME_CAPABILITY_FINGERPRINT_SIZE];
    uint16_t max_objects;
    uint16_t max_combos;
    uint8_t max_combo_keys;
    uint16_t max_macro_steps;
    uint16_t max_persisted_bytes;
    uint32_t supported_object_types;
};

enum zmk_runtime_config_error {
    ZMK_RUNTIME_CONFIG_ERROR_NONE = 0,
    ZMK_RUNTIME_CONFIG_ERROR_INVALID_ARGUMENT = 1,
    ZMK_RUNTIME_CONFIG_ERROR_SCHEMA_VERSION = 2,
    ZMK_RUNTIME_CONFIG_ERROR_CAPABILITY_FINGERPRINT = 3,
    ZMK_RUNTIME_CONFIG_ERROR_RESOURCE_LIMIT = 4,
    ZMK_RUNTIME_CONFIG_ERROR_UNSUPPORTED_CONTENT = 5,
};

struct zmk_runtime_config_validation_result {
    bool valid;
    enum zmk_runtime_config_error error;
};

struct zmk_runtime_config_persistence_status {
    bool has_persisted_snapshot;
    uint32_t persisted_generation;
};

struct zmk_runtime_config_activation_status {
    uint32_t active_generation;
    uint32_t pending_generation;
    uint16_t pressed_key_count;
    uint16_t blocker_count;
};

void zmk_runtime_config_get_capabilities(struct zmk_runtime_capabilities *capabilities);
void zmk_runtime_config_init_empty_snapshot(struct zmk_runtime_config_snapshot *snapshot);
int zmk_runtime_config_validate_snapshot(const struct zmk_runtime_config_snapshot *snapshot,
                                         struct zmk_runtime_config_validation_result *result);
int zmk_runtime_config_stage_snapshot(const struct zmk_runtime_config_snapshot *snapshot,
                                      struct zmk_runtime_config_validation_result *result);
const struct zmk_runtime_config_snapshot *zmk_runtime_config_staged_snapshot(void);

int zmk_runtime_config_begin_update(uint32_t expected_active_generation, size_t snapshot_size,
                                    uint32_t *update_id);
int zmk_runtime_config_upload_update_chunk(uint32_t update_id, size_t offset, const uint8_t *chunk,
                                           size_t chunk_size, size_t *accepted_bytes,
                                           size_t *next_offset);
int zmk_runtime_config_get_uploaded_snapshot(uint32_t update_id, const uint8_t **snapshot_bytes,
                                             size_t *snapshot_size);
int zmk_runtime_config_stage_uploaded_snapshot(uint32_t update_id,
                                               const struct zmk_runtime_config_snapshot *snapshot,
                                               struct zmk_runtime_config_validation_result *result);
int zmk_runtime_config_get_validated_uploaded_snapshot(uint32_t update_id,
                                                       const uint8_t **snapshot_bytes,
                                                       size_t *snapshot_size);
int zmk_runtime_config_abort_update(uint32_t update_id);
size_t zmk_runtime_config_max_update_chunk_bytes(void);

int zmk_runtime_config_persist_update(uint32_t update_id, uint32_t *generation);
int zmk_runtime_config_get_persisted_snapshot(const uint8_t **snapshot_bytes, size_t *snapshot_size,
                                              uint32_t *generation);
void zmk_runtime_config_get_persistence_status(
    struct zmk_runtime_config_persistence_status *status);

/*
 * A persisted generation is activated only after the physical key state and all
 * registered runtime behavior engines are idle. Runtime engines retain a
 * blocker for the lifetime of any invocation that could still release an
 * action from the active generation.
 */
int zmk_runtime_config_request_activation(uint32_t generation);
int zmk_runtime_config_note_key_state(uint32_t position, bool pressed);
int zmk_runtime_config_activation_blocker_acquire(void);
int zmk_runtime_config_activation_blocker_release(void);
void zmk_runtime_config_get_activation_status(
    struct zmk_runtime_config_activation_status *status);
