/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>

#include <zephyr/settings/settings.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci_types.h>

#if IS_ENABLED(CONFIG_SETTINGS)

#include <zephyr/settings/settings.h>

#endif

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/ble.h>
#include <zmk/keys.h>
#include <zmk/split/bluetooth/uuid.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>

#if IS_ENABLED(CONFIG_TOTEM_ADV_THROTTLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/events/position_state_changed.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_BLE_PASSKEY_ENTRY)
#include <zmk/events/keycode_state_changed.h>

#define PASSKEY_DIGITS 6

static struct bt_conn *auth_passkey_entry_conn;
static uint8_t passkey_entries[PASSKEY_DIGITS];
static uint8_t passkey_entry_count;

#endif /* IS_ENABLED(CONFIG_ZMK_BLE_PASSKEY_ENTRY) */

enum advertising_type {
    ZMK_ADV_NONE,
    ZMK_ADV_DIR,
    ZMK_ADV_CONN,
} advertising_status;

#define CURR_ADV(adv) (adv << 4)

#define ZMK_ADV_OPTS_BASE                                                                          \
    (BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_USE_NAME | BT_LE_ADV_OPT_FORCE_NAME_IN_AD)

#define ZMK_ADV_CONN_NAME                                                                          \
    BT_LE_ADV_PARAM(ZMK_ADV_OPTS_BASE, BT_GAP_ADV_FAST_INT_MIN_2, BT_GAP_ADV_FAST_INT_MAX_2, NULL)

/* Totem advertising boost: denser open-adv while armed (see TOTEM_ADV_BOOST). */
#if IS_ENABLED(CONFIG_TOTEM_ADV_THROTTLE) && IS_ENABLED(CONFIG_TOTEM_ADV_BOOST) &&                 \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static bool totem_adv_boost_active = false;
#else
static const bool totem_adv_boost_active = false;
#endif

/* FAST_1 = 30-60 ms (boost), FAST_2 = 100-150 ms (ZMK default). Compound literals
 * live for the full expression at the bt_le_adv_start call site. */
#define ZMK_ADV_CONN_NAME_BOOST                                                                    \
    BT_LE_ADV_PARAM(ZMK_ADV_OPTS_BASE, BT_GAP_ADV_FAST_INT_MIN_1, BT_GAP_ADV_FAST_INT_MAX_1, NULL)

#define ZMK_ADV_CONN_NAME_FILTER                                                                   \
    BT_LE_ADV_PARAM(ZMK_ADV_OPTS_BASE | BT_LE_ADV_OPT_FILTER_CONN, BT_GAP_ADV_FAST_INT_MIN_2,      \
                    BT_GAP_ADV_FAST_INT_MAX_2, NULL)

#define ZMK_ADV_CONN_NAME_BOOST_FILTER                                                             \
    BT_LE_ADV_PARAM(ZMK_ADV_OPTS_BASE | BT_LE_ADV_OPT_FILTER_CONN, BT_GAP_ADV_FAST_INT_MIN_1,      \
                    BT_GAP_ADV_FAST_INT_MAX_1, NULL)

static struct zmk_ble_profile profiles[ZMK_BLE_PROFILE_COUNT];
static uint8_t active_profile;

#define DEVICE_NAME CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

BUILD_ASSERT(
    DEVICE_NAME_LEN <= CONFIG_BT_DEVICE_NAME_MAX,
    "ERROR: BLE device name is too long. Max length: " STRINGIFY(CONFIG_BT_DEVICE_NAME_MAX));

static struct bt_data zmk_ble_ad[] = {
    BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE, BT_BYTES_LIST_LE16(CONFIG_BT_DEVICE_APPEARANCE)),
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID16_SOME, BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL), /* HID Service */
                  BT_UUID_16_ENCODE(BT_UUID_BAS_VAL)                        /* Battery Service */
                  ),
};

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

static bt_addr_le_t peripheral_addrs[ZMK_SPLIT_BLE_PERIPHERAL_COUNT];

#endif /* IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) */

static void raise_profile_changed_event(void) {
    raise_zmk_ble_active_profile_changed((struct zmk_ble_active_profile_changed){
        .index = active_profile, .profile = &profiles[active_profile]});
}

static void raise_profile_changed_event_callback(struct k_work *work) {
    raise_profile_changed_event();
}

K_WORK_DEFINE(raise_profile_changed_event_work, raise_profile_changed_event_callback);

bool zmk_ble_active_profile_is_open(void) { return zmk_ble_profile_is_open(active_profile); }

bool zmk_ble_profile_is_open(uint8_t index) {
    if (index >= ZMK_BLE_PROFILE_COUNT) {
        return false;
    }
    return !bt_addr_le_cmp(&profiles[index].peer, BT_ADDR_LE_ANY);
}

void set_profile_address(uint8_t index, const bt_addr_le_t *addr) {
    char setting_name[17];
    char addr_str[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));

    memcpy(&profiles[index].peer, addr, sizeof(bt_addr_le_t));
    sprintf(setting_name, "ble/profiles/%d", index);
    LOG_DBG("Setting profile addr for %s to %s", setting_name, addr_str);
#if IS_ENABLED(CONFIG_SETTINGS)
    settings_save_one(setting_name, &profiles[index], sizeof(struct zmk_ble_profile));
#endif
    k_work_submit(&raise_profile_changed_event_work);
}

bool zmk_ble_active_profile_is_connected(void) {
    return zmk_ble_profile_is_connected(active_profile);
}

static void profile_connected_foreach(struct bt_conn *conn, void *data) {
    struct {
        uint8_t index;
        bool found;
    } *ctx = data;
    struct bt_conn_info info;

    if (ctx->found) {
        return;
    }
    if (bt_conn_get_info(conn, &info) != 0 || info.role != BT_CONN_ROLE_PERIPHERAL) {
        return;
    }
    if (info.state != BT_CONN_STATE_CONNECTED) {
        return;
    }
    if (zmk_ble_profile_index(bt_conn_get_dst(conn)) == ctx->index) {
        ctx->found = true;
    }
}

bool zmk_ble_profile_is_connected(uint8_t index) {
    if (index >= ZMK_BLE_PROFILE_COUNT) {
        return false;
    }
    struct bt_conn *conn;
    struct bt_conn_info info;
    bt_addr_le_t *addr = &profiles[index].peer;
    if (!bt_addr_le_cmp(addr, BT_ADDR_LE_ANY)) {
        return false;
    } else if ((conn = bt_conn_lookup_addr_le(BT_ID_DEFAULT, addr)) != NULL) {
        bool connected = bt_conn_get_info(conn, &info) == 0 &&
                         info.role == BT_CONN_ROLE_PERIPHERAL &&
                         info.state == BT_CONN_STATE_CONNECTED;
        bt_conn_unref(conn);
        if (connected) {
            return true;
        }
    }

    /* RPA-safe fallback: any host conn whose resolved profile is this index.
     *
     * A lookup by the stored identity may briefly return an old, disconnected
     * connection while the host's current RPA connection is already live. Do
     * not return false for that stale object: doing so makes endpoint selection
     * and reconnect recovery treat the active host as disconnected. */
    struct {
        uint8_t index;
        bool found;
    } ctx = {.index = index, .found = false};
    bt_conn_foreach(BT_CONN_TYPE_LE, profile_connected_foreach, &ctx);
    return ctx.found;
}

#define CHECKED_ADV_STOP()                                                                         \
    err = bt_le_adv_stop();                                                                        \
    advertising_status = ZMK_ADV_NONE;                                                             \
    if (err) {                                                                                     \
        LOG_ERR("Failed to stop advertising (err %d)", err);                                       \
        return err;                                                                                \
    }

/* Directed advertising to the active bonded peer. Used after profile switch to
 * invite that host faster than undirected discovery (helps Windows especially).
 * Privacy centrals (macOS): include DIR_ADDR_RPA so TargetA can be the peer's
 * RPA. Failures fall through to open undirected (caller handles). */
#define CHECKED_DIR_ADV()                                                                          \
    do {                                                                                           \
        addr = zmk_ble_active_profile_addr();                                                      \
        if (addr == NULL || !bt_addr_le_cmp(addr, BT_ADDR_LE_ANY)) {                               \
            err = -EINVAL;                                                                         \
            break;                                                                                 \
        }                                                                                          \
        conn = bt_conn_lookup_addr_le(BT_ID_DEFAULT, addr);                                        \
        if (conn != NULL) {                                                                        \
            LOG_DBG("Skipping directed advertising, profile host already connected");              \
            bt_conn_unref(conn);                                                                   \
            err = 0;                                                                               \
            break;                                                                                 \
        }                                                                                          \
        /* Low-duty directed can run longer than high-duty (~1.28s cap). Peer may be               \
         * a privacy central — request RPA TargetA when the stack supports it. */                \
        struct bt_le_adv_param dir_param = *BT_LE_ADV_CONN_DIR_LOW_DUTY(addr);                     \
        dir_param.options |= BT_LE_ADV_OPT_DIR_ADDR_RPA;                                           \
        err = bt_le_adv_start(&dir_param, zmk_ble_ad, ARRAY_SIZE(zmk_ble_ad), NULL, 0);            \
        if (err) {                                                                                 \
            /* Retry without RPA option (some peers / stacks reject it). */                        \
            dir_param = *BT_LE_ADV_CONN_DIR_LOW_DUTY(addr);                                        \
            err = bt_le_adv_start(&dir_param, zmk_ble_ad, ARRAY_SIZE(zmk_ble_ad), NULL, 0);        \
        }                                                                                          \
        if (err) {                                                                                 \
            char addr_str[BT_ADDR_LE_STR_LEN];                                                     \
            bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));                                   \
            LOG_WRN("Directed advertising to %s failed (err %d)", addr_str, err);                  \
            break;                                                                                 \
        }                                                                                          \
        advertising_status = ZMK_ADV_DIR;                                                          \
        {                                                                                          \
            char addr_str[BT_ADDR_LE_STR_LEN];                                                     \
            bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));                                   \
            LOG_INF("Directed advertising to %s (profile %d)", addr_str, active_profile);          \
        }                                                                                          \
    } while (0)

int update_advertising(void);

#if IS_ENABLED(CONFIG_TOTEM_ADV_THROTTLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static bool adv_throttled = false;
static struct k_work_delayable adv_throttle_work;

/* Retry open advertising when start fails (often: background host still holds a
 * connection slot). Keeps inviting the *active* profile instead of going dark. */
static struct k_work_delayable open_adv_retry_work;
#define OPEN_ADV_RETRY_MS 400
#define OPEN_ADV_RETRY_MAX 25
static uint8_t open_adv_retry_count;

#if IS_ENABLED(CONFIG_TOTEM_DIR_THEN_OPEN)
/* After BT_SEL: briefly use directed ads to the active peer, then open undirected
 * boost. Speeds host discovery while exclusive-host still drops the other PC.
 * Directed phase is skipped for open/empty profiles (pairing). */
static bool totem_dir_phase_active;
static struct k_work_delayable totem_dir_end_work;
#if IS_ENABLED(CONFIG_TOTEM_ADV_BOOST)
static void totem_adv_boost_arm(void);
#endif

static void totem_dir_phase_arm(void) {
    if (zmk_ble_active_profile_is_open()) {
        totem_dir_phase_active = false;
        k_work_cancel_delayable(&totem_dir_end_work);
        return;
    }
    bt_addr_le_t *peer = zmk_ble_active_profile_addr();
    if (peer == NULL || !bt_addr_le_cmp(peer, BT_ADDR_LE_ANY)) {
        totem_dir_phase_active = false;
        k_work_cancel_delayable(&totem_dir_end_work);
        return;
    }
    totem_dir_phase_active = true;
    k_work_reschedule(&totem_dir_end_work, K_SECONDS(CONFIG_TOTEM_DIR_ADV_SEC));
    LOG_INF("Directed-then-open: dir phase %d s for profile %d", CONFIG_TOTEM_DIR_ADV_SEC,
            active_profile);
}

static void totem_dir_phase_clear(void) {
    totem_dir_phase_active = false;
    k_work_cancel_delayable(&totem_dir_end_work);
}

static void totem_dir_end_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (!totem_dir_phase_active) {
        return;
    }
    totem_dir_phase_active = false;
    if (zmk_ble_active_profile_is_connected() || adv_throttled) {
        return;
    }
    LOG_INF("Directed phase ended; open undirected advertising (boost)");
    if (advertising_status == ZMK_ADV_DIR || advertising_status == ZMK_ADV_CONN) {
        int e = bt_le_adv_stop();
        if (e && e != -EALREADY) {
            LOG_WRN("Stop directed adv failed (err %d)", e);
        }
        advertising_status = ZMK_ADV_NONE;
    }
#if IS_ENABLED(CONFIG_TOTEM_ADV_BOOST)
    totem_adv_boost_arm();
#endif
    update_advertising();
}
#endif /* CONFIG_TOTEM_DIR_THEN_OPEN */

static void open_adv_retry_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (zmk_ble_active_profile_is_connected() || adv_throttled) {
        open_adv_retry_count = 0;
        return;
    }
    if (open_adv_retry_count >= OPEN_ADV_RETRY_MAX) {
        LOG_WRN("Open advertising retry limit reached; giving up until next event");
        open_adv_retry_count = 0;
        return;
    }
    open_adv_retry_count++;
    LOG_INF("Open advertising retry %u/%u", open_adv_retry_count, OPEN_ADV_RETRY_MAX);
    if (update_advertising() != 0 && !zmk_ble_active_profile_is_connected()) {
        k_work_schedule(&open_adv_retry_work, K_MSEC(OPEN_ADV_RETRY_MS));
    } else if (advertising_status == ZMK_ADV_CONN) {
        open_adv_retry_count = 0;
    } else if (!zmk_ble_active_profile_is_connected()) {
        k_work_schedule(&open_adv_retry_work, K_MSEC(OPEN_ADV_RETRY_MS));
    }
}

static void open_adv_retry_arm(void) {
    if (adv_throttled || zmk_ble_active_profile_is_connected()) {
        return;
    }
    k_work_schedule(&open_adv_retry_work, K_MSEC(OPEN_ADV_RETRY_MS));
}
#endif

static void totem_fal_clear_quiet(void) {
#if IS_ENABLED(CONFIG_BT_FILTER_ACCEPT_LIST)
    (void)bt_le_filter_accept_list_clear();
#endif
}

/* When the active profile is bonded, only that host may complete a connection
 * (Filter Accept List + BT_LE_ADV_OPT_FILTER_CONN). Open/empty profiles use
 * unfiltered ads for pairing. Background bonded hosts cannot thrash the link
 * while another profile is selected — primary multi-host isolation fix.
 *
 * Returns true only when FAL is armed and filtered advertising should be used.
 * Fail-open: any setup error → false (caller uses unfiltered open ads). */
static bool totem_prepare_active_fal(void) {
#if IS_ENABLED(CONFIG_TOTEM_ACTIVE_ADV_FILTER) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) &&     \
    IS_ENABLED(CONFIG_BT_FILTER_ACCEPT_LIST)
    if (zmk_ble_active_profile_is_open()) {
        LOG_DBG("FAL skip: active profile open (pairing)");
        totem_fal_clear_quiet();
        return false;
    }
    bt_addr_le_t *peer = zmk_ble_active_profile_addr();
    if (peer == NULL || !bt_addr_le_cmp(peer, BT_ADDR_LE_ANY)) {
        LOG_DBG("FAL skip: no bonded peer on active profile");
        totem_fal_clear_quiet();
        return false;
    }

    int err = bt_le_filter_accept_list_clear();
    if (err && err != -EALREADY) {
        LOG_WRN("FAL clear failed (err %d); advertising unfiltered", err);
        return false;
    }

    err = bt_le_filter_accept_list_add(peer);
    /* Already present is OK (some stacks return -EEXIST / -EALREADY). */
    if (err && err != -EEXIST && err != -EALREADY) {
        char addr[BT_ADDR_LE_STR_LEN];
        bt_addr_le_to_str(peer, addr, sizeof(addr));
        LOG_WRN("FAL add %s failed (err %d); advertising unfiltered", addr, err);
        totem_fal_clear_quiet();
        return false;
    }

    {
        char addr[BT_ADDR_LE_STR_LEN];
        bt_addr_le_to_str(peer, addr, sizeof(addr));
        LOG_INF("FAL active host only: %s (profile %d)", addr, active_profile);
    }
    return true;
#else
    return false;
#endif
}

#define CHECKED_OPEN_ADV()                                                                         \
    do {                                                                                           \
        bool use_fal = totem_prepare_active_fal();                                                 \
        bool fal_attempted = use_fal;                                                              \
        /* Pass compound literals directly into bt_le_adv_start (lifetime = full call). */         \
        if (use_fal) {                                                                             \
            err = bt_le_adv_start(totem_adv_boost_active ? ZMK_ADV_CONN_NAME_BOOST_FILTER          \
                                                         : ZMK_ADV_CONN_NAME_FILTER,               \
                                  zmk_ble_ad, ARRAY_SIZE(zmk_ble_ad), NULL, 0);                    \
            if (err && err != -EALREADY) {                                                         \
                LOG_WRN("Filtered advertising failed (err %d); falling back to open", err);        \
                use_fal = false;                                                                   \
                totem_fal_clear_quiet();                                                           \
            } else {                                                                               \
                LOG_DBG("Advertising started (FAL filtered, boost=%d)",                            \
                        (int)totem_adv_boost_active);                                              \
            }                                                                                      \
        }                                                                                          \
        if (!use_fal) {                                                                            \
            err = bt_le_adv_start(totem_adv_boost_active ? ZMK_ADV_CONN_NAME_BOOST                 \
                                                         : ZMK_ADV_CONN_NAME,                      \
                                  zmk_ble_ad, ARRAY_SIZE(zmk_ble_ad), NULL, 0);                    \
            if (fal_attempted && (err == 0 || err == -EALREADY)) {                                 \
                LOG_WRN("Advertising open (unfiltered fallback after FAL)");                       \
            }                                                                                      \
        }                                                                                          \
        if (err == -EALREADY) {                                                                    \
            advertising_status = ZMK_ADV_CONN;                                                     \
            err = 0;                                                                               \
        } else if (err) {                                                                          \
            LOG_WRN("Advertising start failed (err %d); will retry", err);                         \
            advertising_status = ZMK_ADV_NONE;                                                     \
            err = 0;                                                                               \
        } else {                                                                                   \
            advertising_status = ZMK_ADV_CONN;                                                     \
        }                                                                                          \
    } while (0)

#if IS_ENABLED(CONFIG_TOTEM_ADV_THROTTLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#if IS_ENABLED(CONFIG_TOTEM_ADV_BOOST)
static struct k_work_delayable adv_boost_end_work;

static void totem_adv_boost_arm(void) {
    totem_adv_boost_active = true;
    k_work_reschedule(&adv_boost_end_work, K_SECONDS(CONFIG_TOTEM_ADV_BOOST_SEC));
}

static void adv_boost_end_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (!totem_adv_boost_active) {
        return;
    }
    totem_adv_boost_active = false;

    if (zmk_ble_active_profile_is_connected()) {
        return;
    }

#if IS_ENABLED(CONFIG_TOTEM_ADV_POST_SWITCH_DARK)
    /* Selected host did not connect during the boost window. Go dark so
     * background bonded hosts cannot thrash open ads for minutes/hours
     * (that thrash can kill the selected host's bond). Keypress resumes. */
    LOG_INF("Post-switch boost ended; active host not up — advertising dark until keypress");
    if (advertising_status == ZMK_ADV_CONN || advertising_status == ZMK_ADV_DIR) {
        int err = bt_le_adv_stop();
        if (err && err != -EALREADY) {
            LOG_WRN("Failed to stop advertising for post-switch dark (err %d)", err);
        }
        advertising_status = ZMK_ADV_NONE;
    }
    adv_throttled = true;
    open_adv_retry_count = 0;
    k_work_cancel_delayable(&open_adv_retry_work);
    return;
#else
    if (advertising_status == ZMK_ADV_CONN) {
        LOG_INF("Advertising boost ended; returning to normal interval");
        int err = bt_le_adv_stop();
        if (err) {
            LOG_ERR("Failed to stop advertising after boost (err %d)", err);
            return;
        }
        advertising_status = ZMK_ADV_NONE;
        update_advertising();
    }
#endif /* CONFIG_TOTEM_ADV_POST_SWITCH_DARK */
}
#endif /* CONFIG_TOTEM_ADV_BOOST */

/* Totem dual-host helpers (config modules: reconnect_watch / exclusive_host).
 * Never clear adv_throttled / idle_go_dark — that would resurrect overnight ads. */

bool zmk_ble_totem_ads_suppressed(void) { return adv_throttled; }

/* Shared: densify or start open ads without clearing throttle/go-dark. */
static void totem_restart_open_adv_if_running(void) {
    if (adv_throttled) {
        return;
    }
    if (zmk_ble_active_profile_is_connected()) {
        return;
    }
    if (advertising_status == ZMK_ADV_CONN || advertising_status == ZMK_ADV_DIR) {
        int err = bt_le_adv_stop();
        if (err && err != -EALREADY) {
            LOG_WRN("totem boost/kick: adv_stop err %d", err);
        }
        advertising_status = ZMK_ADV_NONE;
        /* Brief ms-class gap only — NOT multi-second EVICT_ADV_COOLDOWN */
    }
    update_advertising();
}

void zmk_ble_totem_adv_boost_rearm(void) {
#if IS_ENABLED(CONFIG_TOTEM_ADV_BOOST)
    if (adv_throttled) {
        return;
    }
    totem_adv_boost_arm();
    totem_restart_open_adv_if_running();
#else
    /* Boost disabled: still kick open ads if dark (without densify restart). */
    if (!adv_throttled && !zmk_ble_active_profile_is_connected() &&
        advertising_status != ZMK_ADV_CONN && advertising_status != ZMK_ADV_DIR) {
        update_advertising();
    }
#endif
}

void zmk_ble_totem_kick_open_adv(void) {
    if (adv_throttled) {
        return;
    }
    if (zmk_ble_active_profile_is_connected()) {
        return;
    }
    if (advertising_status != ZMK_ADV_CONN && advertising_status != ZMK_ADV_DIR) {
        update_advertising();
    } else {
        open_adv_retry_arm();
    }
}

/* Fires once the selected host has been gone for the timeout: stop advertising to
 * save power. A key press resumes it (see the listener below). */
static void adv_throttle_work_handler(struct k_work *work) {
    if (advertising_status == ZMK_ADV_CONN && !zmk_ble_active_profile_is_connected()) {
        LOG_INF("Advertising idle timeout; pausing advertising until a key is pressed");
        int err = bt_le_adv_stop();
        if (err) {
            LOG_ERR("Failed to pause advertising (err %d)", err);
            return;
        }
        advertising_status = ZMK_ADV_NONE;
        adv_throttled = true;
    }
}

#if (CONFIG_TOTEM_EVICT_ADV_COOLDOWN_MS > 0)
/* Delayed re-advertise after a background host leaves while the selected host
 * is still away -- stops exclusive-host thrash from monopolizing the radio. */
static void evict_adv_cooldown_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (zmk_ble_active_profile_is_connected()) {
        return;
    }
    LOG_INF("Evict adv cooldown ended; updating advertising");
    update_advertising();
}

static K_WORK_DELAYABLE_DEFINE(evict_adv_cooldown_work, evict_adv_cooldown_work_handler);
#endif /* CONFIG_TOTEM_EVICT_ADV_COOLDOWN_MS */
#endif

#if IS_ENABLED(CONFIG_TOTEM_IDLE_DISCONNECT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static struct k_work_delayable idle_disconnect_work;
/* Set when the idle timer force-disconnects the host, so the update_advertising()
 * that runs on that disconnect goes dark immediately instead of re-advertising.
 * Otherwise the host (especially a plugged-in / light-sleep Mac) reconnects within a
 * second and wakes the display, and it repeats every timeout. Cleared as it fires. */
static bool idle_go_dark = false;

/* After CONFIG_TOTEM_IDLE_DISCONNECT_MIN minutes with no keypress, drop the active
 * host so advertising can pause. A present host reconnects on the next keypress
 * (which resumes advertising); an asleep/away host simply stays gone. */
static void idle_disconnect_work_handler(struct k_work *work) {
    if (!zmk_ble_active_profile_is_connected()) {
        return;
    }
    struct bt_conn *conn = zmk_ble_active_profile_conn();
    if (conn == NULL) {
        return;
    }
    LOG_INF("Active host idle for %d min; disconnecting and going dark",
            CONFIG_TOTEM_IDLE_DISCONNECT_MIN);
    /* Mark go-dark *and* throttled before disconnect so any concurrent
     * update_advertising path stays dark even if idle_go_dark is missed. */
    idle_go_dark = true;
    adv_throttled = true;
    k_work_cancel_delayable(&adv_throttle_work);
    /* 0x13 (remote user terminated) -- proven to let macOS reconnect and type
     * cleanly (see the exclusive-host module). */
    int err = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    if (err) {
        LOG_ERR("Idle disconnect failed (err %d); clearing go-dark", err);
        idle_go_dark = false;
        adv_throttled = false;
    }
    bt_conn_unref(conn);
}

/* Deferred so disconnected() sees an updated conn table (same reason ZMK defers
 * update_advertising). Cancels the idle timer only when the active host is gone;
 * a background host leaving must not clear the countdown. */
static void idle_disconnect_sync_work_handler(struct k_work *work) {
    /* Active host still up (e.g. exclusive-host just dropped the other PC):
     * leave the countdown alone. Only cancel when the selected host is gone. */
    if (zmk_ble_active_profile_is_connected()) {
        return;
    }
    k_work_cancel_delayable(&idle_disconnect_work);
}

static K_WORK_DEFINE(idle_disconnect_sync_work, idle_disconnect_sync_work_handler);
#endif

int update_advertising(void) {
    int err = 0;
    bt_addr_le_t *addr;
    struct bt_conn *conn;
    enum advertising_type desired_adv = ZMK_ADV_NONE;

    if (zmk_ble_active_profile_is_open()) {
        desired_adv = ZMK_ADV_CONN;
#if IS_ENABLED(CONFIG_TOTEM_DIR_THEN_OPEN) && IS_ENABLED(CONFIG_TOTEM_ADV_THROTTLE) &&             \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
        totem_dir_phase_clear();
#endif
    } else if (!zmk_ble_active_profile_is_connected()) {
#if IS_ENABLED(CONFIG_TOTEM_DIR_THEN_OPEN) && IS_ENABLED(CONFIG_TOTEM_ADV_THROTTLE) &&             \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
        /* Bonded active host: directed first (if phase armed), else open undirected. */
        if (totem_dir_phase_active) {
            desired_adv = ZMK_ADV_DIR;
        } else {
            desired_adv = ZMK_ADV_CONN;
        }
#else
        desired_adv = ZMK_ADV_CONN;
#endif
    }
    LOG_DBG("advertising from %d to %d", advertising_status, desired_adv);

#if IS_ENABLED(CONFIG_TOTEM_ADV_THROTTLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    if (desired_adv == ZMK_ADV_CONN || desired_adv == ZMK_ADV_DIR) {
#if IS_ENABLED(CONFIG_TOTEM_IDLE_DISCONNECT)
        if (idle_go_dark) {
            /* The idle timer just force-disconnected the host: pause instead of
             * (re-)advertising, so the host can't reconnect and wake. No advertising is
             * ever started, so there's no window for the host to grab. */
            idle_go_dark = false;
            adv_throttled = true;
            k_work_cancel_delayable(&adv_throttle_work);
        }
#endif
        if (adv_throttled) {
            /* We deliberately paused advertising (idle throttle, or idle-disconnect
             * go-dark) and stay dark until a real key press clears adv_throttled. Return
             * here so nothing -- a background update_advertising(), or a phantom
             * key-release from a split-link reconnect (release_peripheral_slot) -- can
             * resurrect advertising and let the host reconnect + wake. */
            return 0;
        }
    }
#endif

    switch (desired_adv + CURR_ADV(advertising_status)) {
    case ZMK_ADV_NONE + CURR_ADV(ZMK_ADV_DIR):
    case ZMK_ADV_NONE + CURR_ADV(ZMK_ADV_CONN):
        CHECKED_ADV_STOP();
        break;
    case ZMK_ADV_DIR + CURR_ADV(ZMK_ADV_DIR):
    case ZMK_ADV_DIR + CURR_ADV(ZMK_ADV_CONN):
        CHECKED_ADV_STOP();
        CHECKED_DIR_ADV();
        if (err) {
            /* Directed failed — fall back to open undirected immediately. */
#if IS_ENABLED(CONFIG_TOTEM_DIR_THEN_OPEN) && IS_ENABLED(CONFIG_TOTEM_ADV_THROTTLE) &&             \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
            totem_dir_phase_clear();
#endif
            CHECKED_OPEN_ADV();
        }
        break;
    case ZMK_ADV_DIR + CURR_ADV(ZMK_ADV_NONE):
        CHECKED_DIR_ADV();
        if (err) {
#if IS_ENABLED(CONFIG_TOTEM_DIR_THEN_OPEN) && IS_ENABLED(CONFIG_TOTEM_ADV_THROTTLE) &&             \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
            totem_dir_phase_clear();
#endif
            CHECKED_OPEN_ADV();
        }
        break;
    case ZMK_ADV_CONN + CURR_ADV(ZMK_ADV_DIR):
        CHECKED_ADV_STOP();
        CHECKED_OPEN_ADV();
        break;
    case ZMK_ADV_CONN + CURR_ADV(ZMK_ADV_NONE):
        CHECKED_OPEN_ADV();
        break;
    }

#if IS_ENABLED(CONFIG_TOTEM_ADV_THROTTLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    if (advertising_status == ZMK_ADV_CONN) {
        /* Advertising with the selected host not connected -> arm the idle timer.
         * k_work_schedule (NOT reschedule) so a nearby other device's connect/
         * disconnect churn does not keep resetting it. */
        k_work_schedule(&adv_throttle_work, K_MINUTES(CONFIG_TOTEM_ADV_THROTTLE_TIMEOUT_MIN));
        open_adv_retry_count = 0;
        k_work_cancel_delayable(&open_adv_retry_work);
    } else {
        k_work_cancel_delayable(&adv_throttle_work);
        adv_throttled = false;
        /* Want open ads for the active host but are not advertising yet (e.g.
         * background host still connected / stack rejected start). Keep trying. */
        if (desired_adv == ZMK_ADV_CONN && !adv_throttled &&
            !zmk_ble_active_profile_is_connected()) {
            open_adv_retry_arm();
        }
    }
#endif

    return 0;
};

static void update_advertising_callback(struct k_work *work) { update_advertising(); }

K_WORK_DEFINE(update_advertising_work, update_advertising_callback);

#if IS_ENABLED(CONFIG_TOTEM_ADV_THROTTLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
/* A key press on either half (the central sees right-half presses over the split
 * link) resumes advertising after the idle throttle paused it. First press or two
 * may be lost while the host reconnects. */
static int adv_throttle_keypress_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    /* Only a real key PRESS counts as user presence. Ignore releases -- in particular
     * the pressed=false events a split-link reconnect raises for positions it had
     * tracked (release_peripheral_slot in split/bluetooth/central.c) -- so they can't
     * wake a throttled/dark host. */
    if (ev == NULL || !ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }
#if IS_ENABLED(CONFIG_TOTEM_IDLE_DISCONNECT)
    /* Key press = user activity: restart the idle countdown. (Connect also arms
     * the timer; this path resets it while typing.) */
    k_work_reschedule(&idle_disconnect_work, K_MINUTES(CONFIG_TOTEM_IDLE_DISCONNECT_MIN));
#endif
    if (adv_throttled) {
        adv_throttled = false;
        LOG_INF("Key pressed; resuming advertising");
#if (CONFIG_TOTEM_EVICT_ADV_COOLDOWN_MS > 0)
        k_work_cancel_delayable(&evict_adv_cooldown_work);
#endif
#if IS_ENABLED(CONFIG_TOTEM_ADV_BOOST)
        totem_adv_boost_arm();
#endif
        update_advertising();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(totem_adv_throttle, adv_throttle_keypress_listener);
ZMK_SUBSCRIPTION(totem_adv_throttle, zmk_position_state_changed);

/* Profile switch is an intentional host change: leave go-dark/throttle and
 * advertise for the newly selected profile immediately. Without this, a switch
 * while dark (or a race that left adv_throttled set) waits for another keypress
 * before the target host can see the keyboard. */
static int adv_throttle_profile_changed_listener(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
#if IS_ENABLED(CONFIG_TOTEM_IDLE_DISCONNECT)
    idle_go_dark = false;
#endif
    adv_throttled = false;
#if (CONFIG_TOTEM_EVICT_ADV_COOLDOWN_MS > 0)
    /* Intentional host change: do not wait out a background-evict cooldown. */
    k_work_cancel_delayable(&evict_adv_cooldown_work);
#endif
#if IS_ENABLED(CONFIG_TOTEM_DIR_THEN_OPEN)
    /* Directed invite for the newly selected bonded host, then open undirected. */
    totem_dir_phase_arm();
#endif
#if IS_ENABLED(CONFIG_TOTEM_ADV_BOOST)
    /* Dense open advertising after directed phase (or immediately if no dir). */
    totem_adv_boost_arm();
#endif
    /* Restart advertising even if already open, so boost/dir take effect after
     * exclusive-host drops the previous peer. */
    if (advertising_status == ZMK_ADV_CONN || advertising_status == ZMK_ADV_DIR) {
        int err = bt_le_adv_stop();
        if (err) {
            LOG_WRN("Failed to stop advertising on profile change (err %d)", err);
        }
        advertising_status = ZMK_ADV_NONE;
    }
    LOG_INF("Profile changed; advertising for active profile%s",
#if IS_ENABLED(CONFIG_TOTEM_DIR_THEN_OPEN)
            " (dir-then-open)"
#else
            ""
#endif
#if IS_ENABLED(CONFIG_TOTEM_ADV_BOOST)
            " (boost)"
#else
            ""
#endif
    );
    update_advertising();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(totem_adv_throttle_profile, adv_throttle_profile_changed_listener);
ZMK_SUBSCRIPTION(totem_adv_throttle_profile, zmk_ble_active_profile_changed);
#endif

static void clear_profile_bond(uint8_t profile) {
    if (bt_addr_le_cmp(&profiles[profile].peer, BT_ADDR_LE_ANY)) {
        bt_unpair(BT_ID_DEFAULT, &profiles[profile].peer);
        set_profile_address(profile, BT_ADDR_LE_ANY);
    }
}

void zmk_ble_clear_bonds(void) {
    LOG_DBG("zmk_ble_clear_bonds()");

    clear_profile_bond(active_profile);
    update_advertising();
};

void zmk_ble_clear_all_bonds(void) {
    LOG_DBG("zmk_ble_clear_all_bonds()");

    // Unpair all profiles
    for (int i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
        clear_profile_bond(i);
    }

    // Automatically switch to profile 0
    zmk_ble_prof_select(0);
    update_advertising();
};

int zmk_ble_active_profile_index(void) { return active_profile; }

int zmk_ble_profile_index(const bt_addr_le_t *addr) {
    for (int i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
        if (bt_addr_le_cmp(addr, &profiles[i].peer) == 0) {
            return i;
        }
    }

    /* macOS privacy: host may connect with an RPA while the profile stores the
     * identity address. Do not use zephyr/bluetooth/keys.h (not public in our
     * Zephyr). Instead: if a live connection is reachable by the stored
     * identity and its current dst equals `addr`, it is this profile. */
    for (int i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
        if (!bt_addr_le_cmp(&profiles[i].peer, BT_ADDR_LE_ANY)) {
            continue;
        }
        struct bt_conn *conn = bt_conn_lookup_addr_le(BT_ID_DEFAULT, &profiles[i].peer);
        if (conn == NULL) {
            continue;
        }
        const bt_addr_le_t *dst = bt_conn_get_dst(conn);
        bool match = (dst != NULL && bt_addr_le_cmp(dst, addr) == 0);
        bt_conn_unref(conn);
        if (match) {
            return i;
        }
    }
    return -ENODEV;
}

bt_addr_le_t *zmk_ble_profile_address(uint8_t index) {
    if (index >= ZMK_BLE_PROFILE_COUNT) {
        return (bt_addr_le_t *)(BT_ADDR_LE_NONE);
    }
    return &profiles[index].peer;
}

#if IS_ENABLED(CONFIG_SETTINGS)
static void ble_save_profile_work(struct k_work *work) {
    settings_save_one("ble/active_profile", &active_profile, sizeof(active_profile));
}

static struct k_work_delayable ble_save_work;
#endif

static int ble_save_profile(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
    return k_work_reschedule(&ble_save_work, K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
#else
    return 0;
#endif
}

int zmk_ble_prof_select(uint8_t index) {
    if (index >= ZMK_BLE_PROFILE_COUNT) {
        return -ERANGE;
    }

    LOG_DBG("profile %d", index);
    if (active_profile == index) {
#if IS_ENABLED(CONFIG_TOTEM_RESELECT_RECONNECT) && IS_ENABLED(CONFIG_TOTEM_ADV_THROTTLE) &&        \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
        /* Soft recovery: re-selecting the active profile forces disconnect +
         * re-advertise. Helps macOS half-dead "Connected but no typing" without
         * a full Forget + re-pair when the bond itself is still good. */
        LOG_INF("Re-select profile %d; forcing soft reconnect", index);
#if IS_ENABLED(CONFIG_TOTEM_IDLE_DISCONNECT)
        idle_go_dark = false;
#endif
        adv_throttled = false;
#if IS_ENABLED(CONFIG_TOTEM_ADV_BOOST)
        totem_adv_boost_arm();
#endif
        (void)zmk_ble_prof_disconnect(index);
        if (advertising_status == ZMK_ADV_CONN || advertising_status == ZMK_ADV_DIR) {
            int err = bt_le_adv_stop();
            if (err) {
                LOG_WRN("Failed to stop advertising on reselect (err %d)", err);
            }
            advertising_status = ZMK_ADV_NONE;
        }
        update_advertising();
#endif
        return 0;
    }

    active_profile = index;
    ble_save_profile();

#if IS_ENABLED(CONFIG_TOTEM_ADV_THROTTLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    /* Raise first: exclusive-host drops the previous computer, then the profile
     * listener arms advertising boost, then we (re)start advertising. Avoids
     * advertising for the new profile while the old host still holds a link. */
    raise_profile_changed_event();
    update_advertising();
#else
    update_advertising();
    raise_profile_changed_event();
#endif

    return 0;
};

int zmk_ble_prof_next(void) {
    LOG_DBG("");
    return zmk_ble_prof_select((active_profile + 1) % ZMK_BLE_PROFILE_COUNT);
};

int zmk_ble_prof_prev(void) {
    LOG_DBG("");
    return zmk_ble_prof_select((active_profile + ZMK_BLE_PROFILE_COUNT - 1) %
                               ZMK_BLE_PROFILE_COUNT);
};

int zmk_ble_prof_disconnect(uint8_t index) {
    if (index >= ZMK_BLE_PROFILE_COUNT)
        return -ERANGE;

    bt_addr_le_t *addr = &profiles[index].peer;
    struct bt_conn *conn;
    int result;

    if (!bt_addr_le_cmp(addr, BT_ADDR_LE_ANY)) {
        return -ENODEV;
    } else if ((conn = bt_conn_lookup_addr_le(BT_ID_DEFAULT, addr)) == NULL) {
        return -ENODEV;
    }

    result = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    LOG_DBG("Disconnected from profile %d: %d", index, result);

    bt_conn_unref(conn);
    return result;
}

bt_addr_le_t *zmk_ble_active_profile_addr(void) { return &profiles[active_profile].peer; }

static void active_profile_conn_foreach(struct bt_conn *conn, void *data) {
    struct bt_conn **out = data;
    struct bt_conn_info info;

    if (*out != NULL) {
        return;
    }
    if (bt_conn_get_info(conn, &info) != 0 || info.role != BT_CONN_ROLE_PERIPHERAL) {
        return;
    }
    if (info.state != BT_CONN_STATE_CONNECTED) {
        return;
    }
    if (zmk_ble_profile_index(bt_conn_get_dst(conn)) == active_profile) {
        *out = bt_conn_ref(conn);
    }
}

struct bt_conn *zmk_ble_active_profile_conn(void) {
    struct bt_conn *conn;
    bt_addr_le_t *addr = zmk_ble_active_profile_addr();

    if (!bt_addr_le_cmp(addr, BT_ADDR_LE_ANY)) {
        LOG_WRN("Not sending, no active address for current profile");
        return NULL;
    } else if ((conn = bt_conn_lookup_addr_le(BT_ID_DEFAULT, addr)) != NULL) {
        return conn;
    }

    /* Fallback: RPA / identity lag — find a live host conn that maps to the
     * active profile via zmk_ble_profile_index (IRK-aware). */
    conn = NULL;
    bt_conn_foreach(BT_CONN_TYPE_LE, active_profile_conn_foreach, &conn);
    if (conn == NULL) {
        LOG_WRN("Not sending, not connected to active profile");
    }
    return conn;
}

char *zmk_ble_active_profile_name(void) { return profiles[active_profile].name; }

int zmk_ble_set_device_name(char *name) {
    // Copy new name to advertising parameters
    int err = bt_set_name(name);
    LOG_DBG("New device name: %s", name);
    if (err) {
        LOG_ERR("Failed to set new device name (err %d)", err);
        return err;
    }
    if (advertising_status == ZMK_ADV_CONN) {
        // Stop current advertising so it can restart with new name
        err = bt_le_adv_stop();
        advertising_status = ZMK_ADV_NONE;
        if (err) {
            LOG_ERR("Failed to stop advertising (err %d)", err);
            return err;
        }
    }
    return update_advertising();
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

int zmk_ble_put_peripheral_addr(const bt_addr_le_t *addr) {
    for (int i = 0; i < ZMK_SPLIT_BLE_PERIPHERAL_COUNT; i++) {
        // If the address is recognized and already stored in settings, return
        // index and no additional action is necessary.
        if (bt_addr_le_cmp(&peripheral_addrs[i], addr) == 0) {
            LOG_DBG("Found existing peripheral address in slot %d", i);
            return i;
        } else {
            char addr_str[BT_ADDR_LE_STR_LEN];
            bt_addr_le_to_str(&peripheral_addrs[i], addr_str, sizeof(addr_str));
            LOG_DBG("peripheral slot %d occupied by %s", i, addr_str);
        }

        // If the peripheral address slot is open, store new peripheral in the
        // slot and return index. This compares against BT_ADDR_LE_ANY as that
        // is the zero value.
        if (bt_addr_le_cmp(&peripheral_addrs[i], BT_ADDR_LE_ANY) == 0) {
            char addr_str[BT_ADDR_LE_STR_LEN];
            bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
            LOG_DBG("Storing peripheral %s in slot %d", addr_str, i);
            bt_addr_le_copy(&peripheral_addrs[i], addr);

#if IS_ENABLED(CONFIG_SETTINGS)
            char setting_name[32];
            sprintf(setting_name, "ble/peripheral_addresses/%d", i);
            settings_save_one(setting_name, addr, sizeof(bt_addr_le_t));
#endif // IS_ENABLED(CONFIG_SETTINGS)
            return i;
        }
    }

    // The peripheral does not match a known peripheral and there is no
    // available slot.
    return -ENOMEM;
}

#endif /* IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) */

#if IS_ENABLED(CONFIG_SETTINGS)

static int ble_profiles_handle_set(const char *name, size_t len, settings_read_cb read_cb,
                                   void *cb_arg) {
    const char *next;

    LOG_DBG("Setting BLE value %s", name);

    if (settings_name_steq(name, "profiles", &next) && next) {
        char *endptr;
        uint8_t idx = strtoul(next, &endptr, 10);
        if (*endptr != '\0') {
            LOG_WRN("Invalid profile index: %s", next);
            return -EINVAL;
        }

        if (len != sizeof(struct zmk_ble_profile)) {
            LOG_ERR("Invalid profile size (got %d expected %d)", len,
                    sizeof(struct zmk_ble_profile));
            return -EINVAL;
        }

        if (idx >= ZMK_BLE_PROFILE_COUNT) {
            LOG_WRN("Profile address for index %d is larger than max of %d", idx,
                    ZMK_BLE_PROFILE_COUNT);
            return -EINVAL;
        }

        int err = read_cb(cb_arg, &profiles[idx], sizeof(struct zmk_ble_profile));
        if (err <= 0) {
            LOG_ERR("Failed to handle profile address from settings (err %d)", err);
            return err;
        }

        char addr_str[BT_ADDR_LE_STR_LEN];
        bt_addr_le_to_str(&profiles[idx].peer, addr_str, sizeof(addr_str));

        LOG_DBG("Loaded %s address for profile %d", addr_str, idx);
    } else if (settings_name_steq(name, "active_profile", &next) && !next) {
        if (len != sizeof(active_profile)) {
            return -EINVAL;
        }

        int err = read_cb(cb_arg, &active_profile, sizeof(active_profile));
        if (err <= 0) {
            LOG_ERR("Failed to handle active profile from settings (err %d)", err);
            return err;
        }
    }
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    else if (settings_name_steq(name, "peripheral_addresses", &next) && next) {
        if (len != sizeof(bt_addr_le_t)) {
            return -EINVAL;
        }

        int i = atoi(next);
        if (i < 0 || i >= ZMK_SPLIT_BLE_PERIPHERAL_COUNT) {
            LOG_ERR("Failed to store peripheral address in memory");
        } else {
            int err = read_cb(cb_arg, &peripheral_addrs[i], sizeof(bt_addr_le_t));
            if (err <= 0) {
                LOG_ERR("Failed to handle peripheral address from settings (err %d)", err);
                return err;
            }
        }
    }
#endif

    return 0;
};

static int zmk_ble_complete_startup(void);

static struct settings_handler profiles_handler = {
    .name = "ble", .h_set = ble_profiles_handle_set, .h_commit = zmk_ble_complete_startup};

#endif /* IS_ENABLED(CONFIG_SETTINGS) */

static bool is_conn_active_profile(const struct bt_conn *conn) {
    /* Prefer IRK-aware profile index over raw address equality so macOS RPAs
     * still count as the active profile once the bond can resolve them. */
    int idx = zmk_ble_profile_index(bt_conn_get_dst(conn));
    if (idx >= 0) {
        return idx == active_profile;
    }
    return bt_addr_le_cmp(bt_conn_get_dst(conn), &profiles[active_profile].peer) == 0;
}

static void connected(struct bt_conn *conn, uint8_t err) {
    char addr[BT_ADDR_LE_STR_LEN];
    struct bt_conn_info info;
    LOG_DBG("Connected thread: %p", k_current_get());

    bt_conn_get_info(conn, &info);

    if (info.role != BT_CONN_ROLE_PERIPHERAL) {
        LOG_DBG("SKIPPING FOR ROLE %d", info.role);
        return;
    }

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    advertising_status = ZMK_ADV_NONE;

    if (err) {
        LOG_WRN("Failed to connect to %s (%u)", addr, err);
        update_advertising();
        return;
    }

    LOG_DBG("Connected %s", addr);

#if IS_ENABLED(CONFIG_TOTEM_DIR_THEN_OPEN) && IS_ENABLED(CONFIG_TOTEM_ADV_THROTTLE) &&             \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    if (is_conn_active_profile(conn)) {
        totem_dir_phase_clear();
    }
#endif

    update_advertising();

    if (is_conn_active_profile(conn)) {
        LOG_DBG("Active profile connected");
        k_work_submit(&raise_profile_changed_event_work);
    }
#if IS_ENABLED(CONFIG_TOTEM_IDLE_DISCONNECT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    /* Arm idle-disconnect on host presence, not only on keypress. Covers the
     * "host auto-reconnected / sat idle overnight without typing" path. */
    if (zmk_ble_active_profile_is_connected()) {
        k_work_reschedule(&idle_disconnect_work, K_MINUTES(CONFIG_TOTEM_IDLE_DISCONNECT_MIN));
    }
#endif
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
    char addr[BT_ADDR_LE_STR_LEN];
    struct bt_conn_info info;

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_DBG("Disconnected from %s (reason 0x%02x)", addr, reason);

    bt_conn_get_info(conn, &info);

    if (info.role != BT_CONN_ROLE_PERIPHERAL) {
        LOG_DBG("SKIPPING FOR ROLE %d", info.role);
        return;
    }

    // We need to do this in a work callback, otherwise the advertising update will still see the
    // connection for a profile as active, and not start advertising yet.
#if IS_ENABLED(CONFIG_TOTEM_ADV_THROTTLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) &&          \
    (CONFIG_TOTEM_EVICT_ADV_COOLDOWN_MS > 0)
    /* Background host left (usually exclusive-host eviction) while the selected
     * host is not up: delay open advertising so the wrong machine cannot
     * reconnect in a tight loop and starve the target. Active-host disconnect
     * and profile switch still re-advertise immediately. */
    if (!is_conn_active_profile(conn) && !zmk_ble_active_profile_is_connected()) {
        LOG_INF("Non-active host left; delaying advertising %d ms",
                CONFIG_TOTEM_EVICT_ADV_COOLDOWN_MS);
        k_work_reschedule(&evict_adv_cooldown_work, K_MSEC(CONFIG_TOTEM_EVICT_ADV_COOLDOWN_MS));
    } else {
        k_work_cancel_delayable(&evict_adv_cooldown_work);
        k_work_submit(&update_advertising_work);
    }
#else
    k_work_submit(&update_advertising_work);
#endif

    if (is_conn_active_profile(conn)) {
        LOG_DBG("Active profile disconnected");
        k_work_submit(&raise_profile_changed_event_work);
    }
#if IS_ENABLED(CONFIG_TOTEM_IDLE_DISCONNECT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    /* Defer: active_profile_is_connected() may still see this conn as live. */
    k_work_submit(&idle_disconnect_sync_work);
#endif
}

static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err) {
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    if (!err) {
        LOG_DBG("Security changed: %s level %u", addr, level);
    } else {
        LOG_ERR("Security failed: %s level %u err %d", addr, level, err);
    }
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
                             uint16_t timeout) {
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_DBG("%s: interval %d latency %d timeout %d", addr, interval, latency, timeout);
}

static struct bt_conn_cb conn_callbacks = {
    .connected = connected,
    .disconnected = disconnected,
    .security_changed = security_changed,
    .le_param_updated = le_param_updated,
};

/*
static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey) {
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_DBG("Passkey for %s: %06u", addr, passkey);
}
*/

#if IS_ENABLED(CONFIG_ZMK_BLE_PASSKEY_ENTRY)

static void auth_passkey_entry(struct bt_conn *conn) {
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_DBG("Passkey entry requested for %s", addr);
    if (auth_passkey_entry_conn) {
        bt_conn_unref(auth_passkey_entry_conn);
    }

    passkey_entry_count = 0;
    auth_passkey_entry_conn = bt_conn_ref(conn);
}

#endif

static void auth_cancel(struct bt_conn *conn) {
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

#if IS_ENABLED(CONFIG_ZMK_BLE_PASSKEY_ENTRY)
    if (auth_passkey_entry_conn == conn) {
        bt_conn_unref(auth_passkey_entry_conn);
        auth_passkey_entry_conn = NULL;
        passkey_entry_count = 0;
    }
#endif

    LOG_DBG("Pairing cancelled: %s", addr);
}

static bool pairing_allowed_for_current_profile(struct bt_conn *conn) {
    return zmk_ble_active_profile_is_open() ||
           (IS_ENABLED(CONFIG_BT_SMP_ALLOW_UNAUTH_OVERWRITE) &&
            bt_addr_le_cmp(zmk_ble_active_profile_addr(), bt_conn_get_dst(conn)) == 0);
}

static enum bt_security_err auth_pairing_accept(struct bt_conn *conn,
                                                const struct bt_conn_pairing_feat *const feat) {
    struct bt_conn_info info;
    bt_conn_get_info(conn, &info);

    LOG_DBG("role %d, open? %s", info.role, zmk_ble_active_profile_is_open() ? "yes" : "no");
    if (info.role == BT_CONN_ROLE_PERIPHERAL && !pairing_allowed_for_current_profile(conn)) {
        LOG_WRN("Rejecting pairing request to taken profile %d", active_profile);
        return BT_SECURITY_ERR_PAIR_NOT_ALLOWED;
    }

    return BT_SECURITY_ERR_SUCCESS;
};

static void auth_pairing_complete(struct bt_conn *conn, bool bonded) {
    struct bt_conn_info info;
    char addr[BT_ADDR_LE_STR_LEN];
    const bt_addr_le_t *dst = bt_conn_get_dst(conn);

    bt_addr_le_to_str(dst, addr, sizeof(addr));
    bt_conn_get_info(conn, &info);

    if (info.role != BT_CONN_ROLE_PERIPHERAL) {
        LOG_DBG("SKIPPING FOR ROLE %d", info.role);
        return;
    }

    if (!pairing_allowed_for_current_profile(conn)) {
        LOG_ERR("Pairing completed but current profile is not open: %s", addr);
        bt_unpair(BT_ID_DEFAULT, dst);
        return;
    }

    set_profile_address(active_profile, dst);
    update_advertising();
};

static void auth_pairing_failed(struct bt_conn *conn, enum bt_security_err reason) {
#if IS_ENABLED(CONFIG_ZMK_BLE_PASSKEY_ENTRY)
    if (auth_passkey_entry_conn == conn) {
        bt_conn_unref(auth_passkey_entry_conn);
        auth_passkey_entry_conn = NULL;
        passkey_entry_count = 0;
    }
#else
    ARG_UNUSED(conn);
#endif

    LOG_WRN("Pairing failed (reason %d)", reason);
}

static struct bt_conn_auth_cb zmk_ble_auth_cb_display = {
    .pairing_accept = auth_pairing_accept,
// .passkey_display = auth_passkey_display,

#if IS_ENABLED(CONFIG_ZMK_BLE_PASSKEY_ENTRY)
    .passkey_entry = auth_passkey_entry,
#endif
    .cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb zmk_ble_auth_info_cb_display = {
    .pairing_complete = auth_pairing_complete,
    .pairing_failed = auth_pairing_failed,
};

static void zmk_ble_ready(int err) {
    LOG_DBG("ready? %d", err);
    if (err) {
        LOG_ERR("Bluetooth init failed (err %d)", err);
        return;
    }

    update_advertising();
}

static int zmk_ble_complete_startup(void) {

#if IS_ENABLED(CONFIG_ZMK_BLE_CLEAR_BONDS_ON_START)
    LOG_WRN("Clearing all existing BLE bond information from the keyboard");

    bt_unpair(BT_ID_DEFAULT, NULL);

    for (int i = 0; i < 8; i++) {
        char setting_name[15];
        sprintf(setting_name, "ble/profiles/%d", i);

        int err = settings_delete(setting_name);
        if (err) {
            LOG_ERR("Failed to delete setting: %d", err);
        }
    }

    // Hardcoding a reasonable hardcoded value of peripheral addresses
    // to clear so we properly clear a split central as well.
    for (int i = 0; i < 8; i++) {
        char setting_name[32];
        sprintf(setting_name, "ble/peripheral_addresses/%d", i);

        int err = settings_delete(setting_name);
        if (err) {
            LOG_ERR("Failed to delete setting: %d", err);
        }
    }

#endif // IS_ENABLED(CONFIG_ZMK_BLE_CLEAR_BONDS_ON_START)

    bt_conn_cb_register(&conn_callbacks);
    bt_conn_auth_cb_register(&zmk_ble_auth_cb_display);
    bt_conn_auth_info_cb_register(&zmk_ble_auth_info_cb_display);

    zmk_ble_ready(0);

    return 0;
}

static int zmk_ble_init(void) {
    int err = bt_enable(NULL);

    if (err < 0 && err != -EALREADY) {
        LOG_ERR("BLUETOOTH FAILED (%d)", err);
        return err;
    }

#if IS_ENABLED(CONFIG_TOTEM_ADV_THROTTLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    k_work_init_delayable(&adv_throttle_work, adv_throttle_work_handler);
    k_work_init_delayable(&open_adv_retry_work, open_adv_retry_work_handler);
#if IS_ENABLED(CONFIG_TOTEM_ADV_BOOST)
    k_work_init_delayable(&adv_boost_end_work, adv_boost_end_work_handler);
#endif
#if IS_ENABLED(CONFIG_TOTEM_DIR_THEN_OPEN)
    k_work_init_delayable(&totem_dir_end_work, totem_dir_end_work_handler);
#endif
#if IS_ENABLED(CONFIG_TOTEM_IDLE_DISCONNECT)
    k_work_init_delayable(&idle_disconnect_work, idle_disconnect_work_handler);
#endif
#endif

#if IS_ENABLED(CONFIG_SETTINGS)
    settings_register(&profiles_handler);
    k_work_init_delayable(&ble_save_work, ble_save_profile_work);
#else
    zmk_ble_complete_startup();
#endif

    return 0;
}

#if IS_ENABLED(CONFIG_ZMK_BLE_PASSKEY_ENTRY)

static bool zmk_ble_numeric_usage_to_value(const zmk_key_t key, const zmk_key_t one,
                                           const zmk_key_t zero, uint8_t *value) {
    if (key < one || key > zero) {
        return false;
    }

    *value = (key == zero) ? 0 : (key - one + 1);
    return true;
}

static bool zmk_ble_submit_passkey(void) {
    if (passkey_entry_count != PASSKEY_DIGITS) {
        LOG_WRN("Passkey incomplete: %d of %d digits; waiting for remaining digits",
                passkey_entry_count, PASSKEY_DIGITS);
        return false;
    }

    uint32_t passkey = 0;

    for (int i = 0; i < PASSKEY_DIGITS; i++) {
        passkey = (passkey * 10) + passkey_entries[i];
    }

    LOG_DBG("Final passkey: %06u", passkey);
    struct bt_conn *conn = auth_passkey_entry_conn;
    int err = bt_conn_auth_passkey_entry(conn, passkey);
    if (err) {
        LOG_ERR("Failed to submit passkey (err %d)", err);
        return false;
    }

    /* Pairing callbacks are normally asynchronous, but only release our
     * reference here if a callback did not already finish this entry. */
    if (auth_passkey_entry_conn == conn) {
        bt_conn_unref(auth_passkey_entry_conn);
        auth_passkey_entry_conn = NULL;
        passkey_entry_count = 0;
    }
    return true;
}

static int zmk_ble_handle_key_user(struct zmk_keycode_state_changed *event) {
    zmk_key_t key = event->keycode;

    LOG_DBG("key %d", key);

    if (!auth_passkey_entry_conn) {
        LOG_DBG("No connection for passkey entry");
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (event->state) {
        LOG_DBG("Key press, ignoring");
        return ZMK_EV_EVENT_HANDLED;
    }

    if (key == HID_USAGE_KEY_KEYBOARD_ESCAPE) {
        bt_conn_auth_cancel(auth_passkey_entry_conn);
        return ZMK_EV_EVENT_HANDLED;
    }

    if (key == HID_USAGE_KEY_KEYBOARD_RETURN || key == HID_USAGE_KEY_KEYBOARD_RETURN_ENTER) {
        zmk_ble_submit_passkey();
        return ZMK_EV_EVENT_HANDLED;
    }

    if (key == HID_USAGE_KEY_KEYBOARD_DELETE_BACKSPACE) {
        if (passkey_entry_count > 0) {
            passkey_entry_count--;
            LOG_DBG("Passkey digit erased; %d digits remain", passkey_entry_count);
        } else {
            LOG_DBG("Passkey is already empty");
        }
        return ZMK_EV_EVENT_HANDLED;
    }

    uint8_t val;
    if (!(zmk_ble_numeric_usage_to_value(key, HID_USAGE_KEY_KEYBOARD_1_AND_EXCLAMATION,
                                         HID_USAGE_KEY_KEYBOARD_0_AND_RIGHT_PARENTHESIS, &val) ||
          zmk_ble_numeric_usage_to_value(key, HID_USAGE_KEY_KEYPAD_1_AND_END,
                                         HID_USAGE_KEY_KEYPAD_0_AND_INSERT, &val))) {
        LOG_DBG("Key not a number, ignoring");
        return ZMK_EV_EVENT_HANDLED;
    }

    if (passkey_entry_count == PASSKEY_DIGITS) {
        LOG_DBG("Passkey already has six digits; use Backspace to correct it");
        return ZMK_EV_EVENT_HANDLED;
    }

    passkey_entries[passkey_entry_count++] = val;
    LOG_DBG("value entered: %d, digits collected so far: %d", val, passkey_entry_count);

    return ZMK_EV_EVENT_HANDLED;
}

static int zmk_ble_listener(const zmk_event_t *eh) {
    struct zmk_keycode_state_changed *kc_state;

    kc_state = as_zmk_keycode_state_changed(eh);

    if (kc_state != NULL) {
        return zmk_ble_handle_key_user(kc_state);
    }

    return 0;
}

ZMK_LISTENER(zmk_ble, zmk_ble_listener);
ZMK_SUBSCRIPTION(zmk_ble, zmk_keycode_state_changed);
#endif /* IS_ENABLED(CONFIG_ZMK_BLE_PASSKEY_ENTRY) */

SYS_INIT(zmk_ble_init, APPLICATION, CONFIG_ZMK_BLE_INIT_PRIORITY);

/* Totem helpers when throttle patch path is not compiled (peripheral half, or
 * TOTEM_ADV_THROTTLE=n). Real implementations live inside the throttle block. */
#if !(IS_ENABLED(CONFIG_TOTEM_ADV_THROTTLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
bool zmk_ble_totem_ads_suppressed(void) { return false; }
void zmk_ble_totem_adv_boost_rearm(void) {}
void zmk_ble_totem_kick_open_adv(void) {}
#endif
