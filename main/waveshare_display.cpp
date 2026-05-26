#include "waveshare_display.hpp"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lvgl.h"

static const char *TAG = "WS_DISP";
#define W 800
#define H 480
#define DRAW_BUF_LINES 4
#define PCLK_HZ 12500000  // known-good clock for this panel

/* Waveshare 7" ESP32-S3 RGB GPIOs per official doc + Launcher */
#define DATA0  GPIO_NUM_14  /* B3 */
#define DATA1  GPIO_NUM_39  /* G2 */
#define DATA2  GPIO_NUM_42  /* R5 */
#define DATA3  GPIO_NUM_0   /* G3 */
#define DATA4  GPIO_NUM_45  /* G4 */
#define DATA5  GPIO_NUM_48  /* G5 */
#define DATA6  GPIO_NUM_47  /* G6 */
#define DATA7  GPIO_NUM_21  /* G7 */
#define DATA8  GPIO_NUM_40  /* R7 */
#define DATA9  GPIO_NUM_41  /* R6 */
#define DATA10 GPIO_NUM_1   /* R3 */
#define DATA11 GPIO_NUM_2   /* R4 */
#define DATA12 GPIO_NUM_38  /* B4 */
#define DATA13 GPIO_NUM_17  /* B6 */
#define DATA14 GPIO_NUM_18  /* B5 */
#define DATA15 GPIO_NUM_10  /* B7 */
#define HSYNC  GPIO_NUM_46
#define VSYNC GPIO_NUM_3
#define PCLK   GPIO_NUM_7
#define DE     GPIO_NUM_5

static esp_lcd_panel_handle_t panel = NULL;
static lv_display_t *disp = NULL;
static void *buf1 = NULL, *buf2 = NULL;

// Remap LVGL RGB565 pixel bits to match Waveshare 7" display wiring
static inline uint16_t remap_pixel(uint16_t p) {
    return
        (p & 0x0001)              | // B0 → bit 0
        ((p & 0x0020) >> 4)       | // G0 → bit 1
        ((p & 0x2000) >> 11)      | // R2 → bit 2
        ((p & 0x0040) >> 3)       | // G1 → bit 3
        ((p & 0x0080) >> 3)       | // G2 → bit 4
        ((p & 0x0100) >> 3)       | // G3 → bit 5
        ((p & 0x0200) >> 3)       | // G4 → bit 6
        ((p & 0x0400) >> 3)       | // G5 → bit 7
        ((p & 0x8000) >> 7)       | // R4 → bit 8
        ((p & 0x4000) >> 5)       | // R3 → bit 9
        ((p & 0x0800) >> 1)       | // R0 → bit 10
        ((p & 0x1000) >> 1)       | // R1 → bit 11
        ((p & 0x0002) << 11)      | // B1 → bit 12
        ((p & 0x0008) << 10)      | // B3 → bit 13
        ((p & 0x0004) << 12)      | // B2 → bit 14
        ((p & 0x0010) << 11);       // B4 → bit 15
}

static void flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    uint16_t *buf = (uint16_t *)px;
    uint32_t n = w * h;
    for (uint32_t i = 0; i < n; i++) {
        buf[i] = remap_pixel(buf[i]);
    }
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2+1, area->y2+1, px);
    lv_display_flush_ready(d);
}

esp_err_t waveshare_display_init(lv_display_t **out_disp) {
    esp_lcd_rgb_panel_config_t cfg = {};
    cfg.clk_src = LCD_CLK_SRC_PLL160M;
    cfg.timings.pclk_hz = PCLK_HZ;
    cfg.timings.h_res = W;
    cfg.timings.v_res = H;
    cfg.data_width = 16;
    cfg.num_fbs = 2;
    cfg.bounce_buffer_size_px = W * 10;
    cfg.timings.hsync_pulse_width = 4;
    cfg.timings.hsync_back_porch = 8;
    cfg.timings.hsync_front_porch = 8;
    cfg.timings.vsync_pulse_width = 4;
    cfg.timings.vsync_back_porch = 8;
    cfg.timings.vsync_front_porch = 8;
    cfg.flags.fb_in_psram = 1;
    cfg.hsync_gpio_num = HSYNC;
    cfg.vsync_gpio_num = VSYNC;
    cfg.de_gpio_num = DE;
    cfg.pclk_gpio_num = PCLK;
    cfg.data_gpio_nums[0] = DATA0;
    cfg.data_gpio_nums[1] = DATA1;
    cfg.data_gpio_nums[2] = DATA2;
    cfg.data_gpio_nums[3] = DATA3;
    cfg.data_gpio_nums[4] = DATA4;
    cfg.data_gpio_nums[5] = DATA5;
    cfg.data_gpio_nums[6] = DATA6;
    cfg.data_gpio_nums[7] = DATA7;
    cfg.data_gpio_nums[8] = DATA8;
    cfg.data_gpio_nums[9] = DATA9;
    cfg.data_gpio_nums[10] = DATA10;
    cfg.data_gpio_nums[11] = DATA11;
    cfg.data_gpio_nums[12] = DATA12;
    cfg.data_gpio_nums[13] = DATA13;
    cfg.data_gpio_nums[14] = DATA14;
    cfg.data_gpio_nums[15] = DATA15;

    esp_err_t ret = esp_lcd_new_rgb_panel(&cfg, &panel);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "rgb panel fail %s", esp_err_to_name(ret)); return ret; }
    ret = esp_lcd_panel_init(panel);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "panel init fail"); return ret; }
    ret = esp_lcd_panel_disp_on_off(panel, true);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "disp on fail"); return ret; }

    size_t draw_buf_bytes = W * DRAW_BUF_LINES * sizeof(uint16_t);
    buf1 = heap_caps_malloc(draw_buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf1) {
        ESP_LOGE(TAG, "draw buffer alloc fail (%u bytes internal)", (unsigned)draw_buf_bytes);
        return ESP_ERR_NO_MEM;
    }
    buf2 = NULL;

    disp = lv_display_create(W, H);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, draw_buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

    *out_disp = disp;
    ESP_LOGI(TAG, "Display 800x480 init ok");
    return ESP_OK;
}
