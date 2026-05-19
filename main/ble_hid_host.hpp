#ifndef BLE_HID_HOST_HPP
#define BLE_HID_HOST_HPP

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "lvgl.h"
#include <stddef.h>

#define BLE_HID_SCAN_MAX_RESULTS 12
#define BLE_HID_NAME_MAX 48

typedef struct {
    char name[BLE_HID_NAME_MAX];
    uint8_t addr[6];
    uint8_t addr_type;
} ble_hid_scan_result_t;

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ble_hid_host_init(void);
void ble_hid_start_scan(void);
void ble_hid_set_label(lv_obj_t *label);
void ble_hid_process_queue(void);
void ble_hid_host_disconnect(void);
void ble_hid_forget_device(void);
void ble_hid_scan_for_keyboards(void);
esp_err_t ble_hid_pair_scan_index(int index);
void ble_hid_pause_scan(void);
void ble_hid_resume_scan(void);
bool ble_hid_is_connected(void);
bool ble_hid_is_scanning(void);
bool ble_hid_is_connecting(void);
bool ble_hid_is_ready(void);
bool ble_hid_has_paired_device(void);
int ble_hid_get_paired_slot(void);
int ble_hid_get_scan_results(ble_hid_scan_result_t *out, int max_results);
uint32_t ble_hid_get_scan_generation(void);
void ble_hid_get_connected_name(char *buf, size_t len);


#ifdef __cplusplus
}
#endif

#endif
