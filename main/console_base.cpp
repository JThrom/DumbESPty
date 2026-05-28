#include "ch422g_init.hpp"
#include "waveshare_display.hpp"
#include "ble_hid_host.hpp"
#include "shell.hpp"
#include "ui_status_menu.hpp"
#include "wifi_mgr.hpp"
#include "hostname_mgr.hpp"
#include "ssh_client.hpp"
#include "tailscale_mgr.hpp"
#include "terminal.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_crypto_lock.h"
#include "nvs_flash.h"
#include "lvgl.h"

static const char *TAG = "console";
static lv_obj_t *status_label = NULL;
static terminal_t terminal;

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

    terminal_init(&terminal,
                  100,
                  32,
                  8,
                  15,
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

    ret = ui_status_menu_init(lv_scr_act());
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Touch/status menu init failed: %s", esp_err_to_name(ret));
    }

    ret = tailscale_mgr_init();
    if (ret != ESP_OK)
        ESP_LOGE(TAG, "Tailscale init: %s", esp_err_to_name(ret));
    else
        ESP_LOGI(TAG, "Tailscale init OK");

    while (1) {
        ble_hid_process_queue();
        lv_obj_clear_flag(terminal.canvas, LV_OBJ_FLAG_HIDDEN);

        wifi_mgr_process_queue();
        tailscale_mgr_process_queue();
        ssh_process_queue();
        ui_status_menu_update();
        terminal_render(&terminal);
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
