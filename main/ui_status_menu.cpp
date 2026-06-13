#include "ui_status_menu.hpp"

#include <cstdio>
#include <cstring>
#include <cstdint>

#include "ble_hid_host.hpp"
#include "ch422g_init.hpp"
#include "driver/i2c_master.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "tailscale_mgr.hpp"
#include "wifi_mgr.hpp"

static const char *TAG = "ui_menu";

#if CONFIG_IDF_TARGET_ESP32P4
static constexpr int SCREEN_W = 1024;
static constexpr int SCREEN_H = 600;
#else
static constexpr int SCREEN_W = 800;
static constexpr int SCREEN_H = 480;
#endif
static constexpr int COLLAPSED_W = 20;
static constexpr int EXPANDED_W = 240;

static esp_lcd_panel_io_handle_t s_touch_io = NULL;
static esp_lcd_touch_handle_t s_touch = NULL;
static lv_indev_t *s_indev = NULL;
static i2c_master_bus_handle_t s_touch_i2c_bus = NULL;
static uint8_t s_touch_addr = 0;

static lv_obj_t *s_menu = NULL;
static lv_obj_t *s_hotzone = NULL;
static lv_obj_t *s_dismiss_layer = NULL;
static lv_obj_t *s_collapsed = NULL;
static lv_obj_t *s_expanded = NULL;
static lv_obj_t *s_icon_batt = NULL;
static lv_obj_t *s_icon_ble = NULL;
static lv_obj_t *s_icon_wifi = NULL;
static lv_obj_t *s_icon_tailscale = NULL;
static lv_obj_t *s_status_batt = NULL;
static lv_obj_t *s_status_ble = NULL;
static lv_obj_t *s_status_wifi = NULL;
static lv_obj_t *s_status_tailscale = NULL;
static lv_obj_t *s_ble_btn_scan = NULL;
static lv_obj_t *s_ble_btn_disconnect = NULL;
static lv_obj_t *s_ble_btn_scan_label = NULL;
static lv_obj_t *s_ble_btn_disconnect_label = NULL;
static lv_obj_t *s_ble_list = NULL;
static bool s_expanded_state = false;
static int64_t s_last_update_us = 0;
static int64_t s_last_touch_err_log_us = 0;
static uint32_t s_last_ble_scan_generation = 0;

static i2c_master_bus_handle_t get_touch_i2c_bus(void) {
#if CONFIG_IDF_TARGET_ESP32P4
    if (s_touch_i2c_bus) return s_touch_i2c_bus;

    struct i2c_pin_pair {
        gpio_num_t sda;
        gpio_num_t scl;
    };

    static const i2c_pin_pair candidates[] = {
        {GPIO_NUM_7, GPIO_NUM_8},
        {GPIO_NUM_8, GPIO_NUM_9},
        {GPIO_NUM_6, GPIO_NUM_7},
        {GPIO_NUM_15, GPIO_NUM_16},
        {GPIO_NUM_17, GPIO_NUM_18},
    };

    const uint8_t probe_addrs[] = {
        ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
        ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP,
    };

    esp_err_t last = ESP_FAIL;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        i2c_master_bus_config_t bus_cfg = {};
        bus_cfg.i2c_port = I2C_NUM_0;
        bus_cfg.sda_io_num = candidates[i].sda;
        bus_cfg.scl_io_num = candidates[i].scl;
        bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_cfg.glitch_ignore_cnt = 7;
        bus_cfg.flags.enable_internal_pullup = 1;

        last = i2c_new_master_bus(&bus_cfg, &s_touch_i2c_bus);
        if (last == ESP_OK) {
            bool found_touch = false;
            for (size_t j = 0; j < sizeof(probe_addrs) / sizeof(probe_addrs[0]); ++j) {
                if (i2c_master_probe(s_touch_i2c_bus, probe_addrs[j], 20) == ESP_OK) {
                    found_touch = true;
                    s_touch_addr = probe_addrs[j];
                    ESP_LOGI(TAG, "Touch I2C bus created on SDA=%d SCL=%d (GT911 ack at 0x%02X)",
                             (int)candidates[i].sda, (int)candidates[i].scl, probe_addrs[j]);
                    break;
                }
            }
            if (found_touch) {
                return s_touch_i2c_bus;
            }

            ESP_LOGW(TAG, "Touch probe failed on SDA=%d SCL=%d; trying next pin pair",
                     (int)candidates[i].sda, (int)candidates[i].scl);
            i2c_del_master_bus(s_touch_i2c_bus);
            s_touch_i2c_bus = NULL;
        }
    }

    ESP_LOGE(TAG, "touch init failed: no working I2C bus config for esp32p4 (%s)", esp_err_to_name(last));
    return NULL;
#else
    return ch422g_get_i2c_bus();
#endif
}

static lv_color_t state_color(bool connected, bool transient) {
    if (connected) return lv_color_hex(0x2ECC71);
    if (transient) return lv_color_hex(0xF1C40F);
    return lv_color_hex(0xE74C3C);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    if (!s_touch) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    esp_err_t ret = esp_lcd_touch_read_data(s_touch);
    if (ret != ESP_OK) {
        int64_t now = esp_timer_get_time();
        if ((now - s_last_touch_err_log_us) > 2000000) {
            s_last_touch_err_log_us = now;
            ESP_LOGW(TAG, "touch read failed: %s", esp_err_to_name(ret));
        }
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    esp_lcd_touch_point_data_t point[1];
    uint8_t point_cnt = 0;
    ret = esp_lcd_touch_get_data(s_touch, point, &point_cnt, 1);
    if (ret == ESP_OK && point_cnt > 0) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = point[0].x;
        data->point.y = point[0].y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void menu_toggle(void) {
    s_expanded_state = !s_expanded_state;
    int width = s_expanded_state ? EXPANDED_W : COLLAPSED_W;
    lv_obj_set_width(s_menu, width);
    lv_obj_set_style_bg_opa(s_menu, s_expanded_state ? LV_OPA_80 : LV_OPA_40, 0);
    if (s_expanded_state) {
        lv_obj_add_flag(s_collapsed, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_expanded, LV_OBJ_FLAG_HIDDEN);
        if (s_hotzone) lv_obj_add_flag(s_hotzone, LV_OBJ_FLAG_HIDDEN);
        if (s_dismiss_layer) {
            lv_obj_set_size(s_dismiss_layer, SCREEN_W - EXPANDED_W, SCREEN_H);
            lv_obj_align(s_dismiss_layer, LV_ALIGN_LEFT_MID, 0, 0);
        }
        if (s_dismiss_layer) lv_obj_clear_flag(s_dismiss_layer, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_collapsed, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_expanded, LV_OBJ_FLAG_HIDDEN);
        if (s_hotzone) lv_obj_clear_flag(s_hotzone, LV_OBJ_FLAG_HIDDEN);
        if (s_dismiss_layer) {
            lv_obj_set_size(s_dismiss_layer, SCREEN_W, SCREEN_H);
            lv_obj_align(s_dismiss_layer, LV_ALIGN_CENTER, 0, 0);
        }
        if (s_dismiss_layer) lv_obj_add_flag(s_dismiss_layer, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_dismiss_layer && s_expanded_state) {
        lv_obj_move_foreground(s_dismiss_layer);
    }
    lv_obj_align(s_menu, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_move_foreground(s_menu);
}

static void menu_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        lv_obj_t *target = (lv_obj_t *)lv_event_get_target(e);
        if (!s_expanded_state && target == s_hotzone) {
            menu_toggle();
        }
    }
}

static void dismiss_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && s_expanded_state) {
        menu_toggle();
    }
}

static void ble_scan_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ble_hid_scan_for_keyboards();
    }
}

static void ble_disconnect_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ble_hid_forget_device();
    }
}

static void ble_pick_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx < 0) return;
    ble_hid_pair_scan_index((int)idx);
}

static esp_err_t touch_init_internal(void) {
    i2c_master_bus_handle_t bus = get_touch_i2c_bus();
    if (!bus) {
        ESP_LOGE(TAG, "touch init failed: no I2C bus");
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t primary_addr = s_touch_addr ? s_touch_addr : (uint8_t)ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
    const uint8_t secondary_addr =
        (primary_addr == (uint8_t)ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP)
            ? (uint8_t)ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS
            : (uint8_t)ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
    const uint8_t probe_addrs[] = {primary_addr, secondary_addr};

    esp_err_t ret = ESP_FAIL;
    for (size_t i = 0; i < sizeof(probe_addrs) / sizeof(probe_addrs[0]); i++) {
        esp_lcd_panel_io_i2c_config_t io_cfg = {};
        io_cfg.dev_addr = probe_addrs[i];
        io_cfg.scl_speed_hz = 100000;
        io_cfg.control_phase_bytes = 1;
        io_cfg.dc_bit_offset = 0;
        io_cfg.lcd_cmd_bits = 16;
        io_cfg.lcd_param_bits = 8;
        io_cfg.flags.disable_control_phase = 1;

        ret = esp_lcd_new_panel_io_i2c(bus, &io_cfg, &s_touch_io);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "touch IO init failed on 0x%02X: %s", probe_addrs[i], esp_err_to_name(ret));
            continue;
        }

        esp_lcd_touch_io_gt911_config_t gt911_cfg = {
            .dev_addr = probe_addrs[i],
        };

        esp_lcd_touch_config_t touch_cfg = {};
        touch_cfg.x_max = SCREEN_W;
        touch_cfg.y_max = SCREEN_H;
        touch_cfg.rst_gpio_num = GPIO_NUM_NC;
        touch_cfg.int_gpio_num = GPIO_NUM_NC;
        touch_cfg.levels.reset = 0;
        touch_cfg.levels.interrupt = 0;
        touch_cfg.flags.swap_xy = 0;
        touch_cfg.flags.mirror_x = 0;
        touch_cfg.flags.mirror_y = 0;
        touch_cfg.driver_data = &gt911_cfg;

        ret = esp_lcd_touch_new_i2c_gt911(s_touch_io, &touch_cfg, &s_touch);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "GT911 init ok on addr 0x%02X", probe_addrs[i]);
            break;
        }

        ESP_LOGW(TAG, "GT911 init failed on 0x%02X: %s", probe_addrs[i], esp_err_to_name(ret));
        esp_lcd_panel_io_del(s_touch_io);
        s_touch_io = NULL;
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GT911 init failed on all addresses");
        return ret;
    }

    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, touch_read_cb);
    ESP_LOGI(TAG, "Touch input ready");
    return ESP_OK;
}

esp_err_t ui_status_menu_init(lv_obj_t *parent) {
    esp_err_t ret = touch_init_internal();
    if (ret != ESP_OK) {
        return ret;
    }

    s_menu = lv_obj_create(parent);
    lv_obj_set_size(s_menu, COLLAPSED_W, SCREEN_H);
    lv_obj_align(s_menu, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(s_menu, 0, 0);
    lv_obj_set_style_border_width(s_menu, 0, 0);
    lv_obj_set_style_pad_all(s_menu, 0, 0);
    lv_obj_set_style_bg_color(s_menu, lv_color_hex(0x101726), 0);
    lv_obj_set_style_bg_opa(s_menu, LV_OPA_40, 0);
    lv_obj_clear_flag(s_menu, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_menu);

    s_dismiss_layer = lv_obj_create(parent);
    lv_obj_set_size(s_dismiss_layer, SCREEN_W, SCREEN_H);
    lv_obj_align(s_dismiss_layer, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(s_dismiss_layer, 0, 0);
    lv_obj_set_style_border_width(s_dismiss_layer, 0, 0);
    lv_obj_set_style_bg_opa(s_dismiss_layer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_dismiss_layer, 0, 0);
    lv_obj_clear_flag(s_dismiss_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_dismiss_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_dismiss_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_dismiss_layer, dismiss_event_cb, LV_EVENT_CLICKED, NULL);

    s_hotzone = lv_obj_create(parent);
    lv_obj_set_size(s_hotzone, 120, 120);
    lv_obj_align(s_hotzone, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_radius(s_hotzone, 0, 0);
    lv_obj_set_style_border_width(s_hotzone, 0, 0);
    lv_obj_set_style_bg_opa(s_hotzone, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_hotzone, 0, 0);
    lv_obj_clear_flag(s_hotzone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_hotzone, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_hotzone, menu_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_move_foreground(s_hotzone);

    s_collapsed = lv_obj_create(s_menu);
    lv_obj_set_size(s_collapsed, COLLAPSED_W, SCREEN_H);
    lv_obj_set_style_bg_opa(s_collapsed, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_collapsed, 0, 0);
    lv_obj_set_style_pad_all(s_collapsed, 0, 0);
    lv_obj_clear_flag(s_collapsed, LV_OBJ_FLAG_SCROLLABLE);

    s_icon_batt = lv_label_create(s_collapsed);
    lv_label_set_text(s_icon_batt, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_style_text_font(s_icon_batt, lv_font_get_default(), 0);
    lv_obj_set_style_text_color(s_icon_batt, lv_color_hex(0xE74C3C), 0);
    lv_obj_align(s_icon_batt, LV_ALIGN_TOP_RIGHT, -2, 52);

    s_icon_ble = lv_label_create(s_collapsed);
    lv_label_set_text(s_icon_ble, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(s_icon_ble, lv_font_get_default(), 0);
    lv_obj_set_style_text_color(s_icon_ble, lv_color_hex(0xE74C3C), 0);
    lv_obj_align(s_icon_ble, LV_ALIGN_TOP_MID, 0, 52);

    s_icon_wifi = lv_label_create(s_collapsed);
    lv_label_set_text(s_icon_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(s_icon_wifi, lv_font_get_default(), 0);
    lv_obj_set_style_text_color(s_icon_wifi, lv_color_hex(0xE74C3C), 0);
    lv_obj_align(s_icon_wifi, LV_ALIGN_TOP_MID, 0, 8);

    s_icon_tailscale = lv_label_create(s_collapsed);
    lv_label_set_text(s_icon_tailscale, "TS");
    lv_obj_set_style_text_font(s_icon_tailscale, lv_font_get_default(), 0);
    lv_obj_set_style_text_color(s_icon_tailscale, lv_color_hex(0xE74C3C), 0);
    lv_obj_align(s_icon_tailscale, LV_ALIGN_TOP_MID, 0, 30);

    s_expanded = lv_obj_create(s_menu);
    lv_obj_set_size(s_expanded, EXPANDED_W, SCREEN_H);
    lv_obj_set_style_bg_opa(s_expanded, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_expanded, 0, 0);
    lv_obj_set_style_pad_left(s_expanded, 12, 0);
    lv_obj_set_style_pad_right(s_expanded, 12, 0);
    lv_obj_set_style_pad_top(s_expanded, 18, 0);
    lv_obj_set_style_pad_bottom(s_expanded, 8, 0);
    lv_obj_clear_flag(s_expanded, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_expanded, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = lv_label_create(s_expanded);
    lv_label_set_text(title, "STATUS");
    lv_obj_set_style_text_color(title, lv_color_hex(0xECF0F1), 0);
    lv_obj_set_style_text_font(title, lv_font_get_default(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_status_batt = lv_label_create(s_expanded);
    lv_label_set_text(s_status_batt, "Battery: unavailable");
    lv_obj_set_style_text_color(s_status_batt, lv_color_hex(0xE74C3C), 0);
    lv_obj_align(s_status_batt, LV_ALIGN_TOP_LEFT, 0, 34);

    s_status_ble = lv_label_create(s_expanded);
    lv_label_set_text(s_status_ble, "BLE HID: init");
    lv_obj_set_style_text_color(s_status_ble, lv_color_hex(0xE74C3C), 0);
    lv_obj_align(s_status_ble, LV_ALIGN_TOP_LEFT, 0, 106);

    s_status_tailscale = lv_label_create(s_expanded);
    lv_label_set_text(s_status_tailscale, "Tailscale: init");
    lv_obj_set_style_text_color(s_status_tailscale, lv_color_hex(0xE74C3C), 0);
    lv_obj_align(s_status_tailscale, LV_ALIGN_TOP_LEFT, 0, 82);

    s_status_wifi = lv_label_create(s_expanded);
    lv_label_set_text(s_status_wifi, "WiFi: init");
    lv_obj_set_style_text_color(s_status_wifi, lv_color_hex(0xE74C3C), 0);
    lv_obj_align(s_status_wifi, LV_ALIGN_TOP_LEFT, 0, 58);

    s_ble_btn_scan = lv_button_create(s_expanded);
    lv_obj_set_size(s_ble_btn_scan, EXPANDED_W - 24, 28);
    lv_obj_align(s_ble_btn_scan, LV_ALIGN_TOP_LEFT, 0, 132);
    lv_obj_set_style_bg_color(s_ble_btn_scan, lv_color_hex(0x1F3A5F), 0);
    lv_obj_set_style_bg_opa(s_ble_btn_scan, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ble_btn_scan, 0, 0);
    lv_obj_add_event_cb(s_ble_btn_scan, ble_scan_event_cb, LV_EVENT_CLICKED, NULL);
    s_ble_btn_scan_label = lv_label_create(s_ble_btn_scan);
    lv_label_set_text(s_ble_btn_scan_label, "Scan");
    lv_obj_set_style_text_color(s_ble_btn_scan_label, lv_color_hex(0xECF0F1), 0);
    lv_obj_center(s_ble_btn_scan_label);

    s_ble_btn_disconnect = lv_button_create(s_expanded);
    lv_obj_set_size(s_ble_btn_disconnect, EXPANDED_W - 24, 28);
    lv_obj_align(s_ble_btn_disconnect, LV_ALIGN_TOP_LEFT, 0, 132);
    lv_obj_set_style_bg_color(s_ble_btn_disconnect, lv_color_hex(0x1F3A5F), 0);
    lv_obj_set_style_bg_opa(s_ble_btn_disconnect, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ble_btn_disconnect, 0, 0);
    lv_obj_add_event_cb(s_ble_btn_disconnect, ble_disconnect_event_cb, LV_EVENT_CLICKED, NULL);
    s_ble_btn_disconnect_label = lv_label_create(s_ble_btn_disconnect);
    lv_label_set_text(s_ble_btn_disconnect_label, "Disconnect");
    lv_obj_set_style_text_color(s_ble_btn_disconnect_label, lv_color_hex(0xECF0F1), 0);
    lv_obj_center(s_ble_btn_disconnect_label);
    lv_obj_add_flag(s_ble_btn_disconnect, LV_OBJ_FLAG_HIDDEN);

    s_ble_list = lv_obj_create(s_expanded);
    lv_obj_set_size(s_ble_list, EXPANDED_W - 24, SCREEN_H - 168);
    lv_obj_align(s_ble_list, LV_ALIGN_TOP_LEFT, 0, 164);
    lv_obj_set_style_pad_all(s_ble_list, 6, 0);
    lv_obj_set_style_pad_row(s_ble_list, 8, 0);
    lv_obj_set_style_radius(s_ble_list, 4, 0);
    lv_obj_set_style_border_width(s_ble_list, 1, 0);
    lv_obj_set_style_border_color(s_ble_list, lv_color_hex(0x2C3E50), 0);
    lv_obj_set_style_bg_color(s_ble_list, lv_color_hex(0x222831), 0);
    lv_obj_set_style_bg_opa(s_ble_list, LV_OPA_COVER, 0);
    lv_obj_set_layout(s_ble_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_ble_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ble_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *empty = lv_label_create(s_ble_list);
    lv_label_set_text(empty, "No scan results");
    lv_obj_set_style_text_color(empty, lv_color_hex(0x95A5A6), 0);

    s_last_update_us = 0;
    lv_obj_add_flag(s_icon_batt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_status_batt, LV_OBJ_FLAG_HIDDEN);
    ui_status_menu_update();
    ESP_LOGI(TAG, "Status menu ready (collapsed)");
    return ESP_OK;
}

void ui_status_menu_update(void) {
    if (!s_menu) return;

    int64_t now = esp_timer_get_time();
    if (s_last_update_us != 0 && (now - s_last_update_us) < 200000) return;
    s_last_update_us = now;

    const bool ble_connected = ble_hid_is_connected();
    const bool ble_connecting = ble_hid_is_connecting();
    const bool ble_ready = ble_hid_is_ready();
    const bool ble_scanning = ble_hid_is_scanning();
    const bool ble_paired = ble_hid_has_paired_device();
    const bool wifi_connected = wifi_mgr_is_connected();
    const bool wifi_transient = false;
    const bool tailscale_connected = tailscale_mgr_is_connected();
    const bool tailscale_transient = tailscale_mgr_is_connecting();
    const bool tailscale_enabled = tailscale_mgr_is_enabled();
    const bool ble_transient = ble_scanning || ble_connecting || (ble_connected && !ble_ready);

    const lv_color_t ble_color = state_color(ble_connected && ble_ready, ble_transient);
    const lv_color_t wifi_color = state_color(wifi_connected, wifi_transient);
    const lv_color_t tailscale_color = state_color(tailscale_connected, tailscale_enabled && tailscale_transient);
    const lv_color_t batt_color = lv_color_hex(0xF1C40F);

    lv_obj_set_style_text_color(s_icon_batt, batt_color, 0);
    lv_obj_set_style_text_color(s_icon_ble, ble_color, 0);
    lv_obj_set_style_text_color(s_icon_wifi, wifi_color, 0);
    lv_obj_set_style_text_color(s_icon_tailscale, tailscale_color, 0);

    char line[128];
    char ble_name[BLE_HID_NAME_MAX] = {0};
    ble_hid_get_connected_name(ble_name, sizeof(ble_name));
    if (ble_connected) {
        snprintf(line, sizeof(line), "BLE HID: %s", ble_name[0] ? ble_name : "keyboard");
    } else if (!ble_paired) {
        snprintf(line, sizeof(line), "BLE HID: unpaired");
    } else {
        snprintf(line, sizeof(line), "BLE HID: %s (%s)", ble_name[0] ? ble_name : "keyboard",
                 ble_scanning ? "scanning" : (ble_connecting ? "connecting" : "disconnected"));
    }
    lv_label_set_text(s_status_ble, line);
    lv_obj_set_style_text_color(s_status_ble, ble_color, 0);

    if (wifi_connected) {
        snprintf(line, sizeof(line), "WiFi: %s", wifi_mgr_get_ssid());
    } else {
        snprintf(line, sizeof(line), "WiFi: disconnected");
    }
    lv_label_set_text(s_status_wifi, line);
    lv_obj_set_style_text_color(s_status_wifi, wifi_color, 0);

    tailscale_mgr_get_status_line(line, sizeof(line));
    lv_label_set_text(s_status_tailscale, line);
    lv_obj_set_style_text_color(s_status_tailscale, tailscale_color, 0);

    lv_label_set_text(s_status_batt, "Battery: unavailable");
    lv_obj_set_style_text_color(s_status_batt, batt_color, 0);

    if (ble_paired) {
        lv_obj_add_flag(s_ble_btn_scan, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_ble_btn_disconnect, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ble_list, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_ble_btn_scan, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ble_btn_disconnect, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_ble_list, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_ble_btn_scan_label, ble_scanning ? "Scanning..." : "Scan");

        uint32_t gen = ble_hid_get_scan_generation();
        if (gen != s_last_ble_scan_generation) {
            s_last_ble_scan_generation = gen;
            lv_obj_clean(s_ble_list);

            ble_hid_scan_result_t results[BLE_HID_SCAN_MAX_RESULTS];
            const int count = ble_hid_get_scan_results(results, BLE_HID_SCAN_MAX_RESULTS);
            if (count == 0) {
                lv_obj_t *empty = lv_label_create(s_ble_list);
                lv_label_set_text(empty, ble_scanning ? "Scanning for HID keyboards..." : "Tap Scan to find keyboards");
                lv_obj_set_style_text_color(empty, lv_color_hex(0x95A5A6), 0);
            } else {
                for (int i = 0; i < count; i++) {
                    lv_obj_t *row = lv_button_create(s_ble_list);
                    lv_obj_set_width(row, lv_pct(100));
                    lv_obj_set_height(row, 34);
                    lv_obj_set_style_bg_color(row, lv_color_hex(0x2B3442), 0);
                    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
                    lv_obj_set_style_border_width(row, 0, 0);
                    lv_obj_set_style_pad_left(row, 8, 0);
                    lv_obj_set_style_pad_right(row, 8, 0);
                    lv_obj_add_event_cb(row, ble_pick_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

                    lv_obj_t *txt = lv_label_create(row);
                    if (results[i].name[0]) {
                        snprintf(line,
                                 sizeof(line),
                                 "%.40s [%02X:%02X]",
                                 results[i].name,
                                 results[i].addr[1],
                                 results[i].addr[0]);
                    } else {
                        snprintf(line,
                                 sizeof(line),
                                 "Device %d [%02X:%02X]",
                                 i + 1,
                                 results[i].addr[1],
                                 results[i].addr[0]);
                    }
                    lv_label_set_text(txt, line);
                    lv_obj_set_style_text_color(txt, lv_color_hex(0xECF0F1), 0);
                    lv_obj_set_style_text_align(txt, LV_TEXT_ALIGN_LEFT, 0);
                    lv_obj_align(txt, LV_ALIGN_LEFT_MID, 0, 0);
                }
            }
        }
    }
}
