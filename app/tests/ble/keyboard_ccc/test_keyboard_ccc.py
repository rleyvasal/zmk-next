"""Keep the diagnostic CCC hook visible to Zephyr's managed-CCC machinery.

Run with ZEPHYR_BASE pointing at the pinned Zephyr checkout.
"""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <string.h>
#define ARG_UNUSED(x) ((void)(x))
#define LOG_DBG(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define BT_GATT_CCC_NOTIFY 1
#define BT_GATT_PERM_READ_ENCRYPT 2
#define BT_GATT_PERM_WRITE_ENCRYPT 4
#define BT_UUID_GATT_CCC 1
#define BT_ATT_ERR_INVALID_OFFSET 7
#define BT_ATT_ERR_INVALID_ATTRIBUTE_LEN 13
#define BT_ATT_ERR_INSUFFICIENT_RESOURCES 17
#define BT_ATT_ERR_UNLIKELY 14
#define BT_GATT_ERR(x) (-(x))
#define CONFIG_BT_SETTINGS_CCC_STORE_ON_WRITE 1
#define DELAYED_STORE_CCC 1
struct bt_conn { uint8_t id; struct { int dst; } le; };
struct bt_gatt_attr;
typedef ssize_t (*write_t)(struct bt_conn *,const struct bt_gatt_attr *,const void *,
                          uint16_t,uint16_t,uint8_t);
struct bt_gatt_attr { write_t write; void *user_data; uint16_t perm,handle; };
struct bt_gatt_ccc_cfg { uint8_t id; int peer; uint16_t value; };
struct _bt_gatt_ccc {
    struct bt_gatt_ccc_cfg cfg[1]; uint16_t value;
    void (*cfg_changed)(const struct bt_gatt_attr *,uint16_t);
    ssize_t (*cfg_write)(struct bt_conn *,const struct bt_gatt_attr *,uint16_t);
    bool (*cfg_match)(struct bt_conn *,const struct bt_gatt_attr *);
};
#define BT_GATT_ATTRIBUTE(uuid,perm,read,write,data) {write,data,perm,1}
/* ACTUAL_MACROS */
static bool existing=true, space=true, host_requests_notification;
static unsigned observed, stores, committed;
static void zmk_hog_subscription_observed(struct bt_conn *conn) {
    assert(conn && conn->le.dst==2); observed++;
}
static uint16_t sys_get_le16(const void *buf) {
    const uint8_t *p=buf; return p[0]|((uint16_t)p[1]<<8);
}
static void bt_addr_le_copy(int *dest,const int *src) { *dest=*src; }
static struct bt_gatt_ccc_cfg *find_ccc_cfg(struct bt_conn *conn,struct _bt_gatt_ccc *ccc) {
    return (conn ? existing : space) ? &ccc->cfg[0] : NULL;
}
static void clear_ccc_cfg(struct bt_gatt_ccc_cfg *cfg) { memset(cfg,0,sizeof(*cfg)); }
static void gatt_ccc_changed(const struct bt_gatt_attr *attr,struct _bt_gatt_ccc *ccc) {
    ccc->value=ccc->cfg[0].value; ccc->cfg_changed(attr,ccc->value);
}
static void gatt_delayed_store_enqueue(uint8_t id,const int *peer,int kind) {
    assert(id==0 && *peer==2 && kind==1); stores++;
}
/* ACTUAL_STANDARD_WRITE */
/* ACTUAL_IDENTITY_CHECK */
/* ACTUAL_CALLBACKS */
static struct bt_gatt_attr keyboard[]={ /* ACTUAL_BINDING */ };
static void settings_store(void) {
    /* Same eligibility gate used by Zephyr's CCC save/restore/identity paths. */
    assert(is_host_managed_ccc(&keyboard[0])); committed++;
}
int main(void) {
    struct bt_conn conn={0,{2}};
    struct bt_gatt_attr *attr=&keyboard[0];
    struct _bt_gatt_ccc *ccc=attr->user_data;
    assert(attr->write==bt_gatt_attr_write_ccc && is_host_managed_ccc(attr));
    assert(attr->perm==(BT_GATT_PERM_READ_ENCRYPT|BT_GATT_PERM_WRITE_ENCRYPT));
    uint8_t enable[]={1,0}, disable[]={0,0};
    assert(attr->write(&conn,attr,enable,2,0,0)==2);
    assert(observed==1 && stores==1 && ccc->cfg[0].value==1 && host_requests_notification);
    settings_store(); assert(committed==1);
    assert(attr->write(&conn,attr,disable,2,0,0)==2);
    assert(observed==1 && stores==2 && !ccc->cfg[0].value && !host_requests_notification);
    /* Legacy one-byte CCC writes keep their original return value. */
    existing=false;
    assert(attr->write(&conn,attr,enable,1,0,0)==1);
    assert(observed==2 && stores==3 && ccc->cfg[0].peer==2);
    unsigned before=observed;
    assert(attr->write(&conn,attr,enable,2,1,0)==-BT_ATT_ERR_INVALID_OFFSET);
    assert(attr->write(&conn,attr,enable,0,0,0)==-BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    assert(attr->write(&conn,attr,enable,3,0,0)==-BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    space=false;
    assert(attr->write(&conn,attr,enable,2,0,0)==-BT_ATT_ERR_INSUFFICIENT_RESOURCES);
    assert(observed==before && stores==3);
    assert(attr->write(&conn,attr,disable,2,0,0)==2 && observed==before);
    return 0;
}
"""


class KeyboardCccTests(unittest.TestCase):
    def test_standard_handler_identity_and_write_validation(self):
        app = Path(__file__).resolve().parents[3]
        hog = (app / "src/hog.c").read_text()
        zephyr = Path(os.environ["ZEPHYR_BASE"])
        gatt = (zephyr / "subsys/bluetooth/host/gatt.c").read_text()
        header = (zephyr / "include/zephyr/bluetooth/gatt.h").read_text()

        def section(source, start, end):
            offset = source.index(start)
            return source[offset:source.index(end, offset)]

        macros = "\n".join(section(header, "#define " + name, "\n\n") for name in (
            "BT_GATT_CCC_INITIALIZER", "BT_GATT_CCC_MANAGED"))
        changed = section(hog, "static void input_ccc_changed(", "\n__weak void")
        callback = section(hog, "static ssize_t keyboard_ccc_cfg_write(",
                           "\nstatic ssize_t write_ctrl_point(")
        binding = section(hog, "    BT_GATT_CCC_MANAGED(", "    BT_GATT_DESCRIPTOR(")
        standard = section(gatt, "ssize_t bt_gatt_attr_write_ccc(",
                           "\nssize_t bt_gatt_attr_read_cep(")
        identity = section(gatt, "static bool is_host_managed_ccc(", "\n#if defined(")
        harness = HARNESS.replace("/* ACTUAL_MACROS */", macros)
        harness = harness.replace("/* ACTUAL_CALLBACKS */", changed + callback)
        harness = harness.replace("/* ACTUAL_BINDING */", binding)
        harness = harness.replace("/* ACTUAL_STANDARD_WRITE */", standard)
        harness = harness.replace("/* ACTUAL_IDENTITY_CHECK */", identity)
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "test_keyboard_ccc")
            subprocess.run(
                [os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                 "-Wno-unused-parameter", "-x", "c", "-", "-o", executable],
                input=harness, text=True, check=True, timeout=30,
            )
            subprocess.run([executable], check=True, timeout=3)


if __name__ == "__main__":
    unittest.main()
