/*
 * Copyright (c) 2026 The ZMK Next Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zmk/behavior.h>

struct zmk_runtime_hold_tap_config;

/* Starts a runtime-configured hold-tap using the shared hold-tap engine. The
 * child bindings are resolved before this call and remain immutable for the
 * lifetime of the key press. */
int zmk_behavior_hold_tap_runtime_pressed(const struct zmk_runtime_hold_tap_config *config,
                                          const struct zmk_behavior_binding *tap_binding,
                                          const struct zmk_behavior_binding *hold_binding,
                                          struct zmk_behavior_binding_event event);

/* Releases the runtime-configured hold-tap associated with event.position. */
int zmk_behavior_hold_tap_runtime_released(struct zmk_behavior_binding_event event);
