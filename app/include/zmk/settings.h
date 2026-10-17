/*
 * Copyright (c) 2023 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

/**
 * Erases all saved settings.
 *
 * @note This does not automatically update any code using Zephyr's settings
 * subsystem. This should typically be followed by a call to sys_reboot().
 */
int zmk_settings_erase(void);

/** Weak startup hook, called after settings load succeeds and releases its lock. */
#define ZMK_SETTINGS_LOADED_HOOK_VERSION 1
void zmk_settings_loaded(void);
