/* Stubs for cross-module firmware symbols referenced by the sources under test
 * but whose own modules are not compiled into the test binary.
 *
 * Several are observable from tests:
 *   - shell_print appends to a capture buffer (mock_shell_output / reset).
 *   - shell_handle_key records dispatched keys (mock_shell_keys / reset).
 * USB host / HID host driver calls are no-op success; the keymap LOGIC in
 * usb_hid_host.cpp (dispatch_keycode / parse_boot_keyboard_report) is what we
 * test, not the driver plumbing. */
#include "esp_err.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "usb/usb_host.h"
#include "usb/hid_host.h"

/* ----------------------- shell capture surface --------------------- */
static std::string g_shell_output;
static std::vector<char> g_shell_keys;

extern "C" void shell_print(const char *text) {
    if (text) g_shell_output += text;
}
extern "C" void shell_handle_key(char c) { g_shell_keys.push_back(c); }
extern "C" void shell_pump_ui(void) {}

/* test helpers (declared in test_support.hpp) */
const char *mock_shell_output(void) { return g_shell_output.c_str(); }
void mock_shell_reset(void) { g_shell_output.clear(); g_shell_keys.clear(); }
const std::vector<char> &mock_shell_keys(void) { return g_shell_keys; }

/* ----------------------- wifi_mgr cross-call ----------------------- */
extern "C" esp_err_t wifi_mgr_apply_hostname(const char *) { return ESP_OK; }

/* ----------------------- USB host driver stubs --------------------- */
extern "C" esp_err_t usb_host_install(const usb_host_config_t *) { return ESP_OK; }
extern "C" esp_err_t usb_host_lib_handle_events(uint32_t, uint32_t *flags) {
    if (flags) *flags = 0;
    return ESP_OK;
}
extern "C" esp_err_t usb_host_device_free_all(void) { return ESP_OK; }

extern "C" esp_err_t hid_host_install(const hid_host_driver_config_t *) { return ESP_OK; }
extern "C" esp_err_t hid_host_device_get_params(hid_host_device_handle_t, hid_host_dev_params_t *p) {
    if (p) memset(p, 0, sizeof(*p));
    return ESP_OK;
}
extern "C" esp_err_t hid_host_device_open(hid_host_device_handle_t, const hid_host_device_config_t *) { return ESP_OK; }
extern "C" esp_err_t hid_host_device_close(hid_host_device_handle_t) { return ESP_OK; }
extern "C" esp_err_t hid_host_device_start(hid_host_device_handle_t) { return ESP_OK; }
extern "C" esp_err_t hid_host_device_get_raw_input_report_data(hid_host_device_handle_t,
                                                               uint8_t *, size_t, size_t *out_len) {
    if (out_len) *out_len = 0;
    return ESP_OK;
}
extern "C" esp_err_t hid_class_request_set_protocol(hid_host_device_handle_t, hid_report_protocol_t) { return ESP_OK; }
extern "C" esp_err_t hid_class_request_set_idle(hid_host_device_handle_t, uint8_t, uint8_t) { return ESP_OK; }
