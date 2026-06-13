#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "ble_hid_host.hpp"
#include "shell.hpp"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"
#include "host/ble_sm.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_store.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#define TAG "BLE_HID"

static uint16_t conn_handle = 0xFFFF;
static bool scan_active = false;
static bool hid_configured = false;
static bool auto_reconnect = true;

static bool has_paired_prefix = false;
static uint8_t paired_prefix[4] = {0};
static uint8_t paired_slot = 0;
static bool has_paired_addr = false;
static ble_addr_t paired_addr = {};
static char paired_name[BLE_HID_NAME_MAX] = {0};
static char connected_name[BLE_HID_NAME_MAX] = {0};
static char pending_connect_name[BLE_HID_NAME_MAX] = {0};

static ble_hid_scan_result_t scan_results[BLE_HID_SCAN_MAX_RESULTS];
static int scan_result_count = 0;
static uint32_t scan_generation = 1;
static portMUX_TYPE scan_lock = portMUX_INITIALIZER_UNLOCKED;

static bool manual_scan_mode = false;
static bool hs_synced = false;
static bool pending_manual_scan = false;
static bool reset_scheduled = false;
static bool ble_runtime_disabled = false;
static uint8_t hci_timeout_resets = 0;
static uint8_t own_addr_type = BLE_OWN_ADDR_PUBLIC;
static uint32_t scan_evt_count = 0;
static uint32_t scan_candidate_count = 0;
static uint32_t scan_added_count = 0;
static uint32_t scan_updated_count = 0;

static int gap_event_cb(struct ble_gap_event *event, void *arg);
static void nimble_sync_cb(void);
static void nimble_reset_cb(int reason);
extern "C" void ble_store_config_init(void);

static const char *gap_event_type_to_text(uint8_t t) {
    switch (t) {
        case BLE_GAP_EVENT_CONNECT: return "connect";
        case BLE_GAP_EVENT_DISCONNECT: return "disconnect";
        case BLE_GAP_EVENT_CONN_UPDATE: return "conn_update";
        case BLE_GAP_EVENT_CONN_UPDATE_REQ: return "conn_update_req";
        case BLE_GAP_EVENT_L2CAP_UPDATE_REQ: return "l2cap_update_req";
        case BLE_GAP_EVENT_TERM_FAILURE: return "term_failure";
        case BLE_GAP_EVENT_DISC: return "disc";
        case BLE_GAP_EVENT_DISC_COMPLETE: return "disc_complete";
        case BLE_GAP_EVENT_ADV_COMPLETE: return "adv_complete";
        case BLE_GAP_EVENT_ENC_CHANGE: return "enc_change";
        case BLE_GAP_EVENT_PASSKEY_ACTION: return "passkey_action";
        case BLE_GAP_EVENT_NOTIFY_RX: return "notify_rx";
        case BLE_GAP_EVENT_NOTIFY_TX: return "notify_tx";
        case BLE_GAP_EVENT_SUBSCRIBE: return "subscribe";
        case BLE_GAP_EVENT_MTU: return "mtu";
        case BLE_GAP_EVENT_IDENTITY_RESOLVED: return "identity_resolved";
        case BLE_GAP_EVENT_REPEAT_PAIRING: return "repeat_pairing";
        case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE: return "phy_update";
        case BLE_GAP_EVENT_PARING_COMPLETE: return "pairing_complete";
        case BLE_GAP_EVENT_LINK_ESTAB: return "link_estab";
        default: return "other";
    }
}

static const char *passkey_action_to_text(uint8_t action) {
    switch (action) {
        case BLE_SM_IOACT_NONE: return "none";
        case BLE_SM_IOACT_OOB: return "oob";
        case BLE_SM_IOACT_INPUT: return "input";
        case BLE_SM_IOACT_DISP: return "display";
        case BLE_SM_IOACT_NUMCMP: return "numcmp";
        case BLE_SM_IOACT_OOB_SC: return "oob_sc";
#ifdef BLE_SM_IOACT_STATIC
        case BLE_SM_IOACT_STATIC: return "static";
#endif
        default: return "unknown";
    }
}

static void refresh_own_addr_type(void) {
    uint8_t inferred = BLE_OWN_ADDR_PUBLIC;
    int rc = ble_hs_id_infer_auto(0, &inferred);
    if (rc == 0) {
        own_addr_type = inferred;
    } else {
        own_addr_type = BLE_OWN_ADDR_PUBLIC;
        ESP_LOGW(TAG, "ble_hs_id_infer_auto failed: %d; using public addr type", rc);
    }
}

static uint16_t s_notify_val_handle = 0;
static uint16_t s_notify_cccd_handle = 0;
static uint32_t s_notify_rx_count = 0;
static uint32_t s_decode_ok_count = 0;
static uint32_t s_decode_drop_count = 0;
static bool s_security_requested = false;
static uint16_t s_debug_subscribed_vals[24] = {0};
static int s_debug_subscribed_count = 0;

enum {
    DISC_CTX_HID = 1,
    DISC_CTX_VENDOR = 2,
};

static const char *ble_hs_rc_to_text(int rc) {
    if (rc >= BLE_HS_ERR_HCI_BASE && rc < (BLE_HS_ERR_HCI_BASE + 0x100)) {
        switch (rc - BLE_HS_ERR_HCI_BASE) {
            case 0x08: return "hci-conn-timeout";
            case 0x13: return "hci-remote-user-terminated";
            case 0x16: return "hci-local-host-terminated";
            case 0x3B: return "hci-unacceptable-conn-params";
            case 0x3E: return "hci-conn-failed-establish";
            default: return "hci-status";
        }
    }

    if (rc >= BLE_HS_ERR_SM_US_BASE && rc < (BLE_HS_ERR_SM_US_BASE + BLE_SM_ERR_MAX_PLUS_1)) {
        switch (rc - BLE_HS_ERR_SM_US_BASE) {
            case BLE_SM_ERR_PASSKEY: return "sm-us-passkey";
            case BLE_SM_ERR_OOB: return "sm-us-oob";
            case BLE_SM_ERR_AUTHREQ: return "sm-us-authreq";
            case BLE_SM_ERR_CONFIRM_MISMATCH: return "sm-us-confirm-mismatch";
            case BLE_SM_ERR_PAIR_NOT_SUPP: return "sm-us-pair-not-supported";
            case BLE_SM_ERR_ENC_KEY_SZ: return "sm-us-key-size";
            case BLE_SM_ERR_CMD_NOT_SUPP: return "sm-us-cmd-not-supported";
            case BLE_SM_ERR_UNSPECIFIED: return "sm-us-unspecified";
            case BLE_SM_ERR_REPEATED: return "sm-us-repeated";
            case BLE_SM_ERR_INVAL: return "sm-us-invalid";
            case BLE_SM_ERR_DHKEY: return "sm-us-dhkey";
            case BLE_SM_ERR_NUMCMP: return "sm-us-numcmp";
            case BLE_SM_ERR_ALREADY: return "sm-us-already";
            case BLE_SM_ERR_CROSS_TRANS: return "sm-us-cross-transport";
            case BLE_SM_ERR_KEY_REJ: return "sm-us-key-rejected";
            default: return "sm-us-other";
        }
    }

    if (rc >= BLE_HS_ERR_SM_PEER_BASE && rc < (BLE_HS_ERR_SM_PEER_BASE + BLE_SM_ERR_MAX_PLUS_1)) {
        switch (rc - BLE_HS_ERR_SM_PEER_BASE) {
            case BLE_SM_ERR_PASSKEY: return "sm-peer-passkey";
            case BLE_SM_ERR_OOB: return "sm-peer-oob";
            case BLE_SM_ERR_AUTHREQ: return "sm-peer-authreq";
            case BLE_SM_ERR_CONFIRM_MISMATCH: return "sm-peer-confirm-mismatch";
            case BLE_SM_ERR_PAIR_NOT_SUPP: return "sm-peer-pair-not-supported";
            case BLE_SM_ERR_ENC_KEY_SZ: return "sm-peer-key-size";
            case BLE_SM_ERR_CMD_NOT_SUPP: return "sm-peer-cmd-not-supported";
            case BLE_SM_ERR_UNSPECIFIED: return "sm-peer-unspecified";
            case BLE_SM_ERR_REPEATED: return "sm-peer-repeated";
            case BLE_SM_ERR_INVAL: return "sm-peer-invalid";
            case BLE_SM_ERR_DHKEY: return "sm-peer-dhkey";
            case BLE_SM_ERR_NUMCMP: return "sm-peer-numcmp";
            case BLE_SM_ERR_ALREADY: return "sm-peer-already";
            case BLE_SM_ERR_CROSS_TRANS: return "sm-peer-cross-transport";
            case BLE_SM_ERR_KEY_REJ: return "sm-peer-key-rejected";
            default: return "sm-peer-other";
        }
    }

    switch (rc) {
        case 0: return "ok";
        case BLE_HS_EALREADY: return "already";
        case BLE_HS_EBUSY: return "busy";
        case BLE_HS_ECONTROLLER: return "controller";
        case BLE_HS_EDONE: return "done";
        case BLE_HS_EINVAL: return "invalid";
        case BLE_HS_ENOENT: return "not-found";
        case BLE_HS_ETIMEOUT: return "timeout";
        case BLE_HS_ETIMEOUT_HCI: return "hci-timeout";
        default: return "other";
    }
}

static bool subscribe_cccd(uint16_t conn, uint16_t cccd_handle, uint16_t cccd_bits, const char *reason) {
    uint8_t cccd_data[2] = {(uint8_t)(cccd_bits & 0xFF), (uint8_t)((cccd_bits >> 8) & 0xFF)};
    int rc = ble_gattc_write_flat(conn, cccd_handle, cccd_data, sizeof(cccd_data), NULL, NULL);
    if (rc == 0) {
        hid_configured = true;
        ESP_LOGI(TAG,
                 "subscribed cccd=0x%04X bits=0x%04X reason=%s",
                 cccd_handle,
                 cccd_bits,
                 reason ? reason : "unknown");
        return true;
    }
    ESP_LOGW(TAG,
             "cccd subscribe failed handle=0x%04X bits=0x%04X rc=%d(%s) reason=%s",
             cccd_handle,
             cccd_bits,
             rc,
             ble_hs_rc_to_text(rc),
             reason ? reason : "unknown");
    return false;
}

static void write_hid_char_u8(uint16_t conn, uint16_t val_handle, uint8_t value, const char *label) {
    int rc = ble_gattc_write_flat(conn, val_handle, &value, 1, NULL, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "%s write ok handle=0x%04X value=0x%02X", label, val_handle, value);
    } else {
        ESP_LOGW(TAG, "%s write failed handle=0x%04X rc=%d(%s)",
                 label,
                 val_handle,
                 rc,
                 ble_hs_rc_to_text(rc));
    }
}

static bool debug_val_already_subscribed(uint16_t val_handle) {
    for (int i = 0; i < s_debug_subscribed_count; i++) {
        if (s_debug_subscribed_vals[i] == val_handle) {
            return true;
        }
    }
    return false;
}

static void debug_mark_subscribed(uint16_t val_handle) {
    if (debug_val_already_subscribed(val_handle)) {
        return;
    }
    if (s_debug_subscribed_count < (int)(sizeof(s_debug_subscribed_vals) / sizeof(s_debug_subscribed_vals[0]))) {
        s_debug_subscribed_vals[s_debug_subscribed_count++] = val_handle;
    }
}

static int gatt_disc_dsc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                             uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg) {
    uint16_t cccd_bits = (uint16_t)(uintptr_t)arg;
    if (error && error->status != 0 && error->status != BLE_HS_EDONE) {
        ESP_LOGW(TAG, "descriptor discovery failed status=%d", error->status);
        return 0;
    }

    if (dsc != NULL) {
        if (dsc->uuid.u.type == BLE_UUID_TYPE_16) {
            ESP_LOGI(TAG,
                     "descriptor discovered chr=0x%04X dsc=0x%04X uuid16=0x%04X",
                     chr_val_handle,
                     dsc->handle,
                     dsc->uuid.u16.value);
        } else {
            ESP_LOGI(TAG,
                     "descriptor discovered chr=0x%04X dsc=0x%04X uuid_type=%u",
                     chr_val_handle,
                     dsc->handle,
                     (unsigned)dsc->uuid.u.type);
        }
        if (dsc->uuid.u.type == BLE_UUID_TYPE_16 &&
            dsc->uuid.u16.value == BLE_GATT_DSC_CLT_CFG_UUID16 &&
            chr_val_handle == s_notify_val_handle && dsc->handle > chr_val_handle) {
            if (s_notify_cccd_handle == 0 || dsc->handle < s_notify_cccd_handle) {
                s_notify_cccd_handle = dsc->handle;
            }
        }
        return 0;
    }

    if (s_notify_cccd_handle) {
        subscribe_cccd(conn_handle, s_notify_cccd_handle, cccd_bits, "descriptor-discovery");
    }
    s_notify_val_handle = 0;
    s_notify_cccd_handle = 0;
    return 0;
}

static void maybe_request_security(uint16_t handle, const char *reason) {
    if (handle == 0xFFFF || s_security_requested) {
        return;
    }

    struct ble_gap_conn_desc desc;
    int rc = ble_gap_conn_find(handle, &desc);
    if (rc != 0) {
        ESP_LOGW(TAG,
                 "security check skipped (%s): conn_find rc=%d(%s)",
                 reason ? reason : "unknown",
                 rc,
                 ble_hs_rc_to_text(rc));
        return;
    }

    if (desc.sec_state.encrypted) {
        ESP_LOGI(TAG,
                 "security already encrypted (%s) conn=%u auth=%u bonded=%u",
                 reason ? reason : "unknown",
                 (unsigned)handle,
                 desc.sec_state.authenticated,
                 desc.sec_state.bonded);
        return;
    }

    rc = ble_gap_security_initiate(handle);
    ESP_LOGI(TAG,
             "security initiate (%s) rc=%d(%s) conn=%u",
             reason ? reason : "unknown",
             rc,
             ble_hs_rc_to_text(rc),
             (unsigned)handle);
    if (rc == 0 || rc == BLE_HS_EALREADY || rc == BLE_HS_EBUSY) {
        s_security_requested = true;
    }
}

static QueueHandle_t msg_queue = NULL;
static lv_obj_t *status_label = NULL;

typedef struct {
    uint8_t type;  // 0 = status line, 1 = key character
    char text[256];
} disp_msg_t;

/*
 * IMPORTANT (HID usage alignment):
 * This table index is the HID usage code itself (0x00..0x38 for boot keys).
 * Do not remove or reorder placeholders. In particular, usage 0x32 is the
 * ISO "Non-US #/~" key and MUST stay as a placeholder for US layout here.
 *
 * If 0x32 is removed, all punctuation mappings from 0x33 onward shift by one:
 * - ';' becomes '\''
 * - '/' (0x38) becomes out-of-range/unmapped
 * This was the root cause of several wrong character keys.
 */
static const char hid_to_ascii[] = {
     0,  0,  0,  0, 'a','b','c','d','e','f','g','h','i','j','k','l',
    'm','n','o','p','q','r','s','t','u','v','w','x','y','z',
    '1','2','3','4','5','6','7','8','9','0',
    '\r',0x1B,'\b','\t',' ','-','=','[',']','\\',0,';','\'','`',',','.','/'
};

static const char shift_to[] = "!@#$%^&*()_+{}|:\"<>?~";
static uint8_t s_prev_keys[6] = {0};
static uint8_t s_prev_short_flags = 0;
static uint8_t s_repeat_keycode = 0;
static uint8_t s_repeat_modifier = 0;
static int64_t s_repeat_next_us = 0;
static bool s_repeat_active = false;

static constexpr int64_t KEY_REPEAT_INITIAL_DELAY_US = 400000;
static constexpr int64_t KEY_REPEAT_INTERVAL_US = 60000;

static void repeat_reset(void) {
    s_repeat_keycode = 0;
    s_repeat_modifier = 0;
    s_repeat_next_us = 0;
    s_repeat_active = false;
}

static bool key_is_held(const uint8_t keys[6], uint8_t keycode) {
    if (!keycode) return false;
    for (int i = 0; i < 6; i++) {
        if (keys[i] == keycode) return true;
    }
    return false;
}

static void repeat_update_from_report(uint8_t modifier, const uint8_t keys[6]) {
    uint8_t first_key = 0;
    for (int i = 0; i < 6; i++) {
        if (keys[i] != 0) {
            first_key = keys[i];
            break;
        }
    }

    if (first_key == 0) {
        repeat_reset();
        return;
    }

    if (!s_repeat_active || s_repeat_keycode != first_key || s_repeat_modifier != modifier) {
        s_repeat_active = true;
        s_repeat_keycode = first_key;
        s_repeat_modifier = modifier;
        s_repeat_next_us = esp_timer_get_time() + KEY_REPEAT_INITIAL_DELAY_US;
        return;
    }

    if (!key_is_held(keys, s_repeat_keycode)) {
        repeat_reset();
    }
}

static void maybe_emit_key_repeat(void);

static void bump_scan_generation(void) {
    portENTER_CRITICAL(&scan_lock);
    scan_generation++;
    portEXIT_CRITICAL(&scan_lock);
}

static void clear_scan_results(void) {
    portENTER_CRITICAL(&scan_lock);
    scan_result_count = 0;
    memset(scan_results, 0, sizeof(scan_results));
    scan_generation++;
    portEXIT_CRITICAL(&scan_lock);
}

static bool addr_matches_prefix(const ble_addr_t *addr) {
    if (!has_paired_prefix || !addr) return false;
    return addr->val[5] == paired_prefix[0] &&
           addr->val[4] == paired_prefix[1] &&
           addr->val[3] == paired_prefix[2] &&
           addr->val[2] == paired_prefix[3];
}

static void addr_to_text(const ble_addr_t *addr, char *buf, size_t len) {
    if (!buf || len == 0) return;
    if (!addr) {
        snprintf(buf, len, "unknown");
        return;
    }
    snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             addr->val[5], addr->val[4], addr->val[3],
             addr->val[2], addr->val[1], addr->val[0]);
}

static void log_conn_desc(const char *prefix, uint16_t handle) {
    struct ble_gap_conn_desc desc;
    int rc = ble_gap_conn_find(handle, &desc);
    if (rc != 0) {
        ESP_LOGW(TAG, "%s conn_desc unavailable handle=%u rc=%d(%s)",
                 prefix,
                 (unsigned)handle,
                 rc,
                 ble_hs_rc_to_text(rc));
        return;
    }

    char peer_addr[24] = {0};
    char local_addr[24] = {0};
    addr_to_text(&desc.peer_ota_addr, peer_addr, sizeof(peer_addr));
    addr_to_text(&desc.our_ota_addr, local_addr, sizeof(local_addr));
    ESP_LOGI(TAG,
             "%s handle=%u peer=%s(type=%u) local=%s(type=%u) encrypted=%u authenticated=%u bonded=%u interval=%u latency=%u timeout=%u",
             prefix,
             (unsigned)handle,
             peer_addr,
             (unsigned)desc.peer_ota_addr.type,
             local_addr,
             (unsigned)desc.our_ota_addr.type,
             desc.sec_state.encrypted,
             desc.sec_state.authenticated,
             desc.sec_state.bonded,
             (unsigned)desc.conn_itvl,
             (unsigned)desc.conn_latency,
             (unsigned)desc.supervision_timeout);
}

static bool str_contains_case_insensitive(const char *haystack, const char *needle) {
    if (!haystack || !needle || !needle[0]) return false;
    const size_t nlen = strlen(needle);
    for (const char *h = haystack; *h; h++) {
        size_t i = 0;
        while (i < nlen && h[i] && tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) return true;
    }
    return false;
}

static void parse_adv_name(const uint8_t *data, uint8_t len, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!data || len == 0) return;

    uint8_t i = 0;
    while (i + 1 < len) {
        uint8_t field_len = data[i];
        if (field_len == 0 || (uint16_t)i + field_len >= len) break;
        uint8_t type = data[i + 1];
        const uint8_t *field = &data[i + 2];
        uint8_t field_payload_len = field_len - 1;
        if ((type == 0x09 || type == 0x08) && field_payload_len > 0) {
            size_t n = field_payload_len < (out_len - 1) ? field_payload_len : (out_len - 1);
            memcpy(out, field, n);
            out[n] = '\0';
            return;
        }
        i = (uint8_t)(i + field_len + 1);
    }
}

static bool adv_looks_like_hid_keyboard(const uint8_t *data, uint8_t len, const char *name_hint) {
    bool has_hid_service = false;
    bool has_appearance = false;
    bool appearance_keyboard = false;
    bool appearance_hid_generic = false;

    if (data && len > 0) {
        uint8_t i = 0;
        while (i + 1 < len) {
            uint8_t field_len = data[i];
            if (field_len == 0 || (uint16_t)i + field_len >= len) break;
            uint8_t type = data[i + 1];
            const uint8_t *field = &data[i + 2];
            uint8_t field_payload_len = field_len - 1;

            if ((type == 0x02 || type == 0x03) && field_payload_len >= 2) {
                for (uint8_t j = 0; j + 1 < field_payload_len; j += 2) {
                    uint16_t uuid16 = (uint16_t)field[j] | ((uint16_t)field[j + 1] << 8);
                    if (uuid16 == 0x1812) {
                        has_hid_service = true;
                        break;
                    }
                }
            } else if (type == 0x19 && field_payload_len >= 2) {
                uint16_t appearance = (uint16_t)field[0] | ((uint16_t)field[1] << 8);
                has_appearance = true;
                appearance_keyboard = (appearance == 0x03C1);
                appearance_hid_generic = ((appearance & 0xFFC0) == 0x03C0);
            }

            i = (uint8_t)(i + field_len + 1);
        }
    }

    if (appearance_keyboard) return true;
    if (has_hid_service && (!has_appearance || appearance_hid_generic)) {
        if (name_hint && str_contains_case_insensitive(name_hint, "mouse")) return false;
        return true;
    }
    if (has_hid_service && name_hint &&
        (str_contains_case_insensitive(name_hint, "kbd") || str_contains_case_insensitive(name_hint, "keyboard"))) {
        return true;
    }
    return false;
}

static void save_paired_to_nvs(void) {
    nvs_handle_t h;
    if (nvs_open("blehid", NVS_READWRITE, &h) != ESP_OK) return;
    if (has_paired_prefix) {
        nvs_set_blob(h, "pref", paired_prefix, sizeof(paired_prefix));
        nvs_set_str(h, "name", paired_name[0] ? paired_name : "HID keyboard");
        nvs_set_u8(h, "slot", paired_slot);
        if (has_paired_addr) {
            nvs_set_blob(h, "addr", &paired_addr, sizeof(paired_addr));
        } else {
            nvs_erase_key(h, "addr");
        }
    } else {
        nvs_erase_key(h, "pref");
        nvs_erase_key(h, "name");
        nvs_erase_key(h, "slot");
        nvs_erase_key(h, "addr");
    }
    nvs_commit(h);
    nvs_close(h);
}

static void load_paired_from_nvs(void) {
    has_paired_prefix = false;
    paired_slot = 0;
    has_paired_addr = false;
    paired_name[0] = '\0';

    nvs_handle_t h;
    if (nvs_open("blehid", NVS_READONLY, &h) != ESP_OK) return;

    size_t len = sizeof(paired_prefix);
    if (nvs_get_blob(h, "pref", paired_prefix, &len) == ESP_OK && len == sizeof(paired_prefix)) {
        has_paired_prefix = true;
    }

    len = sizeof(paired_name);
    if (nvs_get_str(h, "name", paired_name, &len) != ESP_OK) {
        paired_name[0] = '\0';
    }
    uint8_t slot = 0;
    if (nvs_get_u8(h, "slot", &slot) == ESP_OK) {
        paired_slot = slot;
    }
    len = sizeof(paired_addr);
    if (nvs_get_blob(h, "addr", &paired_addr, &len) == ESP_OK && len == sizeof(paired_addr)) {
        has_paired_addr = true;
    }
    nvs_close(h);
}

static void set_paired_target(const ble_addr_t *addr, const char *name, uint8_t slot) {
    if (!addr) return;
    paired_prefix[0] = addr->val[5];
    paired_prefix[1] = addr->val[4];
    paired_prefix[2] = addr->val[3];
    paired_prefix[3] = addr->val[2];
    has_paired_prefix = true;
    if (name && name[0]) {
        snprintf(paired_name, sizeof(paired_name), "%s", name);
    } else {
        addr_to_text(addr, paired_name, sizeof(paired_name));
    }
    paired_slot = slot;
    if (addr) {
        paired_addr = *addr;
        has_paired_addr = true;
    }
    save_paired_to_nvs();
}

static void clear_paired_target(void) {
    has_paired_prefix = false;
    memset(paired_prefix, 0, sizeof(paired_prefix));
    paired_slot = 0;
    has_paired_addr = false;
    memset(&paired_addr, 0, sizeof(paired_addr));
    paired_name[0] = '\0';
    connected_name[0] = '\0';
    pending_connect_name[0] = '\0';
    save_paired_to_nvs();
}

static bool scan_result_addr_equal(const ble_hid_scan_result_t *a, const ble_addr_t *b) {
    if (!a || !b) return false;
    return a->addr_type == b->type && memcmp(a->addr, b->val, 6) == 0;
}

static void add_or_update_scan_result(const ble_addr_t *addr,
                                      const char *name,
                                      int8_t rssi,
                                      bool is_hid_keyboard) {
    if (!addr) return;

    char addr_buf[24] = {0};
    char name_buf[BLE_HID_NAME_MAX] = {0};
    int idx = -1;
    int count_snapshot = 0;
    bool created = false;
    bool dropped = false;
    addr_to_text(addr, addr_buf, sizeof(addr_buf));

    portENTER_CRITICAL(&scan_lock);
    for (int i = 0; i < scan_result_count; i++) {
        if (scan_result_addr_equal(&scan_results[i], addr)) {
            idx = i;
            break;
        }
    }

    if (idx < 0 && scan_result_count < BLE_HID_SCAN_MAX_RESULTS) {
        idx = scan_result_count++;
        created = true;
        memset(&scan_results[idx], 0, sizeof(scan_results[idx]));
        memcpy(scan_results[idx].addr, addr->val, 6);
        scan_results[idx].addr_type = addr->type;
    }

    if (idx >= 0) {
        if (name && name[0]) {
            snprintf(scan_results[idx].name, sizeof(scan_results[idx].name), "%s", name);
        } else {
            snprintf(scan_results[idx].name, sizeof(scan_results[idx].name),
                     "%02X:%02X:%02X:%02X:%02X:%02X",
                     addr->val[5], addr->val[4], addr->val[3], addr->val[2], addr->val[1], addr->val[0]);
        }
        scan_generation++;
        if (created) {
            scan_added_count++;
        } else {
            scan_updated_count++;
        }
        count_snapshot = scan_result_count;
        snprintf(name_buf, sizeof(name_buf), "%s",
                 scan_results[idx].name[0] ? scan_results[idx].name : "<none>");
    } else {
        dropped = true;
        snprintf(name_buf, sizeof(name_buf), "%s", (name && name[0]) ? name : "<none>");
    }
    portEXIT_CRITICAL(&scan_lock);

    if (dropped) {
        ESP_LOGW(TAG,
                 "scan result full (%d max), dropping addr=%s rssi=%d name=%s",
                 BLE_HID_SCAN_MAX_RESULTS,
                 addr_buf,
                 (int)rssi,
                 name_buf);
        return;
    }

    ESP_LOGI(TAG,
             "scan %s idx=%d/%d addr=%s type=%u rssi=%d hid=%u name=%s",
             created ? "add" : "update",
             idx,
             count_snapshot,
             addr_buf,
             (unsigned)addr->type,
             (int)rssi,
             is_hid_keyboard ? 1 : 0,
             name_buf);
}

static esp_err_t connect_paired_now(void) {
    if (!has_paired_prefix || !has_paired_addr) return ESP_ERR_NOT_FOUND;
    if (conn_handle != 0xFFFF || scan_active) return ESP_ERR_INVALID_STATE;

    if (paired_name[0]) {
        snprintf(pending_connect_name, sizeof(pending_connect_name), "%s", paired_name);
    }

    char addr_buf[24] = {0};
    addr_to_text(&paired_addr, addr_buf, sizeof(addr_buf));
    ESP_LOGI(TAG,
             "connecting to paired target name=%s addr=%s own_addr_type=%u",
             paired_name[0] ? paired_name : "<none>",
             addr_buf,
             (unsigned)own_addr_type);

    int rc = ble_gap_connect(own_addr_type, &paired_addr, 30000, NULL, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_connect (paired) failed rc=%d(%s)", rc, ble_hs_rc_to_text(rc));
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void update_key(char c) {
    if (!msg_queue) return;
    disp_msg_t msg;
    msg.type = 1;
    msg.text[0] = c;
    msg.text[1] = '\0';
    xQueueSend(msg_queue, &msg, 0);
}

void ble_hid_process_queue(void) {
    maybe_emit_key_repeat();
    if (!msg_queue) return;
    disp_msg_t msg;
    while (xQueueReceive(msg_queue, &msg, 0) == pdTRUE) {
        if (msg.type == 1) {
            shell_handle_key(msg.text[0]);
        }
    }
}

static char map_shifted(char c) {
    const char unsorted[] = "1234567890-=[]\\;',./`";
    const char *p = strchr(unsorted, c);
    if (p) return shift_to[p - unsorted];
    return c;
}

static void log_report_preview(const char *prefix, const uint8_t *data, uint16_t len) {
    uint8_t b0 = len > 0 ? data[0] : 0;
    uint8_t b1 = len > 1 ? data[1] : 0;
    uint8_t b2 = len > 2 ? data[2] : 0;
    uint8_t b3 = len > 3 ? data[3] : 0;
    uint8_t b4 = len > 4 ? data[4] : 0;
    uint8_t b5 = len > 5 ? data[5] : 0;
    uint8_t b6 = len > 6 ? data[6] : 0;
    uint8_t b7 = len > 7 ? data[7] : 0;
    ESP_LOGI(TAG,
             "%s len=%u bytes=%02X %02X %02X %02X %02X %02X %02X %02X",
             prefix,
             (unsigned)len,
             b0, b1, b2, b3, b4, b5, b6, b7);
}

static bool dispatch_keycode(uint8_t modifier, uint8_t keycode) {
    if (keycode == 0x29) {
        ESP_LOGI(TAG, "BLE keycode ESC detected");
    }

    const bool shift = (modifier & 0x22) != 0;
    const bool ctrl = (modifier & 0x11) != 0;

    if (ctrl && shift && keycode == 0x52) { update_key((char)0x80); return true; } // Ctrl+Shift+Up (scroll up)
    if (ctrl && shift && keycode == 0x51) { update_key((char)0x81); return true; } // Ctrl+Shift+Down (scroll down)

    if (keycode == 0x52) { update_key(0x1E); return true; } // Up
    if (keycode == 0x51) { update_key(0x1F); return true; } // Down
    if (keycode == 0x4F) { update_key(0x1D); return true; } // Right
    if (keycode == 0x50) { update_key(0x1C); return true; } // Left
    if (keycode == 0x4C) { update_key(0x7F); return true; } // Delete
    if (keycode == 0x54) { update_key('/'); return true; }  // Keypad /
    if (keycode == 0x58) { update_key('\r'); return true; } // Keypad Enter

    if (keycode >= sizeof(hid_to_ascii)) {
        ESP_LOGI(TAG, "unmapped keycode=0x%02X mod=0x%02X", keycode, modifier);
        return false;
    }

    char c = hid_to_ascii[keycode];
    if (c == 0) {
        ESP_LOGI(TAG, "no-ascii keycode=0x%02X mod=0x%02X", keycode, modifier);
        return false;
    }

    if (ctrl) {
        if (c >= 'a' && c <= 'z') {
            update_key((char)(c - 'a' + 1));
            return true;
        }
        if (c == '[') { update_key(0x1B); return true; }
        if (c == '\\') { update_key(0x1C); return true; }
        if (c == ']') { update_key(0x1D); return true; }
        if (c == '6') { update_key(0x1E); return true; }
        if (c == '-') { update_key(0x1F); return true; }
        if (c == '/') { update_key(0x1F); return true; }
    }

    if (shift) {
        if (c >= 'a' && c <= 'z') {
            c -= 32;
        } else {
            c = map_shifted(c);
        }
    }

    update_key(c);
    return true;
}

static void maybe_emit_key_repeat(void) {
    if (!s_repeat_active) return;
    const int64_t now_us = esp_timer_get_time();
    if (now_us < s_repeat_next_us) return;

    if (!dispatch_keycode(s_repeat_modifier, s_repeat_keycode)) {
        repeat_reset();
        return;
    }

    s_repeat_next_us = now_us + KEY_REPEAT_INTERVAL_US;
}

static bool handle_kbd_report(const uint8_t *data, uint16_t len) {
    if (!data || len < 3) return false;

    /*
     * IMPORTANT (ESC reliability):
     * Some BLE keyboards send a boot report as [mod,res,k1..k6], while others
     * prepend report ID and send [id,mod,res,k1..k6].
     *
     * We MUST decode both layouts and process *new key presses* (edge trigger)
     * instead of just first non-zero key byte, otherwise ESC (0x29) can be
     * missed when another key remains held.
     *
     * Keep this dual-layout + edge-detect logic unless replacing with a full
     * HID descriptor-driven parser.
     */
    uint16_t off = 0;
    if (len >= 9 && data[2] == 0 && data[1] <= 0xE7) {
        off = 1;
    }

    if (len >= (uint16_t)(off + 8)) {
        uint8_t modifier = data[off + 0];
        uint8_t cur_keys[6] = {0};
        bool emitted_key = false;
        for (int i = 0; i < 6; i++) {
            cur_keys[i] = data[off + 2 + i];
        }

        for (int i = 0; i < 6; i++) {
            uint8_t keycode = cur_keys[i];
            if (keycode == 0) continue;

            bool already_held = false;
            for (int j = 0; j < 6; j++) {
                if (s_prev_keys[j] == keycode) {
                    already_held = true;
                    break;
                }
            }
            if (already_held) continue;

            emitted_key |= dispatch_keycode(modifier, keycode);
        }

        if (!emitted_key) {
            bool any_key = false;
            for (int i = 0; i < 6; i++) {
                if (cur_keys[i] != 0) {
                    any_key = true;
                    break;
                }
            }
            if (any_key) {
                ESP_LOGI(TAG,
                         "HID report had keys but no mapped output (off=%u mod=0x%02X keys=%02X %02X %02X %02X %02X %02X)",
                         (unsigned)off,
                         modifier,
                         cur_keys[0], cur_keys[1], cur_keys[2],
                         cur_keys[3], cur_keys[4], cur_keys[5]);
            } else {
                bool any_non_zero = false;
                for (uint16_t i = 0; i < len; i++) {
                    if (data[i] != 0) {
                        any_non_zero = true;
                        break;
                    }
                }
                if (any_non_zero) {
                    /*
                     * IMPORTANT: FN-layer keys can arrive on non-boot report
                     * IDs where k1..k6 are all zero. Keep this preview log so
                     * we can recover ESC/FN behavior after future key-map edits.
                     */
                    log_report_preview("Non-boot/non-keyboard report", data, len);
                }
            }
        }

        repeat_update_from_report(modifier, cur_keys);
        memcpy(s_prev_keys, cur_keys, sizeof(s_prev_keys));
        return emitted_key;
    }

    /*
     * IMPORTANT (FN-layer ESC):
     * A subset of compact keyboards reports FN-combo keys using short reports
     * (e.g. 3-4 bytes) instead of the 8-byte boot report. If we ignore short
     * reports, FN+ESC can disappear entirely even when normal keys still work.
     *
     * Keep this short-report fallback unless we migrate to full HID-report
     * descriptor parsing across all report IDs.
     */
    uint8_t short_off = 0;
    if (len >= 4 && data[2] == 0 && data[1] <= 0xE7) {
        short_off = 1;
    }
    if (len <= (uint16_t)(short_off + 2)) return false;

    uint8_t modifier = data[short_off + 0];
    uint8_t keycode = data[short_off + 2];
        if (keycode == 0) {
        if (len == 3 && data[0] == 0x00 && data[2] == 0x00) {
            /*
             * IMPORTANT (FN+ESC compatibility):
             * This keyboard emits FN-layer ESC as a 3-byte vendor-style report:
             *   00 80 00
             * instead of boot keycode 0x29.
             *
             * Keep this edge-triggered surrogate mapping unless we implement
             * full HID report-descriptor parsing for all report IDs/usages.
             */
            uint8_t flags = data[1];
            const bool esc_now = (flags & 0x80) != 0;
            const bool esc_prev = (s_prev_short_flags & 0x80) != 0;
            if (esc_now && !esc_prev) {
                ESP_LOGI(TAG, "FN-layer ESC surrogate detected (flags=0x%02X)", flags);
                update_key(0x1B);
                s_prev_short_flags = flags;
                return true;
            }
            s_prev_short_flags = flags;
            repeat_reset();
        }

        bool any_non_zero = false;
        for (uint16_t i = 0; i < len; i++) {
            if (data[i] != 0) {
                any_non_zero = true;
                break;
            }
        }
        if (any_non_zero) {
            log_report_preview("Short HID report (no keycode)", data, len);
        }
        return false;
    }

    ESP_LOGI(TAG,
             "Short HID report len=%u off=%u mod=0x%02X key=0x%02X",
             (unsigned)len,
             (unsigned)short_off,
             modifier,
             keycode);
    s_repeat_active = true;
    s_repeat_keycode = keycode;
    s_repeat_modifier = modifier;
    s_repeat_next_us = esp_timer_get_time() + KEY_REPEAT_INITIAL_DELAY_US;
    return dispatch_keycode(modifier, keycode);
}

static int gatt_disc_chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                                const struct ble_gatt_chr *chr, void *arg) {
    uint32_t disc_ctx = (uint32_t)(uintptr_t)arg;
    uint16_t svc_end_handle = (uint16_t)(disc_ctx & 0xFFFFu);
    uint16_t svc_kind = (uint16_t)((disc_ctx >> 16) & 0xFFFFu);
    bool is_hid_svc = (svc_kind == DISC_CTX_HID);
    bool is_vendor_svc = (svc_kind == DISC_CTX_VENDOR);
    if (error && error->status != 0) return 0;
    if (chr == NULL) return 0;

    if (chr->uuid.u.type == BLE_UUID_TYPE_16) {
        ESP_LOGI(TAG,
                 "char discovered uuid16=0x%04X def=0x%04X val=0x%04X props=0x%02X",
                 chr->uuid.u16.value,
                 chr->def_handle,
                 chr->val_handle,
                 (unsigned)chr->properties);
    } else {
        ESP_LOGI(TAG,
                 "char discovered uuid_type=%u def=0x%04X val=0x%04X props=0x%02X",
                 (unsigned)chr->uuid.u.type,
                 chr->def_handle,
                 chr->val_handle,
                 (unsigned)chr->properties);
    }

    bool hid_input_candidate = false;
    bool vendor_input_candidate = false;
    uint16_t cccd_bits = 0;

    if (is_hid_svc &&
        chr->uuid.u.type == BLE_UUID_TYPE_16 &&
        (chr->uuid.u16.value == 0x2A4D || chr->uuid.u16.value == 0x2A22) &&
        (chr->properties & BLE_GATT_CHR_F_NOTIFY)) {
        hid_input_candidate = true;
        cccd_bits = 0x0001;
    } else if (is_vendor_svc &&
               (chr->properties & (BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_INDICATE))) {
        vendor_input_candidate = true;
        if (chr->properties & BLE_GATT_CHR_F_NOTIFY) {
            cccd_bits |= 0x0001;
        }
        if (chr->properties & BLE_GATT_CHR_F_INDICATE) {
            cccd_bits |= 0x0002;
        }
    }

    if (hid_input_candidate || vendor_input_candidate) {

        const char *reason = hid_input_candidate ? "hid-input" : "vendor-input";

        ESP_LOGI(TAG,
                 "%s candidate uuid_type=%u val=0x%04X svc_end=0x%04X cccd_bits=0x%04X",
                 reason,
                 (unsigned)chr->uuid.u.type,
                 chr->val_handle,
                 svc_end_handle,
                 cccd_bits);

        uint16_t cccd_handles[] = {(uint16_t)(chr->val_handle + 1), (uint16_t)(chr->val_handle + 2)};
        for (int i = 0; i < 2; i++) {
            ESP_LOGI(TAG,
                     "trying quick CCCD subscribe val=0x%04X cccd=0x%04X bits=0x%04X reason=%s",
                     chr->val_handle,
                     cccd_handles[i],
                     cccd_bits,
                     reason);
            if (subscribe_cccd(conn_handle, cccd_handles[i], cccd_bits, reason)) {
                return 0;
            }
        }

        s_notify_val_handle = chr->val_handle;
        s_notify_cccd_handle = 0;
        int rc = ble_gattc_disc_all_dscs(conn_handle,
                                         chr->val_handle,
                                         svc_end_handle,
                                         gatt_disc_dsc_cb,
                                         (void *)(uintptr_t)cccd_bits);
        if (rc != 0) {
            ESP_LOGW(TAG, "descriptor discovery start failed rc=%d", rc);
        } else {
            ESP_LOGI(TAG,
                     "descriptor discovery started for val=0x%04X..0x%04X",
                     chr->val_handle,
                     svc_end_handle);
        }
    }

    if (chr->uuid.u.type == BLE_UUID_TYPE_16 && chr->uuid.u16.value == 0x2A4C &&
        (chr->properties & (BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP))) {
        write_hid_char_u8(conn_handle, chr->val_handle, 0x00, "hid control point exit suspend");
    }

    if (chr->uuid.u.type == BLE_UUID_TYPE_16 && chr->uuid.u16.value == 0x2A4E &&
        (chr->properties & (BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP))) {
        write_hid_char_u8(conn_handle, chr->val_handle, 0x00, "hid protocol mode boot");
    }

    return 0;
}

static int gatt_disc_svc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                              const struct ble_gatt_svc *svc, void *arg) {
    if (error && error->status != 0) return 0;
    if (svc == NULL) return 0;

    if (svc->uuid.u.type == BLE_UUID_TYPE_16) {
        ESP_LOGI(TAG,
                 "service discovered uuid16=0x%04X start=0x%04X end=0x%04X",
                 svc->uuid.u16.value,
                 svc->start_handle,
                 svc->end_handle);
    } else {
        ESP_LOGI(TAG,
                 "service discovered uuid_type=%u start=0x%04X end=0x%04X",
                 (unsigned)svc->uuid.u.type,
                 svc->start_handle,
                 svc->end_handle);
    }

    if (svc->uuid.u.type == BLE_UUID_TYPE_16 &&
        (svc->uuid.u16.value == 0x1812 || svc->uuid.u16.value == 0xFF50)) {
        uint16_t svc_kind = (svc->uuid.u16.value == 0x1812) ? DISC_CTX_HID : DISC_CTX_VENDOR;
        uint32_t disc_ctx = ((uint32_t)svc_kind << 16) | (uint32_t)svc->end_handle;
        ESP_LOGI(TAG,
                 "%s service found, starting characteristic discovery",
                 (svc_kind == DISC_CTX_HID) ? "HID" : "vendor");
        int rc = ble_gattc_disc_all_chrs(conn_handle,
                                         svc->start_handle,
                                         svc->end_handle,
                                         gatt_disc_chr_cb,
                                         (void *)(uintptr_t)disc_ctx);
        if (rc != 0) {
            ESP_LOGW(TAG,
                     "characteristic discovery start failed rc=%d(%s)",
                     rc,
                     ble_hs_rc_to_text(rc));
        }
    }
    return 0;
}

static int gap_event_cb(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            ESP_LOGI(TAG, "GAP connect event status=%d", event->connect.status);
            if (event->connect.status == 0) {
                conn_handle = event->connect.conn_handle;
                memset(s_prev_keys, 0, sizeof(s_prev_keys));
                s_prev_short_flags = 0;
                repeat_reset();
                scan_active = false;
                hid_configured = false;
                s_notify_rx_count = 0;
                s_decode_ok_count = 0;
                s_decode_drop_count = 0;
                s_security_requested = false;
                manual_scan_mode = false;
                if (pending_connect_name[0]) {
                    snprintf(connected_name, sizeof(connected_name), "%s", pending_connect_name);
                } else if (paired_name[0]) {
                    snprintf(connected_name, sizeof(connected_name), "%s", paired_name);
                } else {
                    connected_name[0] = '\0';
                }
                bump_scan_generation();
                log_conn_desc("connected", conn_handle);
                maybe_request_security(conn_handle, "connect");

                int rc = ble_gattc_disc_all_svcs(conn_handle, gatt_disc_svc_cb, NULL);
                if (rc != 0) {
                    ESP_LOGW(TAG,
                             "service discovery start failed rc=%d(%s)",
                             rc,
                             ble_hs_rc_to_text(rc));
                }
            } else {
                conn_handle = 0xFFFF;
                repeat_reset();
                connected_name[0] = '\0';
                ESP_LOGW(TAG,
                         "connect failed, status=%d(%s)",
                         event->connect.status,
                         ble_hs_rc_to_text(BLE_HS_ERR_HCI_BASE + event->connect.status));
                if (auto_reconnect && has_paired_prefix) {
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    ble_hid_start_scan();
                }
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGW(TAG,
                     "disconnect reason=%d(%s) conn=%u",
                     event->disconnect.reason,
                     ble_hs_rc_to_text(event->disconnect.reason),
                     (unsigned)event->disconnect.conn.conn_handle);
            conn_handle = 0xFFFF;
            hid_configured = false;
            s_notify_rx_count = 0;
            s_decode_ok_count = 0;
            s_decode_drop_count = 0;
            s_security_requested = false;
            memset(s_prev_keys, 0, sizeof(s_prev_keys));
            s_prev_short_flags = 0;
            repeat_reset();
            connected_name[0] = '\0';
            bump_scan_generation();
            if (auto_reconnect && has_paired_prefix) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                ble_hid_start_scan();
            }
            break;

        case BLE_GAP_EVENT_DISC:
            {
                scan_evt_count++;
                char adv_name[BLE_HID_NAME_MAX] = {0};
                parse_adv_name(event->disc.data, event->disc.length_data, adv_name, sizeof(adv_name));
                const bool is_hid_keyboard = adv_looks_like_hid_keyboard(
                    event->disc.data,
                    event->disc.length_data,
                    adv_name[0] ? adv_name : NULL);
                if (is_hid_keyboard) {
                    scan_candidate_count++;
                }

                if (manual_scan_mode && (is_hid_keyboard || adv_name[0])) {
                    add_or_update_scan_result(&event->disc.addr,
                                              adv_name[0] ? adv_name : NULL,
                                              event->disc.rssi,
                                              is_hid_keyboard);
                } else if (manual_scan_mode && (scan_evt_count % 25u) == 0u) {
                    char addr_buf[24] = {0};
                    addr_to_text(&event->disc.addr, addr_buf, sizeof(addr_buf));
                    ESP_LOGI(TAG,
                             "scan ignore evt=%lu addr=%s rssi=%d name=%s hid=%u",
                             (unsigned long)scan_evt_count,
                             addr_buf,
                             (int)event->disc.rssi,
                             adv_name[0] ? adv_name : "<none>",
                             is_hid_keyboard ? 1 : 0);
                }

                if (has_paired_prefix && addr_matches_prefix(&event->disc.addr)) {
                    scan_active = false;
                    ESP_LOGI(TAG,
                             "paired prefix match found (hid=%u), cancel scan and connect",
                             is_hid_keyboard ? 1 : 0);
                    ble_gap_disc_cancel();
                    ble_addr_t addr = event->disc.addr;
                    paired_addr = addr;
                    has_paired_addr = true;
                    save_paired_to_nvs();
                    if (adv_name[0]) {
                        snprintf(pending_connect_name, sizeof(pending_connect_name), "%s", adv_name);
                    } else if (paired_name[0]) {
                        snprintf(pending_connect_name, sizeof(pending_connect_name), "%s", paired_name);
                    } else {
                        pending_connect_name[0] = '\0';
                    }
                    int rc = ble_gap_connect(own_addr_type, &addr, 30000, NULL, gap_event_cb, NULL);
                    if (rc != 0) {
                        ESP_LOGW(TAG,
                                 "auto connect after scan match failed rc=%d(%s)",
                                 rc,
                                 ble_hs_rc_to_text(rc));
                    }
                }
            }
            break;

        case BLE_GAP_EVENT_DISC_COMPLETE:
            scan_active = false;
            if (event->disc_complete.reason != 0) {
                ESP_LOGW(TAG, "scan complete reason=%d", event->disc_complete.reason);
            }
            ESP_LOGI(TAG,
                     "scan complete events=%lu hid_candidates=%lu added=%lu updated=%lu results=%d",
                     (unsigned long)scan_evt_count,
                     (unsigned long)scan_candidate_count,
                     (unsigned long)scan_added_count,
                     (unsigned long)scan_updated_count,
                     scan_result_count);
            if (auto_reconnect && has_paired_prefix && conn_handle == 0xFFFF) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                ble_hid_start_scan();
            }
            bump_scan_generation();
            break;

        case BLE_GAP_EVENT_NOTIFY_RX:
            if (hid_configured) {
                s_notify_rx_count++;
                const uint16_t attr = event->notify_rx.attr_handle;
                const uint16_t len = event->notify_rx.om ? event->notify_rx.om->om_len : 0;
                if (s_notify_rx_count <= 8 || (s_notify_rx_count % 25u) == 0u) {
                    ESP_LOGI(TAG,
                             "notify rx #%lu attr=0x%04X len=%u",
                             (unsigned long)s_notify_rx_count,
                             attr,
                             (unsigned)len);
                    if (event->notify_rx.om) {
                        log_report_preview("notify preview", event->notify_rx.om->om_data, len);
                    }
                }

                bool decoded = false;
                if (event->notify_rx.om) {
                    decoded = handle_kbd_report(event->notify_rx.om->om_data,
                                                event->notify_rx.om->om_len);
                }

                if (decoded) {
                    s_decode_ok_count++;
                } else {
                    s_decode_drop_count++;
                }

                if (s_notify_rx_count <= 8 || (s_notify_rx_count % 25u) == 0u) {
                    ESP_LOGI(TAG,
                             "notify decode stats ok=%lu drop=%lu",
                             (unsigned long)s_decode_ok_count,
                             (unsigned long)s_decode_drop_count);
                }
            } else {
                ESP_LOGI(TAG, "notify received before HID configured; attr=0x%04X len=%u",
                         event->notify_rx.attr_handle,
                         (unsigned)event->notify_rx.om->om_len);
            }
            break;

        case BLE_GAP_EVENT_ENC_CHANGE:
            ESP_LOGI(TAG,
                     "encryption change status=%d(%s) conn=%u",
                     event->enc_change.status,
                     ble_hs_rc_to_text(event->enc_change.status),
                     (unsigned)event->enc_change.conn_handle);
            log_conn_desc("enc_change", event->enc_change.conn_handle);
            ble_gattc_disc_all_svcs(conn_handle, gatt_disc_svc_cb, NULL);
            break;

        case BLE_GAP_EVENT_LINK_ESTAB:
            ESP_LOGI(TAG,
                     "link established status=%d(%s) conn=%u",
                     event->link_estab.status,
                     ble_hs_rc_to_text(event->link_estab.status),
                     (unsigned)event->link_estab.conn_handle);
            if (event->link_estab.status == 0) {
                maybe_request_security(event->link_estab.conn_handle, "link_estab");
            }
            break;

        case BLE_GAP_EVENT_CONN_UPDATE:
            ESP_LOGI(TAG,
                     "conn update status=%d(%s) conn=%u",
                     event->conn_update.status,
                     ble_hs_rc_to_text(event->conn_update.status),
                     (unsigned)event->conn_update.conn_handle);
            log_conn_desc("conn_update", event->conn_update.conn_handle);
            break;

        case BLE_GAP_EVENT_CONN_UPDATE_REQ:
        case BLE_GAP_EVENT_L2CAP_UPDATE_REQ:
            ESP_LOGI(TAG,
                     "%s conn=%u req itvl=%u-%u lat=%u sup_to=%u ce=%u-%u",
                     event->type == BLE_GAP_EVENT_CONN_UPDATE_REQ ? "conn update req" : "l2cap update req",
                     (unsigned)event->conn_update_req.conn_handle,
                     (unsigned)event->conn_update_req.peer_params->itvl_min,
                     (unsigned)event->conn_update_req.peer_params->itvl_max,
                     (unsigned)event->conn_update_req.peer_params->latency,
                     (unsigned)event->conn_update_req.peer_params->supervision_timeout,
                     (unsigned)event->conn_update_req.peer_params->min_ce_len,
                     (unsigned)event->conn_update_req.peer_params->max_ce_len);
            return 0;

        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG,
                     "mtu update conn=%u cid=%u mtu=%u",
                     (unsigned)event->mtu.conn_handle,
                     (unsigned)event->mtu.channel_id,
                     (unsigned)event->mtu.value);
            break;

        case BLE_GAP_EVENT_TERM_FAILURE:
            ESP_LOGW(TAG,
                     "terminate failed status=%d(%s) conn=%u",
                     event->term_failure.status,
                     ble_hs_rc_to_text(event->term_failure.status),
                     (unsigned)event->term_failure.conn_handle);
            break;

        case BLE_GAP_EVENT_IDENTITY_RESOLVED:
            {
                char addr_buf[24] = {0};
                addr_to_text(&event->identity_resolved.peer_id_addr, addr_buf, sizeof(addr_buf));
                ESP_LOGI(TAG,
                         "identity resolved conn=%u peer_id=%s",
                         (unsigned)event->identity_resolved.conn_handle,
                         addr_buf);
            }
            break;

        case BLE_GAP_EVENT_REPEAT_PAIRING:
            {
                struct ble_gap_conn_desc desc;
                int rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
                if (rc == 0) {
                    char addr_buf[24] = {0};
                    addr_to_text(&desc.peer_id_addr, addr_buf, sizeof(addr_buf));
                    ESP_LOGW(TAG,
                             "repeat pairing conn=%u deleting old bond peer_id=%s and retrying",
                             (unsigned)event->repeat_pairing.conn_handle,
                             addr_buf);
                    ble_store_util_delete_peer(&desc.peer_id_addr);
                } else {
                    ESP_LOGW(TAG,
                             "repeat pairing conn=%u conn_find rc=%d(%s), retrying anyway",
                             (unsigned)event->repeat_pairing.conn_handle,
                             rc,
                             ble_hs_rc_to_text(rc));
                }
            }
            return BLE_GAP_REPEAT_PAIRING_RETRY;

        case BLE_GAP_EVENT_PASSKEY_ACTION:
            ESP_LOGW(TAG,
                     "passkey action conn=%u action=%u(%s) numcmp=%lu",
                     (unsigned)event->passkey.conn_handle,
                     (unsigned)event->passkey.params.action,
                     passkey_action_to_text(event->passkey.params.action),
                     (unsigned long)event->passkey.params.numcmp);
            if (event->passkey.params.action != BLE_SM_IOACT_NONE) {
                ESP_LOGW(TAG, "passkey action unsupported with sm_io_cap=NO_IO; rejecting");
                return BLE_HS_ENOTSUP;
            }
            break;

        case BLE_GAP_EVENT_PARING_COMPLETE:
            ESP_LOGI(TAG,
                     "pairing complete status=%d(%s) conn=%u",
                     event->pairing_complete.status,
                     ble_hs_rc_to_text(event->pairing_complete.status),
                     (unsigned)event->pairing_complete.conn_handle);
            log_conn_desc("pairing_complete", event->pairing_complete.conn_handle);
            break;

        default:
            ESP_LOGI(TAG,
                     "gap event type=%u(%s)",
                     (unsigned)event->type,
                     gap_event_type_to_text(event->type));
            break;
    }
    return 0;
}

void ble_hid_start_scan(void) {
    if (ble_runtime_disabled) {
        ESP_LOGW(TAG, "BLE runtime disabled due to repeated HCI timeouts");
        return;
    }
    if (!hs_synced) {
        pending_manual_scan = pending_manual_scan || manual_scan_mode;
        ESP_LOGW(TAG, "scan requested while NimBLE not synced; waiting for resync");
        return;
    }
    if (scan_active || conn_handle != 0xFFFF) return;

    struct ble_gap_disc_params disc_params = {};
    disc_params.itvl = 160;           // 100ms scan interval (0.625ms units)
    disc_params.window = 160;         // continuous receive in each interval
    disc_params.filter_duplicates = 0;
    disc_params.passive = 0;          // active scan to fetch scan response data

    int rc = ble_gap_disc(own_addr_type, 30000, &disc_params,
                          gap_event_cb, NULL);
    if (rc == 0) {
        scan_active = true;
        scan_evt_count = 0;
        scan_candidate_count = 0;
        scan_added_count = 0;
        scan_updated_count = 0;
        ESP_LOGI(TAG,
                 "scan started own_addr_type=%u manual=%u itvl=%u window=%u",
                 (unsigned)own_addr_type,
                 manual_scan_mode ? 1 : 0,
                 (unsigned)disc_params.itvl,
                 (unsigned)disc_params.window);
        bump_scan_generation();
    } else {
        ESP_LOGW(TAG,
                 "ble_gap_disc start failed rc=%d(%s) manual=%u scan_active=%u conn_handle=0x%04X",
                 rc,
                 ble_hs_rc_to_text(rc),
                 manual_scan_mode ? 1 : 0,
                 scan_active ? 1 : 0,
                 conn_handle);
        if (manual_scan_mode && (rc == BLE_HS_ECONTROLLER || rc == BLE_HS_ETIMEOUT_HCI)) {
            pending_manual_scan = true;
            if (!reset_scheduled) {
                reset_scheduled = true;
                ESP_LOGW(TAG, "scheduling NimBLE reset after scan start failure");
                ble_hs_sched_reset(rc);
            }
        }
    }
}

static void nimble_sync_cb(void) {
    if (ble_runtime_disabled) {
        return;
    }
    hs_synced = true;
    reset_scheduled = false;
    refresh_own_addr_type();
    load_paired_from_nvs();
    auto_reconnect = has_paired_prefix;
    if ((pending_manual_scan || manual_scan_mode) && !has_paired_prefix) {
        pending_manual_scan = false;
        manual_scan_mode = true;
        ble_hid_start_scan();
    } else if (has_paired_prefix) {
        if (connect_paired_now() != ESP_OK) {
            ble_hid_start_scan();
        }
    }
}

static void nimble_reset_cb(int reason) {
    hs_synced = false;
    reset_scheduled = false;
    scan_active = false;
    hid_configured = false;
    s_notify_rx_count = 0;
    s_decode_ok_count = 0;
    s_decode_drop_count = 0;
    s_security_requested = false;
    conn_handle = 0xFFFF;
    repeat_reset();
    connected_name[0] = '\0';
    pending_connect_name[0] = '\0';
    if (manual_scan_mode) {
        pending_manual_scan = true;
    }
    if (reason == BLE_HS_ETIMEOUT_HCI) {
        if (hci_timeout_resets < 255) {
            hci_timeout_resets++;
        }
        if (hci_timeout_resets >= 3 && !ble_runtime_disabled) {
            ble_runtime_disabled = true;
            manual_scan_mode = false;
            pending_manual_scan = false;
            auto_reconnect = false;
            ESP_LOGE(TAG, "disabling BLE runtime after %u HCI timeouts; check ESP-Hosted co-proc firmware", (unsigned)hci_timeout_resets);
            shell_print("\r\n  BLE unavailable (co-proc unresponsive)");
        }
    } else {
        hci_timeout_resets = 0;
    }
    if (!(ble_runtime_disabled && reason == BLE_HS_ETIMEOUT_HCI)) {
        ESP_LOGW(TAG, "NimBLE reset, reason=%d", reason);
    }
    bump_scan_generation();
}

static void nimble_host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_hid_host_disconnect(void) {
    auto_reconnect = has_paired_prefix;
    manual_scan_mode = false;
    if (scan_active) {
        ble_gap_disc_cancel();
        scan_active = false;
    }
    if (conn_handle != 0xFFFF) {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        conn_handle = 0xFFFF;
    }
    hid_configured = false;
    s_notify_rx_count = 0;
    s_decode_ok_count = 0;
    s_decode_drop_count = 0;
    s_security_requested = false;
    memset(s_prev_keys, 0, sizeof(s_prev_keys));
    s_prev_short_flags = 0;
    repeat_reset();
    connected_name[0] = '\0';
    bump_scan_generation();
}

void ble_hid_forget_device(void) {
    ble_hid_host_disconnect();
    clear_paired_target();
    auto_reconnect = false;
    manual_scan_mode = true;
    clear_scan_results();
    ble_hid_start_scan();
}

void ble_hid_scan_for_keyboards(void) {
    if (ble_runtime_disabled) {
        shell_print("\r\n  BLE unavailable (co-proc unresponsive)");
        return;
    }
    if (has_paired_prefix) return;
    manual_scan_mode = true;
    auto_reconnect = false;
    clear_scan_results();
    if (scan_active) {
        ble_gap_disc_cancel();
        scan_active = false;
    }
    if (!hs_synced) {
        pending_manual_scan = true;
        if (!reset_scheduled) {
            reset_scheduled = true;
            ble_hs_sched_reset(BLE_HS_ECONTROLLER);
        }
        return;
    }
    ble_hid_start_scan();
}

void ble_hid_pause_scan(void) {
    auto_reconnect = false;
    manual_scan_mode = false;
    if (scan_active) {
        ble_gap_disc_cancel();
    }
}

void ble_hid_resume_scan(void) {
    auto_reconnect = has_paired_prefix;
    manual_scan_mode = false;
    if (conn_handle == 0xFFFF && has_paired_prefix) {
        if (connect_paired_now() != ESP_OK) {
            ble_hid_start_scan();
        }
    }
}

esp_err_t ble_hid_pair_scan_index(int index) {
    ble_hid_scan_result_t picked = {};

    portENTER_CRITICAL(&scan_lock);
    if (index >= 0 && index < scan_result_count) {
        picked = scan_results[index];
    } else {
        portEXIT_CRITICAL(&scan_lock);
        return ESP_ERR_INVALID_ARG;
    }
    portEXIT_CRITICAL(&scan_lock);

    if (scan_active) {
        ble_gap_disc_cancel();
        scan_active = false;
    }
    if (conn_handle != 0xFFFF) {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        conn_handle = 0xFFFF;
    }

    ble_addr_t addr = {};
    addr.type = picked.addr_type;
    memcpy(addr.val, picked.addr, sizeof(addr.val));

    set_paired_target(&addr, picked.name, (uint8_t)(index + 1));
    snprintf(pending_connect_name, sizeof(pending_connect_name), "%s", picked.name);
    auto_reconnect = true;
    manual_scan_mode = false;

    char addr_buf[24] = {0};
    addr_to_text(&addr, addr_buf, sizeof(addr_buf));
    ESP_LOGI(TAG,
             "user selected scan index=%d name=%s addr=%s type=%u",
             index,
             picked.name[0] ? picked.name : "<none>",
             addr_buf,
             (unsigned)addr.type);

    int rc = ble_gap_connect(own_addr_type, &addr, 30000, NULL, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_connect failed: %d(%s)", rc, ble_hs_rc_to_text(rc));
        return ESP_FAIL;
    }

    bump_scan_generation();
    return ESP_OK;
}

bool ble_hid_is_connected(void) {
    return conn_handle != 0xFFFF;
}

bool ble_hid_is_scanning(void) {
    return scan_active;
}

bool ble_hid_is_connecting(void) {
    return (has_paired_prefix || manual_scan_mode) && (conn_handle == 0xFFFF) && !scan_active;
}

bool ble_hid_is_ready(void) {
    return hid_configured;
}

bool ble_hid_has_paired_device(void) {
    return has_paired_prefix;
}

int ble_hid_get_paired_slot(void) {
    return paired_slot > 0 ? (int)paired_slot : -1;
}

int ble_hid_get_scan_results(ble_hid_scan_result_t *out, int max_results) {
    if (!out || max_results <= 0) return 0;
    int copied = 0;
    portENTER_CRITICAL(&scan_lock);
    copied = scan_result_count < max_results ? scan_result_count : max_results;
    for (int i = 0; i < copied; i++) {
        out[i] = scan_results[i];
    }
    portEXIT_CRITICAL(&scan_lock);
    return copied;
}

uint32_t ble_hid_get_scan_generation(void) {
    uint32_t g;
    portENTER_CRITICAL(&scan_lock);
    g = scan_generation;
    portEXIT_CRITICAL(&scan_lock);
    return g;
}

void ble_hid_get_connected_name(char *buf, size_t len) {
    if (!buf || len == 0) return;
    if (connected_name[0]) {
        snprintf(buf, len, "%s", connected_name);
    } else if (paired_name[0]) {
        snprintf(buf, len, "%s", paired_name);
    } else {
        snprintf(buf, len, "HID keyboard");
    }
}

esp_err_t ble_hid_host_init(void) {
    msg_queue = xQueueCreate(10, sizeof(disp_msg_t));
    if (!msg_queue) return ESP_FAIL;

    clear_scan_results();
    connected_name[0] = '\0';
    pending_connect_name[0] = '\0';
    hs_synced = false;
    pending_manual_scan = false;
    reset_scheduled = false;
    ble_runtime_disabled = false;
    hci_timeout_resets = 0;

    nimble_port_init();
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("DumbESPty");

    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
#if CONFIG_IDF_TARGET_ESP32P4
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_bonding = 0;
#else
    ble_hs_cfg.sm_bonding = 1;
#endif
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sync_cb = nimble_sync_cb;
    ble_hs_cfg.reset_cb = nimble_reset_cb;
    ble_store_config_init();

    nimble_port_freertos_init(nimble_host_task);
    return ESP_OK;
}

void ble_hid_set_label(lv_obj_t *label) {
    status_label = label;
    lv_obj_set_scrollbar_mode(lv_scr_act(), LV_SCROLLBAR_MODE_OFF);
}
