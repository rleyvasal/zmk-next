/*
 * Copyright (c) 2026 The ZMK Next Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>

#include <zmk/behavior.h>
#include <zmk/runtime_config.h>

int zmk_runtime_dispatch_object(zmk_runtime_object_id_t object_id,
                                struct zmk_behavior_binding_event event, bool pressed);
