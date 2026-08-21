/*
 * Copyright (c) 2026 The ZMK Next Contributors
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/matrix.h>
#include <zmk/runtime_config.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct {
    bool physical_key_down[ZMK_KEYMAP_LEN];
    struct zmk_runtime_config_activation_status status;
} runtime_config_activation;

K_MUTEX_DEFINE(runtime_config_activation_lock);

static void runtime_config_activation_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(runtime_config_activation_work, runtime_config_activation_work_handler);

static bool activation_is_safe(void) {
    return runtime_config_activation.status.pressed_key_count == 0U &&
           runtime_config_activation.status.blocker_count == 0U;
}

static void schedule_activation_if_safe(void) {
    bool schedule;

    k_mutex_lock(&runtime_config_activation_lock, K_FOREVER);
    schedule = runtime_config_activation.status.pending_generation != 0U && activation_is_safe();
    k_mutex_unlock(&runtime_config_activation_lock);

    if (schedule) {
        k_work_reschedule(&runtime_config_activation_work,
                          K_MSEC(CONFIG_ZMK_RUNTIME_CONFIG_ACTIVATION_IDLE_MS));
    }
}

static void runtime_config_activation_work_handler(struct k_work *work) {
    uint32_t generation = 0U;

    ARG_UNUSED(work);

    k_mutex_lock(&runtime_config_activation_lock, K_FOREVER);
    if (runtime_config_activation.status.pending_generation != 0U && activation_is_safe()) {
        generation = runtime_config_activation.status.pending_generation;
        runtime_config_activation.status.active_generation = generation;
        runtime_config_activation.status.pending_generation = 0U;
    }
    k_mutex_unlock(&runtime_config_activation_lock);

    if (generation != 0U) {
        LOG_INF("Activated Runtime Config generation %u", generation);
    }
}

int zmk_runtime_config_request_activation(uint32_t generation) {
    if (generation == 0U) {
        return -EINVAL;
    }

    k_mutex_lock(&runtime_config_activation_lock, K_FOREVER);
    if (generation == runtime_config_activation.status.active_generation) {
        runtime_config_activation.status.pending_generation = 0U;
    } else {
        runtime_config_activation.status.pending_generation = generation;
    }
    k_mutex_unlock(&runtime_config_activation_lock);

    schedule_activation_if_safe();
    return 0;
}

int zmk_runtime_config_note_key_state(uint32_t position, bool pressed) {
    bool reschedule = false;

    if (position >= ZMK_KEYMAP_LEN) {
        return -EINVAL;
    }

    k_mutex_lock(&runtime_config_activation_lock, K_FOREVER);
    if (pressed && !runtime_config_activation.physical_key_down[position]) {
        runtime_config_activation.physical_key_down[position] = true;
        runtime_config_activation.status.pressed_key_count++;
    } else if (!pressed && runtime_config_activation.physical_key_down[position]) {
        runtime_config_activation.physical_key_down[position] = false;
        runtime_config_activation.status.pressed_key_count--;
    }
    reschedule = runtime_config_activation.status.pending_generation != 0U && activation_is_safe();
    k_mutex_unlock(&runtime_config_activation_lock);

    if (pressed) {
        k_work_cancel_delayable(&runtime_config_activation_work);
    } else if (reschedule) {
        k_work_reschedule(&runtime_config_activation_work,
                          K_MSEC(CONFIG_ZMK_RUNTIME_CONFIG_ACTIVATION_IDLE_MS));
    }

    return 0;
}

int zmk_runtime_config_activation_blocker_acquire(void) {
    k_mutex_lock(&runtime_config_activation_lock, K_FOREVER);
    if (runtime_config_activation.status.blocker_count == UINT16_MAX) {
        k_mutex_unlock(&runtime_config_activation_lock);
        return -EOVERFLOW;
    }

    runtime_config_activation.status.blocker_count++;
    k_mutex_unlock(&runtime_config_activation_lock);
    k_work_cancel_delayable(&runtime_config_activation_work);
    return 0;
}

int zmk_runtime_config_activation_blocker_release(void) {
    bool reschedule;

    k_mutex_lock(&runtime_config_activation_lock, K_FOREVER);
    if (runtime_config_activation.status.blocker_count == 0U) {
        k_mutex_unlock(&runtime_config_activation_lock);
        return -EINVAL;
    }

    runtime_config_activation.status.blocker_count--;
    reschedule = runtime_config_activation.status.pending_generation != 0U && activation_is_safe();
    k_mutex_unlock(&runtime_config_activation_lock);

    if (reschedule) {
        k_work_reschedule(&runtime_config_activation_work,
                          K_MSEC(CONFIG_ZMK_RUNTIME_CONFIG_ACTIVATION_IDLE_MS));
    }

    return 0;
}

void zmk_runtime_config_get_activation_status(
    struct zmk_runtime_config_activation_status *status) {
    if (!status) {
        return;
    }

    k_mutex_lock(&runtime_config_activation_lock, K_FOREVER);
    *status = runtime_config_activation.status;
    k_mutex_unlock(&runtime_config_activation_lock);
}
