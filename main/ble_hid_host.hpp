#ifndef BLE_HID_HOST_HPP
#define BLE_HID_HOST_HPP

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ble_hid_host_init(void);
void ble_hid_start_scan(void);
void ble_hid_set_label(lv_obj_t *label);
void ble_hid_process_queue(void);
void ble_hid_host_disconnect(void);
void ble_hid_pause_scan(void);
void ble_hid_resume_scan(void);


#ifdef __cplusplus
}
#endif

#endif
