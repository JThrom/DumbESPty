#ifndef USB_HID_HOST_HPP
#define USB_HID_HOST_HPP

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t usb_hid_host_init(void);
void usb_hid_process_queue(void);
bool usb_hid_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif
