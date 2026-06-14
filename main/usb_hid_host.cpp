#include <string.h>
#include "usb_hid_host.hpp"
#include "shell.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_intr_alloc.h"

#if __has_include("usb/usb_host.h") && __has_include("usb/hid_host.h")
#define USB_HID_OTG_AVAILABLE 1
#include "usb/usb_host.h"
#include "usb/hid_host.h"
#else
#define USB_HID_OTG_AVAILABLE 0
#endif

#define TAG "USB_HID"

#if USB_HID_OTG_AVAILABLE

typedef struct {
    hid_host_device_handle_t handle;
    hid_host_driver_event_t event;
    void *arg;
} usb_hid_driver_evt_t;

static QueueHandle_t s_driver_evt_queue = NULL;
static QueueHandle_t s_key_queue = NULL;
static bool s_initialized = false;
static bool s_connected = false;
static uint8_t s_prev_keys[6] = {0};

static const char hid_to_ascii[] = {
     0,  0,  0,  0, 'a','b','c','d','e','f','g','h','i','j','k','l',
    'm','n','o','p','q','r','s','t','u','v','w','x','y','z',
    '1','2','3','4','5','6','7','8','9','0',
    '\r',0x1B,'\b','\t',' ','-','=','[',']','\\',0,';','\'','`',',','.','/'
};

static const char shift_to[] = "!@#$%^&*()_+{}|:\"<>?~";

static void queue_key(char c) {
    if (!s_key_queue) return;
    xQueueSend(s_key_queue, &c, 0);
}

static char map_shifted(char c) {
    const char unsorted[] = "1234567890-=[]\\;',./`";
    const char *p = strchr(unsorted, c);
    if (p) return shift_to[p - unsorted];
    return c;
}

static bool dispatch_keycode(uint8_t modifier, uint8_t keycode) {
    const bool shift = (modifier & 0x22) != 0;
    const bool ctrl = (modifier & 0x11) != 0;

    if (ctrl && shift && keycode == 0x52) { queue_key((char)0x80); return true; }
    if (ctrl && shift && keycode == 0x51) { queue_key((char)0x81); return true; }

    if (keycode == 0x52) { queue_key(0x1E); return true; }
    if (keycode == 0x51) { queue_key(0x1F); return true; }
    if (keycode == 0x4F) { queue_key(0x1D); return true; }
    if (keycode == 0x50) { queue_key(0x1C); return true; }
    if (keycode == 0x4C) { queue_key(0x7F); return true; }
    if (keycode == 0x54) { queue_key('/'); return true; }
    if (keycode == 0x58) { queue_key('\r'); return true; }

    if (keycode >= sizeof(hid_to_ascii)) return false;

    char c = hid_to_ascii[keycode];
    if (c == 0) return false;

    if (ctrl) {
        if (c >= 'a' && c <= 'z') {
            queue_key((char)(c - 'a' + 1));
            return true;
        }
        if (c == '[') { queue_key(0x1B); return true; }
        if (c == '\\') { queue_key(0x1C); return true; }
        if (c == ']') { queue_key(0x1D); return true; }
        if (c == '6') { queue_key(0x1E); return true; }
        if (c == '-') { queue_key(0x1F); return true; }
        if (c == '/') { queue_key(0x1F); return true; }
    }

    if (shift) {
        if (c >= 'a' && c <= 'z') {
            c -= 32;
        } else {
            c = map_shifted(c);
        }
    }

    queue_key(c);
    return true;
}

static void parse_boot_keyboard_report(const uint8_t *data, size_t len) {
    if (!data || len < 8) return;

    const uint8_t modifier = data[0];
    const uint8_t *cur_keys = &data[2];

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
        dispatch_keycode(modifier, keycode);
    }

    memcpy(s_prev_keys, cur_keys, sizeof(s_prev_keys));
}

static void hid_interface_cb(hid_host_device_handle_t hid_device_handle,
                             const hid_host_interface_event_t event,
                             void *arg) {
    (void)arg;
    uint8_t data[64] = {0};
    size_t data_len = 0;

    if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
        if (hid_host_device_get_raw_input_report_data(hid_device_handle,
                                                      data,
                                                      sizeof(data),
                                                      &data_len) == ESP_OK) {
            parse_boot_keyboard_report(data, data_len);
        }
        return;
    }

    if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
        s_connected = false;
        memset(s_prev_keys, 0, sizeof(s_prev_keys));
        hid_host_device_close(hid_device_handle);
    }
}

static void hid_driver_cb(hid_host_device_handle_t hid_device_handle,
                          const hid_host_driver_event_t event,
                          void *arg) {
    (void)arg;
    if (!s_driver_evt_queue) return;
    usb_hid_driver_evt_t evt;
    evt.handle = hid_device_handle;
    evt.event = event;
    evt.arg = NULL;
    xQueueSend(s_driver_evt_queue, &evt, 0);
}

static void handle_driver_event(const usb_hid_driver_evt_t *evt) {
    if (!evt) return;

    hid_host_dev_params_t dev_params;
    if (hid_host_device_get_params(evt->handle, &dev_params) != ESP_OK) {
        return;
    }

    if (evt->event != HID_HOST_DRIVER_EVENT_CONNECTED) return;
    if (dev_params.proto != HID_PROTOCOL_KEYBOARD) return;

    hid_host_device_config_t dev_config = {};
    dev_config.callback = hid_interface_cb;
    dev_config.callback_arg = NULL;

    if (hid_host_device_open(evt->handle, &dev_config) != ESP_OK) {
        ESP_LOGW(TAG, "keyboard open failed");
        return;
    }

    if (dev_params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
        hid_class_request_set_protocol(evt->handle, HID_REPORT_PROTOCOL_BOOT);
        hid_class_request_set_idle(evt->handle, 0, 0);
    }

    if (hid_host_device_start(evt->handle) == ESP_OK) {
        s_connected = true;
        memset(s_prev_keys, 0, sizeof(s_prev_keys));
        ESP_LOGI(TAG, "wired keyboard connected");
    } else {
        hid_host_device_close(evt->handle);
        ESP_LOGW(TAG, "keyboard start failed");
    }
}

static void usb_lib_task(void *arg) {
    TaskHandle_t init_waiter = (TaskHandle_t)arg;

    usb_host_config_t host_config = {};
    host_config.skip_phy_setup = false;
    host_config.intr_flags = ESP_INTR_FLAG_LOWMED;

    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        if (init_waiter) xTaskNotifyGive(init_waiter);
        vTaskDelete(NULL);
        return;
    }

    if (init_waiter) xTaskNotifyGive(init_waiter);

    while (true) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

esp_err_t usb_hid_host_init(void) {
    if (s_initialized) return ESP_OK;

    s_driver_evt_queue = xQueueCreate(8, sizeof(usb_hid_driver_evt_t));
    s_key_queue = xQueueCreate(32, sizeof(char));
    if (!s_driver_evt_queue || !s_key_queue) return ESP_ERR_NO_MEM;

    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    if (xTaskCreate(usb_lib_task, "usb_lib", 4096, self, 4, NULL) != pdPASS) {
        return ESP_FAIL;
    }

    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)) == 0) {
        ESP_LOGE(TAG, "usb host init timeout");
        return ESP_ERR_TIMEOUT;
    }

    hid_host_driver_config_t driver_cfg = {};
    driver_cfg.create_background_task = true;
    driver_cfg.task_priority = 5;
    driver_cfg.stack_size = 4096;
    driver_cfg.core_id = 0;
    driver_cfg.callback = hid_driver_cb;
    driver_cfg.callback_arg = NULL;

    esp_err_t err = hid_host_install(&driver_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hid_host_install failed: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "USB HID host ready");
    return ESP_OK;
}

void usb_hid_process_queue(void) {
    if (!s_initialized) return;

    usb_hid_driver_evt_t evt;
    while (xQueueReceive(s_driver_evt_queue, &evt, 0) == pdTRUE) {
        handle_driver_event(&evt);
    }

    char c = 0;
    while (xQueueReceive(s_key_queue, &c, 0) == pdTRUE) {
        shell_handle_key(c);
    }
}

bool usb_hid_is_connected(void) {
    return s_connected;
}

#else

esp_err_t usb_hid_host_init(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

void usb_hid_process_queue(void) {}

bool usb_hid_is_connected(void) {
    return false;
}

#endif
