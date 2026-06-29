/* Host test mock of usb/usb_host.h.
 * Presence of this header (via __has_include) is what enables the USB HID
 * keymap code path in usb_hid_host.cpp so it can be unit tested. */
#ifndef MOCK_USB_HOST_H
#define MOCK_USB_HOST_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS    (1 << 0)
#define USB_HOST_LIB_EVENT_FLAGS_ALL_FREE      (1 << 1)

typedef struct {
    bool skip_phy_setup;
    int intr_flags;
} usb_host_config_t;

esp_err_t usb_host_install(const usb_host_config_t *config);
esp_err_t usb_host_lib_handle_events(uint32_t timeout, uint32_t *event_flags);
esp_err_t usb_host_device_free_all(void);

#ifdef __cplusplus
}
#endif

#endif
