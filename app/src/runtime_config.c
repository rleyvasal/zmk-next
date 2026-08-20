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
