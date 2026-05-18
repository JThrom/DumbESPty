#include "ch422g_init.hpp"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "CH422G";
#define I2C_SDA GPIO_NUM_8
#define I2C_SCL GPIO_NUM_9

// CH422G uses different I2C addresses for different operations
#define CH422G_ADDR_MODE 0x24
#define CH422G_ADDR_OUT  0x38
#define CH422G_ADDR_IN  0x26

static i2c_master_bus_handle_t bus = NULL;

// Return I2C bus handle for sharing with other drivers (touch)
i2c_master_bus_handle_t ch422g_get_i2c_bus(void) {
    return bus;
}

static esp_err_t ch422g_write(uint8_t addr, uint8_t data) {
    i2c_master_dev_handle_t dev = NULL;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 100000,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (ret != ESP_OK) return ret;
    
    ret = i2c_master_transmit(dev, &data, 1, 1000);
    i2c_master_bus_rm_device(dev);
    return ret;
}

esp_err_t ch422g_init(void) {
    ESP_LOGI(TAG, "Init I2C bus SDA=%d SCL=%d", I2C_SDA, I2C_SCL);
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = true },
    };
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus fail: %s", esp_err_to_name(ret));
        return ret;
    }

    // Step 1: Set mode - enable I/O output (IO_OE=1)
    ESP_LOGI(TAG, "Write 0x01 to addr 0x%02X (MODE)", CH422G_ADDR_MODE);
    ret = ch422g_write(CH422G_ADDR_MODE, 0x01);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Write MODE fail: %s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    // Step 2: Set output pins with GT911 reset sequence
    // First: TP_RST=0 (reset touch), DISP=1, BL=1 (keep display on)
    ESP_LOGI(TAG, "Reset GT911: TP_RST=0, DISP=1, BL=1");
    ret = ch422g_write(CH422G_ADDR_OUT, 0x0C);  // 0x0C = DISP+BL only
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Write OUT fail: %s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // Release from reset: TP_RST=1
    ESP_LOGI(TAG, "Release GT911: TP_RST=1");
    ret = ch422g_write(CH422G_ADDR_OUT, 0x0E);  // 0x0E = all high
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Write OUT fail: %s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(50));  // Wait for GT911 to boot
    
    // Step 3: Enable USB mode - EXIO5 low (bit5=0) for USB, high for CAN
    ESP_LOGI(TAG, "Set USB mode (EXIO5=0)");
    ret = ch422g_write(CH422G_ADDR_OUT, 0x0E);  // 0x0E = bits 1,2,3 set, bit5=0
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Write OUT fail (USB pwr): %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "CH422G init ok (TP_RST=1, DISP=1, BL=1, USB_PWR=1)");
    return ESP_OK;
}

void ch422g_set_backlight(bool on) {
    uint8_t val = on ? 0x0E : 0x06;
    ch422g_write(CH422G_ADDR_OUT, val);
}
