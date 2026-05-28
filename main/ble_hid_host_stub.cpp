#include "ble_hid_host.hpp"

#include "esp_log.h"

static const char *TAG = "BLE_HID_STUB";

extern "C" esp_err_t ble_hid_host_init(void) {
    ESP_LOGI(TAG, "BLE HID host not implemented for esp32p4 target yet");
    return ESP_ERR_NOT_SUPPORTED;
}

extern "C" void ble_hid_start_scan(void) {}
extern "C" void ble_hid_set_label(lv_obj_t *label) { (void)label; }
extern "C" void ble_hid_process_queue(void) {}
extern "C" void ble_hid_host_disconnect(void) {}
extern "C" void ble_hid_forget_device(void) {}
extern "C" void ble_hid_scan_for_keyboards(void) {}
extern "C" esp_err_t ble_hid_pair_scan_index(int index) {
    (void)index;
    return ESP_ERR_NOT_SUPPORTED;
}
extern "C" void ble_hid_pause_scan(void) {}
extern "C" void ble_hid_resume_scan(void) {}
extern "C" bool ble_hid_is_connected(void) { return false; }
extern "C" bool ble_hid_is_scanning(void) { return false; }
extern "C" bool ble_hid_is_connecting(void) { return false; }
extern "C" bool ble_hid_is_ready(void) { return false; }
extern "C" bool ble_hid_has_paired_device(void) { return false; }
extern "C" int ble_hid_get_paired_slot(void) { return -1; }
extern "C" int ble_hid_get_scan_results(ble_hid_scan_result_t *out, int max_results) {
    (void)out;
    (void)max_results;
    return 0;
}
extern "C" uint32_t ble_hid_get_scan_generation(void) { return 0; }
extern "C" void ble_hid_get_connected_name(char *buf, size_t len) {
    if (!buf || len == 0) return;
    buf[0] = '\0';
}
