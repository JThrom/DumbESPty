#include "ch422g_init.hpp"
#include "waveshare_display.hpp"
#include "ble_hid_host.hpp"
#include "usb_hid_host.hpp"
#include "shell.hpp"
#include "ui_status_menu.hpp"
#include "wifi_mgr.hpp"
#include "hostname_mgr.hpp"
#include "ssh_client.hpp"
#include "tailscale_mgr.hpp"
#include "terminal.hpp"
#include "power_mgr.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_crypto_lock.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "lvgl.h"

static const char *TAG = "console";

// Decode the last reset reason so USB->battery unplug reboots are attributable:
//   POWERON  -> system rail actually collapsed (hardware power-path gap)
//   BROWNOUT -> rail dipped below the detector threshold (recoverable in fw)
//   others   -> software/panic/watchdog, not a power event.
static const char *reset_reason_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "POWERON (cold start / rail collapse)";
        case ESP_RST_EXT:       return "EXT (external pin)";
        case ESP_RST_SW:        return "SW (esp_restart)";
        case ESP_RST_PANIC:     return "PANIC (exception/abort)";
        case ESP_RST_INT_WDT:   return "INT_WDT (interrupt watchdog)";
        case ESP_RST_TASK_WDT:  return "TASK_WDT (task watchdog)";
        case ESP_RST_WDT:       return "WDT (other watchdog)";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT (rail dip below detector)";
        case ESP_RST_SDIO:      return "SDIO";
        case ESP_RST_USB:       return "USB (usb-serial-jtag host reset)";
        case ESP_RST_JTAG:      return "JTAG";
        case ESP_RST_EFUSE:     return "EFUSE (efuse crc error)";
        case ESP_RST_PWR_GLITCH:return "PWR_GLITCH (PSDET voltage-glitch reset)";
        case ESP_RST_CPU_LOCKUP:return "CPU_LOCKUP (double exception)";
        default:                return "UNKNOWN";
    }
}
static lv_obj_t *status_label = NULL;
static terminal_t terminal;

// --- Idle low-power management ------------------------------------------------
// Decision logic lives in power_mgr.cpp (host-unit-testable). Here we provide
// the hardware hooks: a millisecond clock from the FreeRTOS tick and the
// Waveshare backlight driver. When idle for POWER_IDLE_TIMEOUT_MS the backlight
// is turned off and the main loop slows; any input/SSH output wakes instantly.
static uint32_t power_now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void power_install_hooks(void) {
    power_mgr_hooks_t hooks = {};
    hooks.now_ms = power_now_ms;
    hooks.get_brightness = waveshare_display_get_brightness;
    hooks.set_brightness = [](int p) { waveshare_display_set_brightness(p); };
    hooks.brightness_supported = waveshare_display_brightness_supported();
    power_mgr_init(&hooks);
}

static void warm_crypto_locks(void) {
    esp_crypto_mpi_lock_acquire();
    esp_crypto_mpi_lock_release();
#ifdef SOC_ECC_SUPPORTED
    esp_crypto_ecc_lock_acquire();
    esp_crypto_ecc_lock_release();
#endif
#ifdef SOC_ECDSA_SUPPORTED
    esp_crypto_ecdsa_lock_acquire();
    esp_crypto_ecdsa_lock_release();
#endif
    ESP_LOGI(TAG, "Crypto locks warmed");
}

static void lv_tick_task(void *arg) {
    while (1) {
        lv_tick_inc(10);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

extern "C" void app_main(void) {
    esp_err_t ret;

    // Reset-reason logging is retained permanently: it is cheap and makes any
    // future power/reset anomaly attributable. The USB->battery unplug reboot
    // was root-caused (2026-06) to a board-level power-path handover transient
    // that briefly power-cycles the P4 core rail (reset_reason=POWERON even
    // with the brownout reset disabled). See SPEC.md "Power-Path / USB->Battery
    // Handover Reboot" for the full investigation and the hardware fix. The
    // earlier TEST-ONLY brownout-disable diagnostic has been removed; brownout
    // reset protection is restored (configured via sdkconfig, threshold 2.42V).
    esp_reset_reason_t rst = esp_reset_reason();
    ESP_LOGW(TAG, "=== BOOT reset_reason=%d (%s) ===", (int)rst, reset_reason_str(rst));

    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "NVS init OK");

#if CONFIG_IDF_TARGET_ESP32S3
    ret = ch422g_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CH422G init failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "CH422G init OK");
#else
    ESP_LOGI(TAG, "Skipping CH422G init on this target");
#endif

    if (esp_psram_is_initialized())
        ESP_LOGI(TAG, "PSRAM initialized");
    else
        ESP_LOGW(TAG, "PSRAM not initialized");

    warm_crypto_locks();

    lv_init();
    ESP_LOGI(TAG, "LVGL initialized");

    if (xTaskCreate(lv_tick_task, "lv_tick", 2048, NULL, 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "LVGL tick task create failed");
    }

    lv_disp_t *disp = NULL;
    ret = waveshare_display_init(&disp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Display init failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "Display initialized");

    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);

    status_label = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_color(status_label, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(status_label, lv_font_get_default(), 0);
    lv_obj_set_pos(status_label, 10, 10);
    lv_obj_set_width(status_label, 780);
    ESP_LOGI(TAG, "Label created");

    ble_hid_set_label(status_label);

    extern const lv_font_t lv_font_term_mono_10;
    extern const lv_font_t lv_font_nerd_symbols_10;
    extern const cozette_bdf_font_t g_cozette_bdf_13;

    const int cell_w = 8;
    const int cell_h = 15;
    int cols = waveshare_display_width() / cell_w;
    int rows = waveshare_display_height() / cell_h;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (cols > TERM_MAX_COLS) cols = TERM_MAX_COLS;
    if (rows > TERM_MAX_ROWS) rows = TERM_MAX_ROWS;

    terminal_load_default_fg_index();
    terminal_init(&terminal,
                  cols,
                  rows,
                  cell_w,
                  cell_h,
                  lv_scr_act(),
                  &g_cozette_bdf_13,
                  &lv_font_term_mono_10,
                  &lv_font_nerd_symbols_10);
    terminal_set_output_cb(&terminal, ssh_terminal_output_cb);
    ssh_set_terminal(&terminal);

    lv_obj_clear_flag(terminal.canvas, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "Terminal canvas: %dx%d, font %dx%d, cols %d rows %d",
             terminal.font_w * terminal.cols, terminal.font_h * terminal.rows,
             terminal.font_w, terminal.font_h, terminal.cols, terminal.rows);

    lv_obj_update_layout(lv_scr_act());

    shell_init(&terminal);

    ret = hostname_mgr_init();
    if (ret != ESP_OK)
        ESP_LOGE(TAG, "Hostname init: %s", esp_err_to_name(ret));
    else
        ESP_LOGI(TAG, "Hostname: %s", hostname_mgr_get());

    if (status_label) {
        lv_obj_del(status_label);
        status_label = NULL;
        ble_hid_set_label(NULL);
    }

    // Init WiFi (coexists with BT)
    ret = wifi_mgr_init();
    if (ret != ESP_OK)
        ESP_LOGE(TAG, "WiFi init: %s", esp_err_to_name(ret));
    else
        ESP_LOGI(TAG, "WiFi init OK");

    // Init BLE HID host
    ret = ble_hid_host_init();
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "BLE HID host unsupported on this target; continuing without BLE keyboard");
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BLE HID host init failed: %s", esp_err_to_name(ret));
        shell_print("\r\n  BLE HID Error");
    } else {
        ESP_LOGI(TAG, "BLE HID Host init OK");
    }

    ret = usb_hid_host_init();
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "USB HID host unsupported on this target; continuing without wired keyboard");
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB HID host init failed: %s", esp_err_to_name(ret));
        shell_print("\r\n  USB HID Error");
    } else {
        ESP_LOGI(TAG, "USB HID Host init OK");
    }

    ret = ui_status_menu_init(lv_scr_act());
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Touch/status menu init failed: %s", esp_err_to_name(ret));
    }

    ret = tailscale_mgr_init();
    if (ret != ESP_OK)
        ESP_LOGE(TAG, "Tailscale init: %s", esp_err_to_name(ret));
    else
        ESP_LOGI(TAG, "Tailscale init OK");

    power_install_hooks();

    while (1) {
        ble_hid_process_queue();
        usb_hid_process_queue();
        lv_obj_clear_flag(terminal.canvas, LV_OBJ_FLAG_HIDDEN);

        wifi_mgr_process_queue();
        tailscale_mgr_process_queue();
        ssh_process_queue();
        ui_status_menu_update();
        terminal_render(&terminal);
        lv_timer_handler();

        unsigned delay_ms = power_mgr_step();
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}
