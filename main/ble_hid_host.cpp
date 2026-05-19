#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "ble_hid_host.hpp"
#include "shell.hpp"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "esp_log.h"
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

static int gap_event_cb(struct ble_gap_event *event, void *arg);

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

static void add_or_update_scan_result(const ble_addr_t *addr, const char *name) {
    if (!addr) return;

    portENTER_CRITICAL(&scan_lock);
    int idx = -1;
    for (int i = 0; i < scan_result_count; i++) {
        if (scan_result_addr_equal(&scan_results[i], addr)) {
            idx = i;
            break;
        }
    }

    if (idx < 0 && scan_result_count < BLE_HID_SCAN_MAX_RESULTS) {
        idx = scan_result_count++;
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
    }
    portEXIT_CRITICAL(&scan_lock);
}

static esp_err_t connect_paired_now(void) {
    if (!has_paired_prefix || !has_paired_addr) return ESP_ERR_NOT_FOUND;
    if (conn_handle != 0xFFFF || scan_active) return ESP_ERR_INVALID_STATE;

    if (paired_name[0]) {
        snprintf(pending_connect_name, sizeof(pending_connect_name), "%s", paired_name);
    }

    int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &paired_addr, 30000, NULL, gap_event_cb, NULL);
    if (rc != 0) {
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

    if (keycode == 0x52) { update_key(0x1E); return true; } // Up
    if (keycode == 0x51) { update_key(0x1F); return true; } // Down
    if (keycode == 0x4F) { update_key(0x1D); return true; } // Right
    if (keycode == 0x50) { update_key(0x1C); return true; } // Left
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

    bool shift = modifier & 0x22;
    bool ctrl = modifier & 0x11;

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

static void handle_kbd_report(const uint8_t *data, uint16_t len) {
    if (!data || len < 3) return;

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

        memcpy(s_prev_keys, cur_keys, sizeof(s_prev_keys));
        return;
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
    if (len <= (uint16_t)(short_off + 2)) return;

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
            }
            s_prev_short_flags = flags;
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
        return;
    }

    ESP_LOGI(TAG,
             "Short HID report len=%u off=%u mod=0x%02X key=0x%02X",
             (unsigned)len,
             (unsigned)short_off,
             modifier,
             keycode);
    dispatch_keycode(modifier, keycode);
}

static int gatt_disc_chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                               const struct ble_gatt_chr *chr, void *arg) {
    if (error && error->status != 0) return 0;
    if (chr == NULL) return 0;

    if (chr->uuid.u.type == BLE_UUID_TYPE_16 &&
        chr->uuid.u16.value == 0x2A4D &&
        (chr->properties & BLE_GATT_CHR_F_NOTIFY)) {

        uint16_t cccd_handles[] = {(uint16_t)(chr->val_handle + 1), (uint16_t)(chr->val_handle + 2)};

        for (int i = 0; i < 2; i++) {
            uint8_t cccd_data[2] = {0x01, 0x00};
            int rc = ble_gattc_write_flat(conn_handle, cccd_handles[i],
                                          cccd_data, 2, NULL, NULL);
            if (rc == 0) {
                hid_configured = true;
                break;
            }
        }
    }
    return 0;
}

static int gatt_disc_svc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                              const struct ble_gatt_svc *svc, void *arg) {
    if (error && error->status != 0) return 0;
    if (svc == NULL) return 0;

    if (svc->uuid.u.type == BLE_UUID_TYPE_16 &&
        svc->uuid.u16.value == 0x1812) {
        ble_gattc_disc_all_chrs(conn_handle, svc->start_handle,
                                          svc->end_handle, gatt_disc_chr_cb, NULL);
    }
    return 0;
}

static int gap_event_cb(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                conn_handle = event->connect.conn_handle;
                memset(s_prev_keys, 0, sizeof(s_prev_keys));
                s_prev_short_flags = 0;
                scan_active = false;
                hid_configured = false;
                manual_scan_mode = false;
                if (pending_connect_name[0]) {
                    snprintf(connected_name, sizeof(connected_name), "%s", pending_connect_name);
                } else if (paired_name[0]) {
                    snprintf(connected_name, sizeof(connected_name), "%s", paired_name);
                } else {
                    connected_name[0] = '\0';
                }
                bump_scan_generation();

                int rc = ble_gap_security_initiate(conn_handle);
                if (rc != 0) {
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    ble_gattc_disc_all_svcs(conn_handle, gatt_disc_svc_cb, NULL);
                }
            } else {
                conn_handle = 0xFFFF;
                connected_name[0] = '\0';
                if (auto_reconnect && has_paired_prefix) {
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    ble_hid_start_scan();
                }
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            conn_handle = 0xFFFF;
            hid_configured = false;
            memset(s_prev_keys, 0, sizeof(s_prev_keys));
            s_prev_short_flags = 0;
            connected_name[0] = '\0';
            bump_scan_generation();
            if (auto_reconnect && has_paired_prefix) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                ble_hid_start_scan();
            }
            break;

        case BLE_GAP_EVENT_DISC:
            {
                char adv_name[BLE_HID_NAME_MAX] = {0};
                parse_adv_name(event->disc.data, event->disc.length_data, adv_name, sizeof(adv_name));
                const bool is_hid_keyboard = adv_looks_like_hid_keyboard(
                    event->disc.data,
                    event->disc.length_data,
                    adv_name[0] ? adv_name : NULL);

                if (manual_scan_mode && is_hid_keyboard) {
                    add_or_update_scan_result(&event->disc.addr, adv_name[0] ? adv_name : NULL);
                }

                if (has_paired_prefix && is_hid_keyboard && addr_matches_prefix(&event->disc.addr)) {
                    scan_active = false;
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
                    ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &addr, 30000, NULL, gap_event_cb, NULL);
                }
            }
            break;

        case BLE_GAP_EVENT_DISC_COMPLETE:
            scan_active = false;
            if (auto_reconnect && has_paired_prefix && conn_handle == 0xFFFF) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                ble_hid_start_scan();
            }
            bump_scan_generation();
            break;

        case BLE_GAP_EVENT_NOTIFY_RX:
            if (hid_configured) {
                handle_kbd_report(event->notify_rx.om->om_data,
                                  event->notify_rx.om->om_len);
            }
            break;

        case BLE_GAP_EVENT_ENC_CHANGE:
            if (event->enc_change.status == 0) {
                ble_gattc_disc_all_svcs(conn_handle, gatt_disc_svc_cb, NULL);
            } else {
                vTaskDelay(pdMS_TO_TICKS(2000));
                ble_gattc_disc_all_svcs(conn_handle, gatt_disc_svc_cb, NULL);
            }
            break;
    }
    return 0;
}

void ble_hid_start_scan(void) {
    if (scan_active || conn_handle != 0xFFFF) return;

    struct ble_gap_disc_params disc_params = {};
    disc_params.itvl = 160;           // 100ms scan interval (0.625ms units)
    disc_params.window = 40;          // 25ms scan window
    disc_params.filter_duplicates = 1;
    disc_params.passive = 1;          // passive scan to reduce interference

    if (ble_gap_disc(BLE_OWN_ADDR_PUBLIC, 30000, &disc_params,
                      gap_event_cb, NULL) == 0) {
        scan_active = true;
        bump_scan_generation();
    }
}

static void nimble_sync_cb(void) {
    load_paired_from_nvs();
    auto_reconnect = has_paired_prefix;
    if (has_paired_prefix) {
        if (connect_paired_now() != ESP_OK) {
            ble_hid_start_scan();
        }
    }
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
    memset(s_prev_keys, 0, sizeof(s_prev_keys));
    s_prev_short_flags = 0;
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
    if (has_paired_prefix) return;
    manual_scan_mode = true;
    auto_reconnect = false;
    clear_scan_results();
    if (scan_active) {
        ble_gap_disc_cancel();
        scan_active = false;
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

    int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &addr, 30000, NULL, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_connect failed: %d", rc);
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

    nimble_port_init();
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("DumbESPty");

    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sync_cb = nimble_sync_cb;

    nimble_port_freertos_init(nimble_host_task);
    return ESP_OK;
}

void ble_hid_set_label(lv_obj_t *label) {
    status_label = label;
    lv_obj_set_scrollbar_mode(lv_scr_act(), LV_SCROLLBAR_MODE_OFF);
}
