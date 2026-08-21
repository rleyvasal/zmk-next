/*
 * Copyright (c) 2026 The ZMK Next Contributors
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/logging/log.h>

#include <pb_decode.h>

#include <zmk/runtime_config.h>
#include <zmk/studio/rpc.h>

LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

ZMK_RPC_SUBSYSTEM(runtime_config)

#define RUNTIME_CONFIG_RESPONSE(type, ...) ZMK_RPC_RESPONSE(runtime_config, type, __VA_ARGS__)

static void set_error(zmk_studio_Response *response,
                      zmk_runtime_config_RuntimeConfigErrorCode code) {
    response->type.request_response.subsystem.runtime_config.has_error = true;
    response->type.request_response.subsystem.runtime_config.error.code = code;
}

static zmk_runtime_config_RuntimeConfigErrorCode error_from_errno(int error) {
    switch (error) {
    case -EBUSY:
        return zmk_runtime_config_RuntimeConfigErrorCode_RUNTIME_CONFIG_ERROR_UPDATE_IN_PROGRESS;
    case -ENOENT:
        return zmk_runtime_config_RuntimeConfigErrorCode_RUNTIME_CONFIG_ERROR_UPDATE_NOT_FOUND;
    case -EAGAIN:
        return zmk_runtime_config_RuntimeConfigErrorCode_RUNTIME_CONFIG_ERROR_UPDATE_INCOMPLETE;
    case -EMSGSIZE:
    case -EFBIG:
        return zmk_runtime_config_RuntimeConfigErrorCode_RUNTIME_CONFIG_ERROR_INVALID_CHUNK;
    case -ENOSPC:
        return zmk_runtime_config_RuntimeConfigErrorCode_RUNTIME_CONFIG_ERROR_RESOURCE_LIMIT;
    case -ESTALE:
        return zmk_runtime_config_RuntimeConfigErrorCode_RUNTIME_CONFIG_ERROR_STALE_GENERATION;
    case -EPROTONOSUPPORT:
        return zmk_runtime_config_RuntimeConfigErrorCode_RUNTIME_CONFIG_ERROR_SNAPSHOT_SCHEMA_VERSION;
    case -EXDEV:
        return zmk_runtime_config_RuntimeConfigErrorCode_RUNTIME_CONFIG_ERROR_CAPABILITY_FINGERPRINT;
    case -ENOTSUP:
        return zmk_runtime_config_RuntimeConfigErrorCode_RUNTIME_CONFIG_ERROR_NOT_SUPPORTED;
    case -EBADMSG:
        return zmk_runtime_config_RuntimeConfigErrorCode_RUNTIME_CONFIG_ERROR_VALIDATION;
    case -EIO:
    case -ENOSYS:
        return zmk_runtime_config_RuntimeConfigErrorCode_RUNTIME_CONFIG_ERROR_PERSISTENCE;
    default:
        return zmk_runtime_config_RuntimeConfigErrorCode_RUNTIME_CONFIG_ERROR_INVALID_REQUEST;
    }
}

zmk_studio_Response get_runtime_capabilities(const zmk_studio_Request *req) {
    struct zmk_runtime_capabilities capabilities;
    zmk_runtime_config_RuntimeCapabilities response =
        zmk_runtime_config_RuntimeCapabilities_init_zero;

    ARG_UNUSED(req);

    zmk_runtime_config_get_capabilities(&capabilities);

    response.protocol_version = 1;
    response.persistence_schema_version = capabilities.persistence_schema_version;
    response.capability_fingerprint.size = sizeof(capabilities.capability_fingerprint);
    memcpy(response.capability_fingerprint.bytes, capabilities.capability_fingerprint,
           sizeof(capabilities.capability_fingerprint));
    response.has_limits = true;
    response.limits.max_runtime_objects = capabilities.max_objects;
    response.limits.max_combos = capabilities.max_combos;
    response.limits.max_combo_keys = capabilities.max_combo_keys;
    response.limits.max_macro_steps = capabilities.max_macro_steps;
    response.limits.max_persisted_bytes = capabilities.max_persisted_bytes;

    return RUNTIME_CONFIG_RESPONSE(get_runtime_capabilities, response);
}

static void populate_status(zmk_runtime_config_RuntimeConfigStatus *status) {
    struct zmk_runtime_config_activation_status activation;

    zmk_runtime_config_get_activation_status(&activation);
    status->state =
        activation.pending_generation != 0U
            ? zmk_runtime_config_RuntimeConfigState_RUNTIME_CONFIG_STATE_PERSISTED_PENDING_IDLE
            : zmk_runtime_config_RuntimeConfigState_RUNTIME_CONFIG_STATE_ACTIVE;
    status->active_generation = activation.active_generation;
    status->pending_generation = activation.pending_generation;
}

zmk_studio_Response get_runtime_config_status(const zmk_studio_Request *req) {
    zmk_runtime_config_RuntimeConfigStatus response =
        zmk_runtime_config_RuntimeConfigStatus_init_zero;

    ARG_UNUSED(req);

    populate_status(&response);
    return RUNTIME_CONFIG_RESPONSE(get_runtime_config_status, response);
}

zmk_studio_Response begin_runtime_update(const zmk_studio_Request *req) {
    const zmk_runtime_config_BeginRuntimeUpdateRequest *request =
        &req->subsystem.runtime_config.request_type.begin_runtime_update;
    zmk_runtime_config_BeginRuntimeUpdateResponse response =
        zmk_runtime_config_BeginRuntimeUpdateResponse_init_zero;
    uint32_t update_id;
    int ret;

    if (request->snapshot_sha256.size != 0U) {
        zmk_studio_Response studio_response =
            RUNTIME_CONFIG_RESPONSE(begin_runtime_update, response);

        set_error(&studio_response,
                  zmk_runtime_config_RuntimeConfigErrorCode_RUNTIME_CONFIG_ERROR_NOT_SUPPORTED);
        return studio_response;
    }

    ret = zmk_runtime_config_begin_update(request->expected_active_generation,
                                          request->snapshot_size, &update_id);
    if (ret != 0) {
        zmk_studio_Response studio_response =
            RUNTIME_CONFIG_RESPONSE(begin_runtime_update, response);

        set_error(&studio_response, error_from_errno(ret));
        return studio_response;
    }

    response.update_id = update_id;
    response.max_chunk_bytes = zmk_runtime_config_max_update_chunk_bytes();
    return RUNTIME_CONFIG_RESPONSE(begin_runtime_update, response);
}

zmk_studio_Response upload_runtime_update_chunk(const zmk_studio_Request *req) {
    const zmk_runtime_config_UploadRuntimeUpdateChunkRequest *request =
        &req->subsystem.runtime_config.request_type.upload_runtime_update_chunk;
    zmk_runtime_config_UploadRuntimeUpdateChunkResponse response =
        zmk_runtime_config_UploadRuntimeUpdateChunkResponse_init_zero;
    size_t accepted_bytes;
    size_t next_offset;
    int ret = zmk_runtime_config_upload_update_chunk(request->update_id, request->offset,
                                                     request->chunk.bytes, request->chunk.size,
                                                     &accepted_bytes, &next_offset);

    if (ret != 0) {
        zmk_studio_Response studio_response =
            RUNTIME_CONFIG_RESPONSE(upload_runtime_update_chunk, response);

        set_error(&studio_response, error_from_errno(ret));
        return studio_response;
    }

    response.accepted_bytes = accepted_bytes;
    response.next_offset = next_offset;
    return RUNTIME_CONFIG_RESPONSE(upload_runtime_update_chunk, response);
}

struct snapshot_decode_context {
    bool unsupported_content;
};

static bool reject_snapshot_content(pb_istream_t *stream, const pb_field_t *field, void **arg) {
    struct snapshot_decode_context *context = *arg;

    ARG_UNUSED(stream);
    ARG_UNUSED(field);

    context->unsupported_content = true;
    return false;
}

static int decode_uploaded_snapshot(uint32_t update_id,
                                    struct zmk_runtime_config_snapshot *snapshot) {
    const uint8_t *snapshot_bytes;
    size_t snapshot_size;
    struct snapshot_decode_context context = {0};
    zmk_runtime_config_RuntimeConfigSnapshot wire_snapshot =
        zmk_runtime_config_RuntimeConfigSnapshot_init_zero;
    pb_istream_t stream;
    int ret = zmk_runtime_config_get_uploaded_snapshot(update_id, &snapshot_bytes, &snapshot_size);

    if (ret != 0) {
        return ret;
    }

    wire_snapshot.keymap_overrides.funcs.decode = reject_snapshot_content;
    wire_snapshot.keymap_overrides.arg = &context;
    wire_snapshot.layers.funcs.decode = reject_snapshot_content;
    wire_snapshot.layers.arg = &context;
    wire_snapshot.runtime_objects.funcs.decode = reject_snapshot_content;
    wire_snapshot.runtime_objects.arg = &context;
    wire_snapshot.combos.funcs.decode = reject_snapshot_content;
    wire_snapshot.combos.arg = &context;

    stream = pb_istream_from_buffer(snapshot_bytes, snapshot_size);
    if (!pb_decode(&stream, &zmk_runtime_config_RuntimeConfigSnapshot_msg, &wire_snapshot)) {
        return context.unsupported_content ? -ENOTSUP : -EBADMSG;
    }

    if (wire_snapshot.persistence_schema_version > UINT16_MAX) {
        return -EPROTONOSUPPORT;
    }

    if (wire_snapshot.generation != 0U) {
        return -EINVAL;
    }

    if (wire_snapshot.capability_fingerprint.size != ZMK_RUNTIME_CAPABILITY_FINGERPRINT_SIZE) {
        return -EXDEV;
    }

    zmk_runtime_config_init_empty_snapshot(snapshot);
    snapshot->persistence_schema_version = wire_snapshot.persistence_schema_version;
    snapshot->generation = wire_snapshot.generation;
    memcpy(snapshot->capability_fingerprint, wire_snapshot.capability_fingerprint.bytes,
           sizeof(snapshot->capability_fingerprint));

    return 0;
}

static void set_resource_usage(zmk_runtime_config_ValidationResult *response,
                               const struct zmk_runtime_config_snapshot *snapshot,
                               size_t serialized_size) {
    struct zmk_runtime_capabilities capabilities;

    zmk_runtime_config_get_capabilities(&capabilities);
    response->has_resource_usage = true;
    response->resource_usage.has_runtime_objects = true;
    response->resource_usage.runtime_objects.used = snapshot ? snapshot->object_count : 0U;
    response->resource_usage.runtime_objects.limit = capabilities.max_objects;
    response->resource_usage.has_combos = true;
    response->resource_usage.combos.used = snapshot ? snapshot->combo_count : 0U;
    response->resource_usage.combos.limit = capabilities.max_combos;
    response->resource_usage.has_macro_steps = true;
    response->resource_usage.macro_steps.used = snapshot ? snapshot->macro_step_count : 0U;
    response->resource_usage.macro_steps.limit = capabilities.max_macro_steps;
    response->resource_usage.has_persisted_bytes = true;
    response->resource_usage.persisted_bytes.used = serialized_size;
    response->resource_usage.persisted_bytes.limit = capabilities.max_persisted_bytes;
    response->resource_usage.has_keymap_overrides = true;
}

zmk_studio_Response validate_runtime_update(const zmk_studio_Request *req) {
    const zmk_runtime_config_ValidateRuntimeUpdateRequest *request =
        &req->subsystem.runtime_config.request_type.validate_runtime_update;
    zmk_runtime_config_ValidationResult response = zmk_runtime_config_ValidationResult_init_zero;
    struct zmk_runtime_config_snapshot snapshot;
    struct zmk_runtime_config_validation_result validation;
    const uint8_t *serialized_snapshot;
    size_t serialized_size = 0U;
    int ret;

    ret = zmk_runtime_config_get_uploaded_snapshot(request->update_id, &serialized_snapshot,
                                                   &serialized_size);
    if (ret == 0) {
        ret = decode_uploaded_snapshot(request->update_id, &snapshot);
    }

    if (ret == 0) {
        ret =
            zmk_runtime_config_stage_uploaded_snapshot(request->update_id, &snapshot, &validation);
    }

    set_resource_usage(&response, ret == 0 ? &snapshot : NULL, serialized_size);
    response.valid = ret == 0;

    zmk_studio_Response studio_response =
        RUNTIME_CONFIG_RESPONSE(validate_runtime_update, response);

    if (ret != 0) {
        set_error(&studio_response, error_from_errno(ret));
    }

    return studio_response;
}

zmk_studio_Response commit_runtime_update(const zmk_studio_Request *req) {
    const zmk_runtime_config_CommitRuntimeUpdateRequest *request =
        &req->subsystem.runtime_config.request_type.commit_runtime_update;
    zmk_runtime_config_CommitRuntimeUpdateResult response =
        zmk_runtime_config_CommitRuntimeUpdateResult_init_zero;
    int ret = zmk_runtime_config_persist_update(request->update_id, &response.generation);

    if (ret != 0) {
        zmk_studio_Response studio_response =
            RUNTIME_CONFIG_RESPONSE(commit_runtime_update, response);

        set_error(&studio_response, error_from_errno(ret));
        return studio_response;
    }

    response.saved = true;
    response.has_status = true;
    populate_status(&response.status);
    response.activation =
        response.status.pending_generation != 0U
            ? zmk_runtime_config_RuntimeConfigActivation_RUNTIME_CONFIG_ACTIVATION_PENDING_IDLE
            : zmk_runtime_config_RuntimeConfigActivation_RUNTIME_CONFIG_ACTIVATION_ACTIVE;
    return RUNTIME_CONFIG_RESPONSE(commit_runtime_update, response);
}

zmk_studio_Response abort_runtime_update(const zmk_studio_Request *req) {
    const zmk_runtime_config_AbortRuntimeUpdateRequest *request =
        &req->subsystem.runtime_config.request_type.abort_runtime_update;
    zmk_runtime_config_AbortRuntimeUpdateResponse response =
        zmk_runtime_config_AbortRuntimeUpdateResponse_init_zero;
    int ret = zmk_runtime_config_abort_update(request->update_id);

    if (ret != 0) {
        zmk_studio_Response studio_response =
            RUNTIME_CONFIG_RESPONSE(abort_runtime_update, response);

        set_error(&studio_response, error_from_errno(ret));
        return studio_response;
    }

    response.aborted = true;
    return RUNTIME_CONFIG_RESPONSE(abort_runtime_update, response);
}

ZMK_RPC_SUBSYSTEM_HANDLER(runtime_config, get_runtime_capabilities,
                          ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(runtime_config, get_runtime_config_status,
                          ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(runtime_config, begin_runtime_update, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(runtime_config, upload_runtime_update_chunk,
                          ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(runtime_config, validate_runtime_update, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(runtime_config, commit_runtime_update, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(runtime_config, abort_runtime_update, ZMK_STUDIO_RPC_HANDLER_SECURED);
