#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#define TAG "BLE_HID"

static uint16_t conn_handle = 0xFFFF;
static bool scan_active = false;
static bool hid_configured = false;
static bool auto_reconnect = true;

static QueueHandle_t msg_queue = NULL;
static lv_obj_t *status_label = NULL;

typedef struct {
    uint8_t type;  // 0 = status line, 1 = key character
    char text[256];
} disp_msg_t;

static const char hid_to_ascii[] = {
     0,  0,  0,  0, 'a','b','c','d','e','f','g','h','i','j','k','l',
    'm','n','o','p','q','r','s','t','u','v','w','x','y','z',
    '1','2','3','4','5','6','7','8','9','0',
    '\r',0x1B,'\b','\t',' ','-','=','[',']','\\',';','\'','`',',','.','/'
};

static const char shift_to[] = "!@#$%^&*()_+{}|:\"<>?~";

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

static void handle_kbd_report(const uint8_t *data, uint16_t len) {
    if (len < 3) return;

    uint8_t modifier = data[0];
    uint8_t keycode = data[2];

    if (keycode == 0x52) { update_key(0x1E); return; } // Up
    if (keycode == 0x51) { update_key(0x1F); return; } // Down
    if (keycode == 0x4F) { update_key(0x1D); return; } // Right
    if (keycode == 0x50) { update_key(0x1C); return; } // Left
    if (keycode == 0x54) { update_key('/'); return; }  // Keypad /
    if (keycode == 0x58) { update_key('\r'); return; } // Keypad Enter

    if (keycode == 0) return;
    if (keycode >= sizeof(hid_to_ascii)) {
        ESP_LOGD(TAG, "unmapped keycode=0x%02X mod=0x%02X", keycode, modifier);
        return;
    }

    char c = hid_to_ascii[keycode];
    if (c == 0) {
        ESP_LOGD(TAG, "no-ascii keycode=0x%02X mod=0x%02X", keycode, modifier);
        return;
    }

    bool shift = modifier & 0x22;
    bool ctrl = modifier & 0x11;

    if (ctrl) {
        if (c >= 'a' && c <= 'z') {
            update_key((char)(c - 'a' + 1));
            return;
        }
        if (c == '[') { update_key(0x1B); return; }
        if (c == '\\') { update_key(0x1C); return; }
        if (c == ']') { update_key(0x1D); return; }
        if (c == '6') { update_key(0x1E); return; }
        if (c == '-') { update_key(0x1F); return; }
        if (c == '/') { update_key(0x1F); return; }
    }

    if (shift) {
        if (c >= 'a' && c <= 'z') {
            c -= 32;
        } else {
            c = map_shifted(c);
        }
    }

    update_key(c);
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
                scan_active = false;
                hid_configured = false;

                int rc = ble_gap_security_initiate(conn_handle);
                if (rc != 0) {
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    ble_gattc_disc_all_svcs(conn_handle, gatt_disc_svc_cb, NULL);
                }
            } else {
                conn_handle = 0xFFFF;
                if (auto_reconnect) {
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    ble_hid_start_scan();
                }
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            conn_handle = 0xFFFF;
            hid_configured = false;
            if (auto_reconnect) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                ble_hid_start_scan();
            }
            break;

        case BLE_GAP_EVENT_DISC:
            if (event->disc.addr.val[5] == 0x29 &&
                event->disc.addr.val[4] == 0x46 &&
                event->disc.addr.val[3] == 0x78 &&
                event->disc.addr.val[2] == 0xD0) {

                scan_active = false;
                ble_gap_disc_cancel();

                ble_addr_t addr = event->disc.addr;
                ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &addr,
                                         30000, NULL, gap_event_cb, NULL);
            }
            break;

        case BLE_GAP_EVENT_DISC_COMPLETE:
            scan_active = false;
            if (auto_reconnect) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                ble_hid_start_scan();
            }
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
    if (scan_active) return;

    struct ble_gap_disc_params disc_params = {};
    disc_params.itvl = 160;           // 100ms scan interval (0.625ms units)
    disc_params.window = 40;          // 25ms scan window
    disc_params.filter_duplicates = 1;
    disc_params.passive = 1;          // passive scan to reduce interference

    if (ble_gap_disc(BLE_OWN_ADDR_PUBLIC, 30000, &disc_params,
                      gap_event_cb, NULL) == 0) {
        scan_active = true;
    }
}

static void nimble_sync_cb(void) {
    ble_hid_start_scan();
}

static void nimble_host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_hid_host_disconnect(void) {
    auto_reconnect = false;
    if (scan_active) {
        ble_gap_disc_cancel();
        scan_active = false;
    }
    if (conn_handle != 0xFFFF) {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        conn_handle = 0xFFFF;
    }
    hid_configured = false;
}

void ble_hid_pause_scan(void) {
    auto_reconnect = false;
    if (scan_active) {
        ble_gap_disc_cancel();
    }
}

void ble_hid_resume_scan(void) {
    auto_reconnect = true;
    if (conn_handle == 0xFFFF) {
        ble_hid_start_scan();
    }
}

esp_err_t ble_hid_host_init(void) {
    msg_queue = xQueueCreate(10, sizeof(disp_msg_t));
    if (!msg_queue) return ESP_FAIL;

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
