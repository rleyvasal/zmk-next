/*
 * Copyright (c) 2026 The ZMK Next Contributors
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <zmk/runtime_config.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_RUNTIME_CONFIG_PERSISTENCE)

#define RUNTIME_CONFIG_SETTINGS_ROOT "zmk_next"
#define RUNTIME_CONFIG_SETTINGS_SLOT_COUNT 2U
#define RUNTIME_CONFIG_SETTINGS_MANIFEST_MAGIC 0x5A4E5243U
#define RUNTIME_CONFIG_SETTINGS_MANIFEST_VERSION 1U
#define RUNTIME_CONFIG_SETTINGS_CHUNK_COUNT                                                        \
    DIV_ROUND_UP(CONFIG_ZMK_RUNTIME_MAX_PERSISTED_BYTES,                                           \
                 CONFIG_ZMK_RUNTIME_CONFIG_SETTINGS_CHUNK_BYTES)

BUILD_ASSERT(CONFIG_ZMK_RUNTIME_MAX_PERSISTED_BYTES <= UINT16_MAX,
             "Runtime Config manifest payload length is a uint16_t");

struct runtime_config_settings_manifest {
    uint32_t magic;
    uint16_t manifest_version;
    uint16_t persistence_schema_version;
    uint32_t generation;
    uint8_t capability_fingerprint[ZMK_RUNTIME_CAPABILITY_FINGERPRINT_SIZE];
    uint16_t payload_length;
    uint16_t chunk_count;
    uint32_t crc32;
} __packed;

struct runtime_config_settings_slot {
    bool manifest_present;
    struct runtime_config_settings_manifest manifest;
    uint8_t payload[CONFIG_ZMK_RUNTIME_MAX_PERSISTED_BYTES];
    uint16_t chunk_lengths[RUNTIME_CONFIG_SETTINGS_CHUNK_COUNT];
    bool chunk_present[RUNTIME_CONFIG_SETTINGS_CHUNK_COUNT];
};

static struct {
    struct runtime_config_settings_slot slots[RUNTIME_CONFIG_SETTINGS_SLOT_COUNT];
    int selected_slot;
    struct zmk_runtime_config_persistence_status status;
} runtime_config_settings = {.selected_slot = -1};

static const char *slot_name(size_t slot) { return slot == 0U ? "a" : "b"; }

static int make_manifest_key(char *key, size_t key_size, size_t slot) {
    int written =
        snprintf(key, key_size, RUNTIME_CONFIG_SETTINGS_ROOT "/%s/manifest", slot_name(slot));

    return written < 0 || (size_t)written >= key_size ? -ENAMETOOLONG : 0;
}

static int make_chunk_key(char *key, size_t key_size, size_t slot, size_t chunk) {
    int written = snprintf(key, key_size, RUNTIME_CONFIG_SETTINGS_ROOT "/%s/chunk/%u",
                           slot_name(slot), (unsigned int)chunk);

    return written < 0 || (size_t)written >= key_size ? -ENAMETOOLONG : 0;
}

static bool
manifest_has_compatible_header(const struct runtime_config_settings_manifest *manifest) {
    struct zmk_runtime_capabilities capabilities;

    zmk_runtime_config_get_capabilities(&capabilities);
    return manifest->magic == RUNTIME_CONFIG_SETTINGS_MANIFEST_MAGIC &&
           manifest->manifest_version == RUNTIME_CONFIG_SETTINGS_MANIFEST_VERSION &&
           manifest->persistence_schema_version == capabilities.persistence_schema_version &&
           memcmp(manifest->capability_fingerprint, capabilities.capability_fingerprint,
                  sizeof(manifest->capability_fingerprint)) == 0;
}

static bool slot_is_valid(const struct runtime_config_settings_slot *slot) {
    if (!slot->manifest_present || !manifest_has_compatible_header(&slot->manifest) ||
        slot->manifest.payload_length == 0U ||
        slot->manifest.payload_length > CONFIG_ZMK_RUNTIME_MAX_PERSISTED_BYTES ||
        slot->manifest.chunk_count == 0U ||
        slot->manifest.chunk_count > RUNTIME_CONFIG_SETTINGS_CHUNK_COUNT ||
        slot->manifest.chunk_count !=
            DIV_ROUND_UP(slot->manifest.payload_length,
                         CONFIG_ZMK_RUNTIME_CONFIG_SETTINGS_CHUNK_BYTES)) {
        return false;
    }

    for (size_t i = 0; i < slot->manifest.chunk_count; i++) {
        size_t offset = i * CONFIG_ZMK_RUNTIME_CONFIG_SETTINGS_CHUNK_BYTES;
        size_t expected_length = MIN(CONFIG_ZMK_RUNTIME_CONFIG_SETTINGS_CHUNK_BYTES,
                                     slot->manifest.payload_length - offset);

        if (!slot->chunk_present[i] || slot->chunk_lengths[i] != expected_length) {
            return false;
        }
    }

    return crc32_ieee(slot->payload, slot->manifest.payload_length) == slot->manifest.crc32;
}

static bool generation_is_newer(uint32_t candidate, uint32_t current) {
    return (int32_t)(candidate - current) > 0;
}

static void select_newest_valid_slot(void) {
    int newest_slot = -1;

    for (size_t slot = 0; slot < RUNTIME_CONFIG_SETTINGS_SLOT_COUNT; slot++) {
        if (!slot_is_valid(&runtime_config_settings.slots[slot])) {
            continue;
        }

        if (newest_slot < 0 ||
            generation_is_newer(runtime_config_settings.slots[slot].manifest.generation,
                                runtime_config_settings.slots[newest_slot].manifest.generation)) {
            newest_slot = slot;
        }
    }

    runtime_config_settings.selected_slot = newest_slot;
    runtime_config_settings.status.has_persisted_snapshot = newest_slot >= 0;
    runtime_config_settings.status.persisted_generation =
        newest_slot >= 0 ? runtime_config_settings.slots[newest_slot].manifest.generation : 0U;

    if (newest_slot >= 0) {
        LOG_INF("Selected Runtime Config generation %u from slot %s",
                runtime_config_settings.status.persisted_generation, slot_name(newest_slot));
        (void)zmk_runtime_config_request_activation(
            runtime_config_settings.status.persisted_generation);
    } else {
        LOG_INF("No valid persisted Runtime Config snapshot found");
    }
}

static int read_exact(settings_read_cb read_cb, void *cb_arg, void *destination, size_t len) {
    int ret = read_cb(cb_arg, destination, len);

    return ret == (int)len ? 0 : ret < 0 ? ret : -EINVAL;
}

static int load_manifest(size_t slot, size_t len, settings_read_cb read_cb, void *cb_arg) {
    struct runtime_config_settings_slot *target = &runtime_config_settings.slots[slot];
    int ret;

    if (len != sizeof(target->manifest)) {
        return -EINVAL;
    }

    ret = read_exact(read_cb, cb_arg, &target->manifest, sizeof(target->manifest));
    if (ret != 0) {
        return ret;
    }

    target->manifest_present = true;
    return 0;
}

static int load_chunk(size_t slot, size_t chunk, size_t len, settings_read_cb read_cb,
                      void *cb_arg) {
    struct runtime_config_settings_slot *target = &runtime_config_settings.slots[slot];
    size_t offset = chunk * CONFIG_ZMK_RUNTIME_CONFIG_SETTINGS_CHUNK_BYTES;
    int ret;

    if (chunk >= RUNTIME_CONFIG_SETTINGS_CHUNK_COUNT || len == 0U ||
        len > CONFIG_ZMK_RUNTIME_CONFIG_SETTINGS_CHUNK_BYTES ||
        len > CONFIG_ZMK_RUNTIME_MAX_PERSISTED_BYTES - offset) {
        return -EINVAL;
    }

    ret = read_exact(read_cb, cb_arg, &target->payload[offset], len);
    if (ret != 0) {
        return ret;
    }

    target->chunk_lengths[chunk] = len;
    target->chunk_present[chunk] = true;
    return 0;
}

static int runtime_config_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                       void *cb_arg) {
    const char *next;
    const char *chunk_name;
    int slot;

    if (settings_name_steq(name, "a", &next)) {
        slot = 0;
    } else if (settings_name_steq(name, "b", &next)) {
        slot = 1;
    } else {
        return -ENOENT;
    }

    if (!next) {
        return -ENOENT;
    }

    if (settings_name_steq(next, "manifest", &chunk_name) && !chunk_name) {
        return load_manifest(slot, len, read_cb, cb_arg);
    }

    if (!settings_name_steq(next, "chunk", &chunk_name) || !chunk_name) {
        return -ENOENT;
    }

    char *end;
    unsigned long chunk = strtoul(chunk_name, &end, 10);
    if (*end != '\0' || chunk >= RUNTIME_CONFIG_SETTINGS_CHUNK_COUNT) {
        return -EINVAL;
    }

    return load_chunk(slot, chunk, len, read_cb, cb_arg);
}

static int runtime_config_settings_commit(void) {
    select_newest_valid_slot();
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(runtime_config, RUNTIME_CONFIG_SETTINGS_ROOT, NULL,
                               runtime_config_settings_set, runtime_config_settings_commit, NULL);

static void cache_committed_slot(size_t slot, const uint8_t *payload, size_t payload_size,
                                 const struct runtime_config_settings_manifest *manifest) {
    struct runtime_config_settings_slot *target = &runtime_config_settings.slots[slot];

    memset(target, 0, sizeof(*target));
    target->manifest_present = true;
    target->manifest = *manifest;
    memcpy(target->payload, payload, payload_size);

    for (size_t i = 0; i < manifest->chunk_count; i++) {
        size_t offset = i * CONFIG_ZMK_RUNTIME_CONFIG_SETTINGS_CHUNK_BYTES;

        target->chunk_lengths[i] =
            MIN(CONFIG_ZMK_RUNTIME_CONFIG_SETTINGS_CHUNK_BYTES, payload_size - offset);
        target->chunk_present[i] = true;
    }
}

int zmk_runtime_config_persist_update(uint32_t update_id, uint32_t *generation) {
    const uint8_t *payload;
    size_t payload_size;
    struct runtime_config_settings_manifest manifest;
    struct zmk_runtime_capabilities capabilities;
    size_t slot;
    char key[32];
    int ret;

    if (!generation) {
        return -EINVAL;
    }

    ret = zmk_runtime_config_get_validated_uploaded_snapshot(update_id, &payload, &payload_size);
    if (ret != 0) {
        return ret;
    }

    slot = runtime_config_settings.selected_slot == 0 ? 1U : 0U;
    ret = make_manifest_key(key, sizeof(key), slot);
    if (ret != 0) {
        return ret;
    }

    /* Invalidate the inactive slot before touching its chunks. */
    ret = settings_delete(key);
    if (ret != 0 && ret != -ENOENT) {
        LOG_ERR("Failed to invalidate Runtime Config slot %s (%d)", slot_name(slot), ret);
        return ret;
    }

    for (size_t chunk = 0;
         chunk < DIV_ROUND_UP(payload_size, CONFIG_ZMK_RUNTIME_CONFIG_SETTINGS_CHUNK_BYTES);
         chunk++) {
        size_t offset = chunk * CONFIG_ZMK_RUNTIME_CONFIG_SETTINGS_CHUNK_BYTES;
        size_t length = MIN(CONFIG_ZMK_RUNTIME_CONFIG_SETTINGS_CHUNK_BYTES, payload_size - offset);

        ret = make_chunk_key(key, sizeof(key), slot, chunk);
        if (ret != 0) {
            return ret;
        }

        ret = settings_save_one(key, &payload[offset], length);
        if (ret != 0) {
            LOG_ERR("Failed to persist Runtime Config slot %s chunk %u (%d)", slot_name(slot),
                    (unsigned int)chunk, ret);
            return ret;
        }
    }

    memset(&manifest, 0, sizeof(manifest));
    manifest.magic = RUNTIME_CONFIG_SETTINGS_MANIFEST_MAGIC;
    manifest.manifest_version = RUNTIME_CONFIG_SETTINGS_MANIFEST_VERSION;
    manifest.persistence_schema_version = ZMK_RUNTIME_CONFIG_PERSISTENCE_SCHEMA_VERSION;
    manifest.generation = runtime_config_settings.status.persisted_generation + 1U;
    if (manifest.generation == 0U) {
        manifest.generation = 1U;
    }
    zmk_runtime_config_get_capabilities(&capabilities);
    memcpy(manifest.capability_fingerprint, capabilities.capability_fingerprint,
           sizeof(manifest.capability_fingerprint));
    manifest.payload_length = payload_size;
    manifest.chunk_count =
        DIV_ROUND_UP(payload_size, CONFIG_ZMK_RUNTIME_CONFIG_SETTINGS_CHUNK_BYTES);
    manifest.crc32 = crc32_ieee(payload, payload_size);

    ret = make_manifest_key(key, sizeof(key), slot);
    if (ret != 0) {
        return ret;
    }

    ret = settings_save_one(key, &manifest, sizeof(manifest));
    if (ret != 0) {
        LOG_ERR("Failed to finalize Runtime Config slot %s (%d)", slot_name(slot), ret);
        return ret;
    }

    cache_committed_slot(slot, payload, payload_size, &manifest);
    runtime_config_settings.selected_slot = slot;
    runtime_config_settings.status.has_persisted_snapshot = true;
    runtime_config_settings.status.persisted_generation = manifest.generation;
    *generation = manifest.generation;

    ret = zmk_runtime_config_request_activation(manifest.generation);
    if (ret != 0) {
        return ret;
    }

    return zmk_runtime_config_abort_update(update_id);
}

int zmk_runtime_config_get_persisted_snapshot(const uint8_t **snapshot_bytes, size_t *snapshot_size,
                                              uint32_t *generation) {
    const struct runtime_config_settings_slot *slot;

    if (!snapshot_bytes || !snapshot_size || !generation) {
        return -EINVAL;
    }

    if (runtime_config_settings.selected_slot < 0) {
        return -ENOENT;
    }

    slot = &runtime_config_settings.slots[runtime_config_settings.selected_slot];
    *snapshot_bytes = slot->payload;
    *snapshot_size = slot->manifest.payload_length;
    *generation = slot->manifest.generation;
    return 0;
}

void zmk_runtime_config_get_persistence_status(
    struct zmk_runtime_config_persistence_status *status) {
    if (status) {
        *status = runtime_config_settings.status;
    }
}

#else

int zmk_runtime_config_persist_update(uint32_t update_id, uint32_t *generation) {
    ARG_UNUSED(update_id);
    ARG_UNUSED(generation);
    return -ENOTSUP;
}

int zmk_runtime_config_get_persisted_snapshot(const uint8_t **snapshot_bytes, size_t *snapshot_size,
                                              uint32_t *generation) {
    ARG_UNUSED(snapshot_bytes);
    ARG_UNUSED(snapshot_size);
    ARG_UNUSED(generation);
    return -ENOTSUP;
}

void zmk_runtime_config_get_persistence_status(
    struct zmk_runtime_config_persistence_status *status) {
    if (status) {
        memset(status, 0, sizeof(*status));
    }
}

#endif
