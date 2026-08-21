/*
 * Copyright (c) 2026 The ZMK Next Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zmk/behavior.h>

struct zmk_runtime_tap_dance_action;
struct zmk_runtime_tap_dance_config;

/* Starts or continues a runtime tap-dance invocation for a physical position. */
int zmk_behavior_tap_dance_runtime_pressed(
    const struct zmk_runtime_tap_dance_config *config,
    const struct zmk_runtime_tap_dance_action *actions,
    struct zmk_behavior_binding_event event);
int zmk_behavior_tap_dance_runtime_released(struct zmk_behavior_binding_event event);
