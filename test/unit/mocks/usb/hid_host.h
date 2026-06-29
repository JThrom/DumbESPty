/* Host test mock of usb/hid_host.h. Models the type/enum surface used by
 * usb_hid_host.cpp; functions are stubbed in stubs_firmware.cpp. */
#ifndef MOCK_USB_HID_HOST_H
#define MOCK_USB_HID_HOST_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *hid_host_device_handle_t;

typedef enum {
    HID_HOST_DRIVER_EVENT_CONNECTED = 0,
} hid_host_driver_event_t;

typedef enum {
    HID_HOST_INTERFACE_EVENT_INPUT_REPORT = 0,
    HID_HOST_INTERFACE_EVENT_DISCONNECTED = 1,
} hid_host_interface_event_t;

typedef enum {
    HID_PROTOCOL_NONE = 0,
    HID_PROTOCOL_KEYBOARD = 1,
    HID_PROTOCOL_MOUSE = 2,
} hid_protocol_t;

typedef enum {
    HID_SUBCLASS_NO_SUBCLASS = 0,
    HID_SUBCLASS_BOOT_INTERFACE = 1,
} hid_subclass_t;

typedef enum {
    HID_REPORT_PROTOCOL_BOOT = 0,
    HID_REPORT_PROTOCOL_REPORT = 1,
} hid_report_protocol_t;

typedef struct {
    uint8_t proto;
    uint8_t sub_class;
    uint8_t addr;
    uint8_t iface_num;
} hid_host_dev_params_t;

typedef void (*hid_host_interface_callback_t)(hid_host_device_handle_t, const hid_host_interface_event_t, void *);
typedef void (*hid_host_driver_callback_t)(hid_host_device_handle_t, const hid_host_driver_event_t, void *);

typedef struct {
    hid_host_interface_callback_t callback;
    void *callback_arg;
} hid_host_device_config_t;

typedef struct {
    bool create_background_task;
    int task_priority;
    int stack_size;
    int core_id;
    hid_host_driver_callback_t callback;
    void *callback_arg;
} hid_host_driver_config_t;

esp_err_t hid_host_install(const hid_host_driver_config_t *config);
esp_err_t hid_host_device_get_params(hid_host_device_handle_t handle, hid_host_dev_params_t *params);
esp_err_t hid_host_device_open(hid_host_device_handle_t handle, const hid_host_device_config_t *config);
esp_err_t hid_host_device_close(hid_host_device_handle_t handle);
esp_err_t hid_host_device_start(hid_host_device_handle_t handle);
esp_err_t hid_host_device_get_raw_input_report_data(hid_host_device_handle_t handle,
                                                    uint8_t *data, size_t len, size_t *out_len);
esp_err_t hid_class_request_set_protocol(hid_host_device_handle_t handle, hid_report_protocol_t proto);
esp_err_t hid_class_request_set_idle(hid_host_device_handle_t handle, uint8_t duration, uint8_t report_id);

#ifdef __cplusplus
}
#endif

#endif
