/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zmk/keys.h>
#include <zmk/hid.h>

/* With CONFIG_ZMK_BLE_DIAGNOSTIC_HOOKS, modules can override these weak hooks to watch HID
 * subscriptions and keyboard report delivery. */
struct bt_conn;
void zmk_hog_subscription_observed(struct bt_conn *conn);
/* Return true to get zmk_hog_keyboard_report_result() for this report. */
bool zmk_hog_keyboard_report_attempted(struct bt_conn *conn, bool subscribed);
void zmk_hog_keyboard_report_result(struct bt_conn *conn, int err);

int zmk_hog_send_keyboard_report(struct zmk_hid_keyboard_report_body *body);
int zmk_hog_send_consumer_report(struct zmk_hid_consumer_report_body *body);

#if IS_ENABLED(CONFIG_ZMK_POINTING)
int zmk_hog_send_mouse_report(struct zmk_hid_mouse_report_body *body);
#endif // IS_ENABLED(CONFIG_ZMK_POINTING)
