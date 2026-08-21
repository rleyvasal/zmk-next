/*
 * Copyright (c) 2026 The ZMK Next Contributors
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <zmk/runtime_config.h>

static int invalidate(struct zmk_runtime_config_validation_result *result,
                      enum zmk_runtime_config_error error, int ret) {
    if (result) {
        result->valid = false;
        result->error = error;
    }

    return ret;
}

int zmk_runtime_config_validate_snapshot(const struct zmk_runtime_config_snapshot *snapshot,
                                         struct zmk_runtime_config_validation_result *result) {
    struct zmk_runtime_capabilities capabilities;

    if (!snapshot || !result) {
        return invalidate(result, ZMK_RUNTIME_CONFIG_ERROR_INVALID_ARGUMENT, -EINVAL);
    }

    result->valid = false;
    result->error = ZMK_RUNTIME_CONFIG_ERROR_NONE;
    zmk_runtime_config_get_capabilities(&capabilities);

    if (snapshot->persistence_schema_version != capabilities.persistence_schema_version) {
        return invalidate(result, ZMK_RUNTIME_CONFIG_ERROR_SCHEMA_VERSION, -EPROTONOSUPPORT);
    }

    if (memcmp(snapshot->capability_fingerprint, capabilities.capability_fingerprint,
               sizeof(snapshot->capability_fingerprint)) != 0) {
        return invalidate(result, ZMK_RUNTIME_CONFIG_ERROR_CAPABILITY_FINGERPRINT, -EXDEV);
    }

    if (snapshot->keymap_override_count > capabilities.max_keymap_overrides ||
        snapshot->object_count > capabilities.max_objects ||
        snapshot->combo_count > capabilities.max_combos ||
        snapshot->macro_step_count > capabilities.max_macro_steps) {
        return invalidate(result, ZMK_RUNTIME_CONFIG_ERROR_RESOURCE_LIMIT, -ENOSPC);
    }

    /* Mod-morph and macros are runtime object engines. Combos follow later. */
    if (snapshot->combo_count != 0) {
        return invalidate(result, ZMK_RUNTIME_CONFIG_ERROR_UNSUPPORTED_CONTENT, -ENOTSUP);
    }

    result->valid = true;
    return 0;
}
