/*
 * Copyright (c) 2026 The ZMK Next Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <pb_encode.h>

#include <zmk/runtime_config.h>
#include <zmk/studio/rpc.h>

LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

ZMK_RPC_SUBSYSTEM(runtime_config)

#define RUNTIME_CONFIG_RESPONSE(type, ...) ZMK_RPC_RESPONSE(runtime_config, type, __VA_ARGS__)

static bool encode_capability_fingerprint(pb_ostream_t *stream, const pb_field_t *field,
                                          void *const *arg) {
    struct zmk_runtime_capabilities capabilities;

    ARG_UNUSED(arg);

    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    zmk_runtime_config_get_capabilities(&capabilities);
    return pb_encode_string(stream, capabilities.capability_fingerprint,
                            sizeof(capabilities.capability_fingerprint));
}

zmk_studio_Response get_runtime_capabilities(const zmk_studio_Request *req) {
    struct zmk_runtime_capabilities capabilities;
    zmk_runtime_config_RuntimeCapabilities response =
        zmk_runtime_config_RuntimeCapabilities_init_zero;

    ARG_UNUSED(req);

    zmk_runtime_config_get_capabilities(&capabilities);

    response.protocol_version = 1;
    response.persistence_schema_version = capabilities.persistence_schema_version;
    response.capability_fingerprint.funcs.encode = encode_capability_fingerprint;
    response.limits.max_runtime_objects = capabilities.max_objects;
    response.limits.max_combos = capabilities.max_combos;
    response.limits.max_combo_keys = capabilities.max_combo_keys;
    response.limits.max_macro_steps = capabilities.max_macro_steps;
    response.limits.max_persisted_bytes = capabilities.max_persisted_bytes;

    return RUNTIME_CONFIG_RESPONSE(get_runtime_capabilities, response);
}

ZMK_RPC_SUBSYSTEM_HANDLER(runtime_config, get_runtime_capabilities,
                          ZMK_STUDIO_RPC_HANDLER_UNSECURED);
