/* Host test mock of LVGL (lvgl.h).
 * Only the types, fields, enums and functions referenced by terminal.cpp /
 * shell.cpp are modelled. Rendering functions are stubbed to allocate plain
 * host buffers so the draw path links; tests do not assert pixel output (the
 * drawing routines need a real panel), they exercise the parser/logic paths. */
#ifndef MOCK_LVGL_H
#define MOCK_LVGL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- enums / opacity / formats / alignment ---- */
typedef enum {
    LV_COLOR_FORMAT_A8 = 1,
    LV_COLOR_FORMAT_RGB565 = 2,
    LV_COLOR_FORMAT_RGB888 = 3,
    LV_COLOR_FORMAT_ARGB8888 = 4,
} lv_color_format_t;

typedef enum {
    LV_ALIGN_TOP_LEFT = 0,
    LV_ALIGN_CENTER = 1,
} lv_align_t;

#define LV_OPA_TRANSP 0
#define LV_OPA_COVER  255

typedef uint8_t lv_opa_t;

/* ---- color ---- */
typedef struct {
    uint8_t blue;
    uint8_t green;
    uint8_t red;
} lv_color_t;

lv_color_t lv_color_make(uint8_t r, uint8_t g, uint8_t b);

/* ---- objects ---- */
typedef struct lv_obj_t lv_obj_t;

void lv_obj_align(lv_obj_t *obj, lv_align_t align, int32_t x, int32_t y);
void lv_obj_invalidate(lv_obj_t *obj);

/* ---- draw buffer ---- */
typedef struct {
    lv_color_format_t cf;
    uint32_t w;
    uint32_t h;
    uint32_t stride;
} lv_image_header_t;

typedef struct {
    lv_image_header_t header;
    void *data;
    uint32_t data_size;
} lv_draw_buf_t;

lv_draw_buf_t *lv_draw_buf_create(uint32_t w, uint32_t h, lv_color_format_t cf, uint32_t stride);
void lv_draw_buf_destroy(lv_draw_buf_t *buf);
void lv_draw_buf_flush_cache(lv_draw_buf_t *buf, const void *area);
uint32_t lv_draw_buf_width_to_stride(uint32_t w, lv_color_format_t cf);

/* ---- canvas ---- */
lv_obj_t *lv_canvas_create(lv_obj_t *parent);
void lv_canvas_set_buffer(lv_obj_t *canvas, void *buf, int32_t w, int32_t h, lv_color_format_t cf);
void lv_canvas_fill_bg(lv_obj_t *canvas, lv_color_t color, lv_opa_t opa);
lv_draw_buf_t *lv_canvas_get_draw_buf(lv_obj_t *canvas);

/* ---- fonts ---- */
typedef struct _lv_font_t {
    uint16_t line_height;
    uint16_t base_line;
    void *dsc;
} lv_font_t;

typedef struct {
    const lv_font_t *resolved_font;
    uint16_t adv_w;
    uint16_t box_w;
    uint16_t box_h;
    int16_t ofs_x;
    int16_t ofs_y;
    uint8_t format;
    uint32_t gid;
} lv_font_glyph_dsc_t;

uint16_t lv_font_get_glyph_width(const lv_font_t *font, uint32_t letter, uint32_t letter_next);
uint16_t lv_font_get_line_height(const lv_font_t *font);
bool lv_font_get_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc,
                           uint32_t letter, uint32_t letter_next);
const void *lv_font_get_glyph_bitmap(lv_font_glyph_dsc_t *dsc, lv_draw_buf_t *draw_buf);

/* ---- layer (only used as an opaque field in terminal_t) ---- */
typedef struct {
    void *draw_buf;
    void *opaque;
} lv_layer_t;

/* ---- tick / timer ---- */
uint32_t lv_tick_get(void);
void lv_timer_handler(void);

#ifdef __cplusplus
}
#endif

#endif
