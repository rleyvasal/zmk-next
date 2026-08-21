/*
 * Copyright (c) 2026 The ZMK Next Contributors
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/logging/log.h>

#include <pb_decode.h>
#include <pb_encode.h>

#include <zmk/runtime_config.h>
#include <zmk/keymap.h>
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
    case -EEXIST:
    case -ENODEV:
        return zmk_runtime_config_RuntimeConfigErrorCode_RUNTIME_CONFIG_ERROR_VALIDATION;
    case -EIO:
    case -ENOSYS:
        return zmk_runtime_config_RuntimeConfigErrorCode_RUNTIME_CONFIG_ERROR_PERSISTENCE;
    default:
        return zmk_runtime_config_RuntimeConfigErrorCode_RUNTIME_CONFIG_ERROR_INVALID_REQUEST;
    }
}

static bool encode_runtime_features(pb_ostream_t *stream, const pb_field_t *field,
                                    void *const *arg) {
    const uint32_t features[] = {
        zmk_runtime_config_RuntimeFeature_RUNTIME_FEATURE_KEYMAP_OVERRIDES,
        zmk_runtime_config_RuntimeFeature_RUNTIME_FEATURE_MOD_MORPHS,
        zmk_runtime_config_RuntimeFeature_RUNTIME_FEATURE_MACROS,
        zmk_runtime_config_RuntimeFeature_RUNTIME_FEATURE_MACRO_WAIT,
        zmk_runtime_config_RuntimeFeature_RUNTIME_FEATURE_MACRO_PAUSE_UNTIL_RELEASE,
        zmk_runtime_config_RuntimeFeature_RUNTIME_FEATURE_COMBOS,
        zmk_runtime_config_RuntimeFeature_RUNTIME_FEATURE_COMBO_SLOW_RELEASE,
        zmk_runtime_config_RuntimeFeature_RUNTIME_FEATURE_COMBO_REQUIRE_PRIOR_IDLE,
        zmk_runtime_config_RuntimeFeature_RUNTIME_FEATURE_HOLD_TAPS,
        zmk_runtime_config_RuntimeFeature_RUNTIME_FEATURE_HOLD_TAP_QUICK_TAP,
        zmk_runtime_config_RuntimeFeature_RUNTIME_FEATURE_HOLD_TAP_REQUIRE_PRIOR_IDLE,
        zmk_runtime_config_RuntimeFeature_RUNTIME_FEATURE_TAP_DANCES,
    };

    ARG_UNUSED(arg);

    for (size_t i = 0; i < ARRAY_SIZE(features); i++) {
        if (!pb_encode_tag_for_field(stream, field) || !pb_encode_varint(stream, features[i])) {
            return false;
        }
    }

    return true;
}

static bool encode_runtime_object_types(pb_ostream_t *stream, const pb_field_t *field,
                                        void *const *arg) {
    const uint32_t types[] = {
        zmk_runtime_config_RuntimeObjectType_RUNTIME_OBJECT_TYPE_MOD_MORPH,
        zmk_runtime_config_RuntimeObjectType_RUNTIME_OBJECT_TYPE_MACRO,
        zmk_runtime_config_RuntimeObjectType_RUNTIME_OBJECT_TYPE_HOLD_TAP,
        zmk_runtime_config_RuntimeObjectType_RUNTIME_OBJECT_TYPE_TAP_DANCE,
    };

    ARG_UNUSED(arg);

    for (size_t i = 0; i < ARRAY_SIZE(types); i++) {
        if (!pb_encode_tag_for_field(stream, field) || !pb_encode_varint(stream, types[i])) {
            return false;
        }
    }

    return true;
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
    response.limits.max_tap_dance_actions = capabilities.max_tap_dance_actions;
    response.limits.max_persisted_bytes = capabilities.max_persisted_bytes;
    response.limits.max_layers = ZMK_KEYMAP_LAYERS_LEN;
    response.limits.max_keymap_overrides = capabilities.max_keymap_overrides;
    response.supported_features.funcs.encode = encode_runtime_features;
    response.supported_object_types.funcs.encode = encode_runtime_object_types;

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
    uint32_t update_id;
    uint16_t keymap_override_count;
    uint16_t object_count;
    uint16_t combo_count;
    uint16_t macro_step_count;
    uint16_t tap_dance_action_count;
    uint16_t tap_dance_action_offset;
    int error;
    bool unsupported_content;
};

static bool reject_snapshot_content(pb_istream_t *stream, const pb_field_t *field, void **arg) {
    struct snapshot_decode_context *context = *arg;

    ARG_UNUSED(stream);
    ARG_UNUSED(field);

    context->unsupported_content = true;
    return false;
}

static int decode_action_reference(const zmk_runtime_config_ActionReference *wire_action,
                                   bool allow_runtime_object,
                                   struct zmk_runtime_action_ref *action) {
    if (!wire_action || !action) {
        return -EINVAL;
    }

    if (wire_action->which_target ==
        zmk_runtime_config_ActionReference_compiled_behavior_tag) {
        if (wire_action->target.compiled_behavior.behavior_id == 0U ||
            wire_action->target.compiled_behavior.behavior_id > UINT16_MAX) {
            return -EINVAL;
        }

        *action = (struct zmk_runtime_action_ref){
            .kind = ZMK_RUNTIME_ACTION_COMPILED_BEHAVIOR,
            .data.compiled = {
                .local_id = wire_action->target.compiled_behavior.behavior_id,
                .param1 = wire_action->target.compiled_behavior.param1,
                .param2 = wire_action->target.compiled_behavior.param2,
            },
        };
        return 0;
    }

    if (allow_runtime_object &&
        wire_action->which_target == zmk_runtime_config_ActionReference_runtime_object_id_tag &&
        wire_action->target.runtime_object_id != ZMK_RUNTIME_OBJECT_ID_INVALID) {
        *action = (struct zmk_runtime_action_ref){
            .kind = ZMK_RUNTIME_ACTION_OBJECT,
            .data.object_id = wire_action->target.runtime_object_id,
        };
        return 0;
    }

    return -EINVAL;
}

static bool decode_keymap_override(pb_istream_t *stream, const pb_field_t *field, void **arg) {
    struct snapshot_decode_context *context = *arg;
    zmk_runtime_config_KeymapOverride wire_override =
        zmk_runtime_config_KeymapOverride_init_zero;
    struct zmk_runtime_keymap_override override = {0};
    int ret;

    ARG_UNUSED(field);

    if (!pb_decode(stream, &zmk_runtime_config_KeymapOverride_msg, &wire_override)) {
        return false;
    }

    override.layer_id = wire_override.layer_id;
    override.key_position = wire_override.key_position;

    if (!wire_override.has_action || wire_override.layer_id > UINT8_MAX ||
        wire_override.key_position > UINT16_MAX ||
        decode_action_reference(&wire_override.action, true, &override.action) != 0) {
        context->error = -EINVAL;
        return false;
    }

    ret = zmk_runtime_config_append_staged_keymap_override(context->update_id, &override);
    if (ret != 0) {
        context->error = ret;
        return false;
    }

    context->keymap_override_count++;
    return true;
}

static bool decode_macro_step(pb_istream_t *stream, const pb_field_t *field, void **arg) {
    struct snapshot_decode_context *context = *arg;
    zmk_runtime_config_MacroStep wire_step = zmk_runtime_config_MacroStep_init_zero;
    struct zmk_runtime_macro_step step = {0};
    int ret;

    ARG_UNUSED(field);

    if (!pb_decode(stream, &zmk_runtime_config_MacroStep_msg, &wire_step)) {
        return false;
    }

    switch (wire_step.which_instruction) {
    case zmk_runtime_config_MacroStep_tap_tag:
        step.type = ZMK_RUNTIME_MACRO_STEP_TAP;
        ret = decode_action_reference(&wire_step.instruction.tap, false, &step.data.action);
        break;
    case zmk_runtime_config_MacroStep_press_tag:
        step.type = ZMK_RUNTIME_MACRO_STEP_PRESS;
        ret = decode_action_reference(&wire_step.instruction.press, false, &step.data.action);
        break;
    case zmk_runtime_config_MacroStep_release_tag:
        step.type = ZMK_RUNTIME_MACRO_STEP_RELEASE;
        ret = decode_action_reference(&wire_step.instruction.release, false, &step.data.action);
        break;
    case zmk_runtime_config_MacroStep_wait_ms_tag:
        step.type = ZMK_RUNTIME_MACRO_STEP_WAIT;
        step.data.duration_ms = wire_step.instruction.wait_ms;
        ret = 0;
        break;
    case zmk_runtime_config_MacroStep_pause_until_release_tag:
        step.type = ZMK_RUNTIME_MACRO_STEP_PAUSE_UNTIL_RELEASE;
        ret = wire_step.instruction.pause_until_release ? 0 : -EINVAL;
        break;
    default:
        ret = -EINVAL;
        break;
    }

    if (ret != 0) {
        context->error = ret;
        return false;
    }

    ret = zmk_runtime_config_append_staged_macro_step(context->update_id, &step);
    if (ret != 0) {
        context->error = ret;
        return false;
    }

    context->macro_step_count++;
    return true;
}

static bool decode_tap_dance_action(pb_istream_t *stream, const pb_field_t *field, void **arg) {
    struct snapshot_decode_context *context = *arg;
    zmk_runtime_config_TapDanceAction wire_action =
        zmk_runtime_config_TapDanceAction_init_zero;
    struct zmk_runtime_tap_dance_action action = {0};
    uint16_t expected_tap_count;
    int ret;

    ARG_UNUSED(field);

    if (!pb_decode(stream, &zmk_runtime_config_TapDanceAction_msg, &wire_action) ||
        !wire_action.has_tap_action || !wire_action.has_hold_action ||
        context->tap_dance_action_count >= CONFIG_ZMK_RUNTIME_MAX_TAP_DANCE_ACTIONS) {
        context->error = -EINVAL;
        return false;
    }

    expected_tap_count = context->tap_dance_action_count - context->tap_dance_action_offset + 1U;
    if (wire_action.tap_count != expected_tap_count) {
        context->error = -EINVAL;
        return false;
    }

    ret = decode_action_reference(&wire_action.tap_action, false, &action.tap_action);
    if (ret == 0) {
        ret = decode_action_reference(&wire_action.hold_action, false, &action.hold_action);
    }
    if (ret != 0) {
        context->error = ret;
        return false;
    }

    ret = zmk_runtime_config_append_staged_tap_dance_action(context->update_id, &action);
    if (ret != 0) {
        context->error = ret;
        return false;
    }

    context->tap_dance_action_count++;
    return true;
}

static bool decode_runtime_object(pb_istream_t *stream, const pb_field_t *field, void **arg) {
    struct snapshot_decode_context *context = *arg;
    struct zmk_runtime_object_slot object = {0};
    uint32_t wire_id = 0U;
    bool has_id = false;
    bool has_definition = false;
    bool eof = false;
    int ret;

    ARG_UNUSED(field);

    while (stream->bytes_left != 0U) {
        pb_wire_type_t wire_type;
        uint32_t tag;

        if (!pb_decode_tag(stream, &wire_type, &tag, &eof)) {
            return false;
        }

        if (eof) {
            break;
        }

        switch (tag) {
        case zmk_runtime_config_RuntimeObject_id_tag:
            if (has_id || wire_type != PB_WT_VARINT || !pb_decode_varint(stream, &wire_id) ||
                wire_id == ZMK_RUNTIME_OBJECT_ID_INVALID) {
                context->error = -EINVAL;
                return false;
            }
            has_id = true;
            break;
        case zmk_runtime_config_RuntimeObject_mod_morph_tag: {
            pb_istream_t substream;
            zmk_runtime_config_ModMorphObject wire_mod_morph =
                zmk_runtime_config_ModMorphObject_init_zero;

            if (has_definition || wire_type != PB_WT_STRING ||
                !pb_make_string_substream(stream, &substream) ||
                !pb_decode(&substream, &zmk_runtime_config_ModMorphObject_msg, &wire_mod_morph) ||
                !pb_close_string_substream(stream, &substream) ||
                !wire_mod_morph.has_normal_action || !wire_mod_morph.has_morphed_action ||
                decode_action_reference(&wire_mod_morph.normal_action, false,
                                        &object.data.mod_morph.normal_action) != 0 ||
                decode_action_reference(&wire_mod_morph.morphed_action, false,
                                        &object.data.mod_morph.morphed_action) != 0) {
                context->error = -EINVAL;
                return false;
            }

            object.type = ZMK_RUNTIME_OBJECT_TYPE_MOD_MORPH;
            object.data.mod_morph.modifiers = wire_mod_morph.modifiers;
            has_definition = true;
            break;
        }
        case zmk_runtime_config_RuntimeObject_macro_tag: {
            pb_istream_t substream;
            zmk_runtime_config_MacroObject wire_macro = zmk_runtime_config_MacroObject_init_zero;
            uint16_t step_offset = context->macro_step_count;

            wire_macro.steps.funcs.decode = decode_macro_step;
            wire_macro.steps.arg = context;
            if (has_definition || wire_type != PB_WT_STRING ||
                !pb_make_string_substream(stream, &substream) ||
                !pb_decode(&substream, &zmk_runtime_config_MacroObject_msg, &wire_macro) ||
                !pb_close_string_substream(stream, &substream)) {
                if (context->error == 0) {
                    context->error = -EINVAL;
                }
                return false;
            }

            object.type = ZMK_RUNTIME_OBJECT_TYPE_MACRO;
            object.data.macro.step_offset = step_offset;
            object.data.macro.step_count = context->macro_step_count - step_offset;
            has_definition = true;
            break;
        }
        case zmk_runtime_config_RuntimeObject_hold_tap_tag: {
            pb_istream_t substream;
            zmk_runtime_config_HoldTapObject wire_hold_tap =
                zmk_runtime_config_HoldTapObject_init_zero;

            if (has_definition || wire_type != PB_WT_STRING ||
                !pb_make_string_substream(stream, &substream) ||
                !pb_decode(&substream, &zmk_runtime_config_HoldTapObject_msg, &wire_hold_tap) ||
                !pb_close_string_substream(stream, &substream) || !wire_hold_tap.has_tap_action ||
                !wire_hold_tap.has_hold_action ||
                wire_hold_tap.flavor <
                    zmk_runtime_config_HoldTapFlavor_HOLD_TAP_FLAVOR_HOLD_PREFERRED ||
                wire_hold_tap.flavor >
                    zmk_runtime_config_HoldTapFlavor_HOLD_TAP_FLAVOR_TAP_UNLESS_INTERRUPTED ||
                decode_action_reference(&wire_hold_tap.tap_action, false,
                                        &object.data.hold_tap.tap_action) != 0 ||
                decode_action_reference(&wire_hold_tap.hold_action, false,
                                        &object.data.hold_tap.hold_action) != 0) {
                context->error = -EINVAL;
                return false;
            }

            object.type = ZMK_RUNTIME_OBJECT_TYPE_HOLD_TAP;
            object.data.hold_tap.flavor = wire_hold_tap.flavor;
            object.data.hold_tap.tapping_term_ms = wire_hold_tap.tapping_term_ms;
            object.data.hold_tap.quick_tap_ms = wire_hold_tap.quick_tap_ms;
            object.data.hold_tap.require_prior_idle_ms = wire_hold_tap.require_prior_idle_ms;
            has_definition = true;
            break;
        }
        case zmk_runtime_config_RuntimeObject_tap_dance_tag: {
            pb_istream_t substream;
            zmk_runtime_config_TapDanceObject wire_tap_dance =
                zmk_runtime_config_TapDanceObject_init_zero;
            uint16_t action_offset = context->tap_dance_action_count;

            context->tap_dance_action_offset = action_offset;
            wire_tap_dance.actions.funcs.decode = decode_tap_dance_action;
            wire_tap_dance.actions.arg = context;
            if (has_definition || wire_type != PB_WT_STRING ||
                !pb_make_string_substream(stream, &substream) ||
                !pb_decode(&substream, &zmk_runtime_config_TapDanceObject_msg, &wire_tap_dance) ||
                !pb_close_string_substream(stream, &substream) ||
                context->tap_dance_action_count == action_offset ||
                wire_tap_dance.tapping_term_ms == 0U) {
                if (context->error == 0) {
                    context->error = -EINVAL;
                }
                return false;
            }

            object.type = ZMK_RUNTIME_OBJECT_TYPE_TAP_DANCE;
            object.data.tap_dance.action_offset = action_offset;
            object.data.tap_dance.action_count =
                context->tap_dance_action_count - action_offset;
            object.data.tap_dance.tapping_term_ms = wire_tap_dance.tapping_term_ms;
            has_definition = true;
            break;
        }
        default:
            if (!pb_skip_field(stream, wire_type)) {
                return false;
            }
            break;
        }
    }

    if (!has_id || !has_definition) {
        context->error = -EINVAL;
        return false;
    }

    object.id = wire_id;
    ret = zmk_runtime_config_append_staged_object(context->update_id, &object);
    if (ret != 0) {
        context->error = ret;
        return false;
    }

    context->object_count++;
    return true;
}

struct combo_decode_context {
    struct zmk_runtime_combo_slot combo;
    int error;
};

static bool decode_combo_position(pb_istream_t *stream, const pb_field_t *field, void **arg) {
    struct combo_decode_context *context = *arg;
    uint32_t position = 0U;

    ARG_UNUSED(field);

    if (!pb_decode_varint(stream, &position) || position > UINT16_MAX ||
        context->combo.key_count >= CONFIG_ZMK_RUNTIME_MAX_COMBO_KEYS) {
        context->error = -EINVAL;
        return false;
    }

    context->combo.positions[context->combo.key_count++] = position;
    return true;
}

static bool decode_combo_definition(pb_istream_t *stream, const pb_field_t *field, void **arg) {
    struct snapshot_decode_context *snapshot_context = *arg;
    struct combo_decode_context combo_context = {0};
    zmk_runtime_config_ComboDefinition wire_combo =
        zmk_runtime_config_ComboDefinition_init_zero;
    int ret;

    ARG_UNUSED(field);

    wire_combo.key_positions.funcs.decode = decode_combo_position;
    wire_combo.key_positions.arg = &combo_context;
    if (!pb_decode(stream, &zmk_runtime_config_ComboDefinition_msg, &wire_combo)) {
        snapshot_context->error = combo_context.error != 0 ? combo_context.error : -EBADMSG;
        return false;
    }

    if (wire_combo.id == ZMK_RUNTIME_OBJECT_ID_INVALID || !wire_combo.has_output ||
        decode_action_reference(&wire_combo.output, true, &combo_context.combo.output) != 0) {
        snapshot_context->error = -EINVAL;
        return false;
    }

    combo_context.combo.id = wire_combo.id;
    combo_context.combo.timeout_ms = wire_combo.timeout_ms;
    combo_context.combo.require_prior_idle_ms = wire_combo.require_prior_idle_ms;
    combo_context.combo.slow_release = wire_combo.slow_release;
    ret = zmk_runtime_config_append_staged_combo(snapshot_context->update_id,
                                                 &combo_context.combo);
    if (ret != 0) {
        snapshot_context->error = ret;
        return false;
    }

    snapshot_context->combo_count++;
    return true;
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

    ret = zmk_runtime_config_set_staged_keymap_overrides(update_id, NULL, 0U);
    if (ret != 0) {
        return ret;
    }

    ret = zmk_runtime_config_set_staged_objects(update_id, NULL, 0U);
    if (ret != 0) {
        return ret;
    }

    ret = zmk_runtime_config_set_staged_combos(update_id, NULL, 0U);
    if (ret != 0) {
        return ret;
    }

    ret = zmk_runtime_config_set_staged_macro_steps(update_id, NULL, 0U);
    if (ret != 0) {
        return ret;
    }

    context.update_id = update_id;
    wire_snapshot.keymap_overrides.funcs.decode = decode_keymap_override;
    wire_snapshot.keymap_overrides.arg = &context;
    wire_snapshot.layers.funcs.decode = reject_snapshot_content;
    wire_snapshot.layers.arg = &context;
    wire_snapshot.runtime_objects.funcs.decode = decode_runtime_object;
    wire_snapshot.runtime_objects.arg = &context;
    wire_snapshot.combos.funcs.decode = decode_combo_definition;
    wire_snapshot.combos.arg = &context;

    stream = pb_istream_from_buffer(snapshot_bytes, snapshot_size);
    if (!pb_decode(&stream, &zmk_runtime_config_RuntimeConfigSnapshot_msg, &wire_snapshot)) {
        if (context.error != 0) {
            return context.error;
        }
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
    snapshot->keymap_override_count = context.keymap_override_count;
    snapshot->object_count = context.object_count;
    snapshot->combo_count = context.combo_count;
    snapshot->macro_step_count = context.macro_step_count;
    snapshot->tap_dance_action_count = context.tap_dance_action_count;
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
    response->resource_usage.has_tap_dance_actions = true;
    response->resource_usage.tap_dance_actions.used =
        snapshot ? snapshot->tap_dance_action_count : 0U;
    response->resource_usage.tap_dance_actions.limit = capabilities.max_tap_dance_actions;
    response->resource_usage.has_persisted_bytes = true;
    response->resource_usage.persisted_bytes.used = serialized_size;
    response->resource_usage.persisted_bytes.limit = capabilities.max_persisted_bytes;
    response->resource_usage.has_keymap_overrides = true;
    response->resource_usage.keymap_overrides.used = snapshot ? snapshot->keymap_override_count : 0U;
    response->resource_usage.keymap_overrides.limit = capabilities.max_keymap_overrides;
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

    if (ret == 0) {
        ret = zmk_runtime_config_get_persistable_update_size(request->update_id, &serialized_size);
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
