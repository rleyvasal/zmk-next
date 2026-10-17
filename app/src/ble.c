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
#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_BLE_HOST_ADV_OFF_ON_USB)
#include <zmk/events/usb_conn_state_changed.h>
#endif
#include <zmk/keys.h>
#include <zmk/split/bluetooth/uuid.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>

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

static struct zmk_ble_profile profiles[ZMK_BLE_PROFILE_COUNT];
static uint8_t active_profile;

/* Advertising runs on the system work queue only. Profile and USB changes bump the target
 * version; a transaction whose target went stale stops and runs again for the new one.
 * Profile writers only hold this lock for the RAM update, never for HCI. */
static struct k_spinlock adv_target_lock;
static uint32_t adv_target_version;
static uint32_t advertising_target_version;
struct adv_target {
    uint32_t version;
    uint8_t profile;
    bt_addr_le_t peer;
};

static struct adv_target adv_target_snapshot(void) {
    k_spinlock_key_t key = k_spin_lock(&adv_target_lock);
    struct adv_target target = {
        .version = adv_target_version,
        .profile = active_profile,
        .peer = profiles[active_profile].peer,
    };
    k_spin_unlock(&adv_target_lock, key);
    return target;
}

static bool adv_target_is_current(const struct adv_target *target) {
    k_spinlock_key_t key = k_spin_lock(&adv_target_lock);
    bool current = target->version == adv_target_version;
    k_spin_unlock(&adv_target_lock, key);
#if IS_ENABLED(CONFIG_ZMK_BLE_HOST_ADV_OFF_ON_USB)
    current = current && !zmk_usb_is_powered();
#endif
    return current;
}

enum adv_request {
    ADV_LINK_COMPLETE,
    ADV_RESTART,
};
static atomic_t adv_requests;
static void update_advertising_callback(struct k_work *work);
static K_WORK_DEFINE(update_advertising_work, update_advertising_callback);

static bool advertising_worker(void) {
    return k_current_get() == k_work_queue_thread_get(&k_sys_work_q);
}

static void request_advertising(enum adv_request request) {
    atomic_set_bit(&adv_requests, request);
    k_work_submit(&update_advertising_work);
}

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

    k_spinlock_key_t key = k_spin_lock(&adv_target_lock);
    memcpy(&profiles[index].peer, addr, sizeof(bt_addr_le_t));
    adv_target_version++;
    k_spin_unlock(&adv_target_lock, key);
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

__weak void zmk_ble_advertising_observed(uint8_t stage, int err) {
    ARG_UNUSED(stage);
    ARG_UNUSED(err);
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

/* All callers run on the system work queue. A profile/USB change while HCI
 * sleeps invalidates the transaction; never keep advertising its old target. */
static int start_advertising(const struct adv_target *target, const struct bt_le_adv_param *param,
                             enum advertising_type type, uint8_t stage) {
    if (!adv_target_is_current(target)) {
        k_work_submit(&update_advertising_work);
        return -EAGAIN;
    }
    int err = bt_le_adv_start(param, zmk_ble_ad, ARRAY_SIZE(zmk_ble_ad), NULL, 0);
    zmk_ble_advertising_observed(stage, err);
    if (!err) {
        advertising_status = type;
        advertising_target_version = target->version;
        if (!adv_target_is_current(target)) {
            err = bt_le_adv_stop();
            zmk_ble_advertising_observed(ZMK_BLE_ADV_STOP, err);
            if (!err) {
                advertising_status = ZMK_ADV_NONE;
            }
            k_work_submit(&update_advertising_work);
            return err ? err : -EAGAIN;
        }
    }
    return err;
}

#define CHECKED_ADV_STOP()                                                                         \
    err = bt_le_adv_stop();                                                                        \
    zmk_ble_advertising_observed(ZMK_BLE_ADV_STOP, err);                                           \
    if (err) {                                                                                     \
        LOG_ERR("Failed to stop advertising (err %d)", err);                                       \
        return err;                                                                                \
    }                                                                                              \
    advertising_status = ZMK_ADV_NONE;

/* Directed advertising to the active bonded peer. Used after profile switch to
 * invite that host faster than undirected discovery (helps Windows especially).
 * Privacy centrals (macOS): include DIR_ADDR_RPA so TargetA can be the peer's
 * RPA. Failures fall through to open undirected (caller handles). */
#define CHECKED_DIR_ADV()                                                                          \
    do {                                                                                           \
        addr = &target.peer;                                                                       \
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
        err = start_advertising(&target, &dir_param, ZMK_ADV_DIR, ZMK_BLE_ADV_START_DIRECTED_RPA); \
        if (err && err != -EAGAIN) {                                                               \
            /* Retry without RPA option (some peers / stacks reject it). */                        \
            dir_param = *BT_LE_ADV_CONN_DIR_LOW_DUTY(addr);                                        \
            err = start_advertising(&target, &dir_param, ZMK_ADV_DIR, ZMK_BLE_ADV_START_DIRECTED); \
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

#define CHECKED_OPEN_ADV()                                                                         \
    do {                                                                                           \
        err = start_advertising(&target, ZMK_ADV_CONN_NAME, ZMK_ADV_CONN, ZMK_BLE_ADV_START_OPEN); \
        if (err == -EALREADY) {                                                                    \
            advertising_status = ZMK_ADV_CONN;                                                     \
            err = 0;                                                                               \
        } else if (err == -EAGAIN) {                                                               \
            /* The target changed; the queued run advertises for the new one. */                   \
            err = 0;                                                                               \
        } else if (err) {                                                                          \
            LOG_ERR("Advertising failed to start (err %d)", err);                                  \
            return err;                                                                            \
        }                                                                                          \
    } while (0)

int update_advertising(void) {
    if (!advertising_worker()) {
        k_work_submit(&update_advertising_work);
        return 0;
    }
    int err = 0;
    if (atomic_test_and_clear_bit(&adv_requests, ADV_LINK_COMPLETE)) {
        advertising_status = ZMK_ADV_NONE;
    }
    if (atomic_test_bit(&adv_requests, ADV_RESTART)) {
        CHECKED_ADV_STOP();
        atomic_clear_bit(&adv_requests, ADV_RESTART);
    }
    struct adv_target target = adv_target_snapshot();
    if (advertising_status != ZMK_ADV_NONE && advertising_target_version != target.version) {
        CHECKED_ADV_STOP();
    }
    bt_addr_le_t *addr;
    struct bt_conn *conn;
    enum advertising_type desired_adv = ZMK_ADV_NONE;

    if (zmk_ble_active_profile_is_open()) {
        desired_adv = ZMK_ADV_CONN;
    } else if (!zmk_ble_active_profile_is_connected()) {
        desired_adv = ZMK_ADV_CONN;
    }
#if IS_ENABLED(CONFIG_ZMK_BLE_HOST_ADV_OFF_ON_USB)
    /* Host advertising only; the split link between halves is not affected. */
    if (zmk_usb_is_powered()) {
        desired_adv = ZMK_ADV_NONE;
    }
#endif
    LOG_DBG("advertising from %d to %d", advertising_status, desired_adv);

    switch (desired_adv + CURR_ADV(advertising_status)) {
    case ZMK_ADV_NONE + CURR_ADV(ZMK_ADV_DIR):
    case ZMK_ADV_NONE + CURR_ADV(ZMK_ADV_CONN):
        CHECKED_ADV_STOP();
        break;
    case ZMK_ADV_DIR + CURR_ADV(ZMK_ADV_DIR):
    case ZMK_ADV_DIR + CURR_ADV(ZMK_ADV_CONN):
        CHECKED_ADV_STOP();
        CHECKED_DIR_ADV();
        if (err && err != -EAGAIN && advertising_status == ZMK_ADV_NONE) {
            /* Directed failed — fall back to open undirected immediately. */
            CHECKED_OPEN_ADV();
        }
        break;
    case ZMK_ADV_DIR + CURR_ADV(ZMK_ADV_NONE):
        CHECKED_DIR_ADV();
        if (err && err != -EAGAIN && advertising_status == ZMK_ADV_NONE) {
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

    return 0;
};

static void update_advertising_callback(struct k_work *work) {
    ARG_UNUSED(work);
    update_advertising();
}

#if IS_ENABLED(CONFIG_ZMK_BLE_HOST_ADV_OFF_ON_USB)
static int usb_host_adv_listener(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    k_spinlock_key_t key = k_spin_lock(&adv_target_lock);
    adv_target_version++;
    k_spin_unlock(&adv_target_lock, key);
    k_work_submit(&update_advertising_work);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zmk_ble_usb_host_adv, usb_host_adv_listener);
ZMK_SUBSCRIPTION(zmk_ble_usb_host_adv, zmk_usb_conn_state_changed);
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
        return 0;
    }

    k_spinlock_key_t key = k_spin_lock(&adv_target_lock);
    active_profile = index;
    adv_target_version++;
    k_spin_unlock(&adv_target_lock, key);
    ble_save_profile();

    update_advertising();
    raise_profile_changed_event();

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
        struct bt_conn_info info;
        bool connected = bt_conn_get_info(conn, &info) == 0 &&
                         info.role == BT_CONN_ROLE_PERIPHERAL &&
                         info.state == BT_CONN_STATE_CONNECTED;
        if (connected) {
            return conn;
        }
        bt_conn_unref(conn);
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
    request_advertising(ADV_RESTART);
    return 0;
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
    request_advertising(ADV_LINK_COMPLETE);

    if (err) {
        LOG_WRN("Failed to connect to %s (%u)", addr, err);
        update_advertising();
        return;
    }

    LOG_DBG("Connected %s", addr);

    update_advertising();

    if (is_conn_active_profile(conn)) {
        LOG_DBG("Active profile connected");
        k_work_submit(&raise_profile_changed_event_work);
    }
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
    k_work_submit(&update_advertising_work);

    if (is_conn_active_profile(conn)) {
        LOG_DBG("Active profile disconnected");
        k_work_submit(&raise_profile_changed_event_work);
    }
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

#if IS_ENABLED(CONFIG_ZMK_BLE_PASSKEY_ENTRY)
/* Drop a passkey entry in progress when pairing fails, so the next request starts empty. */
static void auth_pairing_failed(struct bt_conn *conn, enum bt_security_err reason) {
    if (auth_passkey_entry_conn == conn) {
        bt_conn_unref(auth_passkey_entry_conn);
        auth_passkey_entry_conn = NULL;
        passkey_entry_count = 0;
    }

    LOG_WRN("Pairing failed (reason %d)", reason);
}
#endif

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
#if IS_ENABLED(CONFIG_ZMK_BLE_PASSKEY_ENTRY)
    .pairing_failed = auth_pairing_failed,
#endif
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
