/* Host-side LVGL stubs. Canvas/draw-buffer creation allocates plain host
 * memory so the firmware's display init/draw code links and can run without a
 * panel. Glyph lookup returns "not found" so the draw path takes its
 * safe/blank branches; unit tests assert on parser/state, not pixels. */
#include "lvgl.h"

#include <cstdlib>
#include <cstring>

struct lv_obj_t {
    lv_draw_buf_t *draw_buf;
};

extern "C" lv_color_t lv_color_make(uint8_t r, uint8_t g, uint8_t b) {
    lv_color_t c;
    c.red = r;
    c.green = g;
    c.blue = b;
    return c;
}

extern "C" void lv_obj_align(lv_obj_t *, lv_align_t, int32_t, int32_t) {}
extern "C" void lv_obj_invalidate(lv_obj_t *) {}

extern "C" uint32_t lv_draw_buf_width_to_stride(uint32_t w, lv_color_format_t cf) {
    uint32_t bpp = (cf == LV_COLOR_FORMAT_RGB565) ? 2 : 1;
    return w * bpp;
}

extern "C" lv_draw_buf_t *lv_draw_buf_create(uint32_t w, uint32_t h, lv_color_format_t cf, uint32_t stride) {
    lv_draw_buf_t *b = (lv_draw_buf_t *)calloc(1, sizeof(lv_draw_buf_t));
    if (!b) return nullptr;
    b->header.w = w;
    b->header.h = h;
    b->header.cf = cf;
    b->header.stride = stride ? stride : lv_draw_buf_width_to_stride(w, cf);
    b->data_size = b->header.stride * h;
    b->data = calloc(1, b->data_size ? b->data_size : 1);
    return b;
}

extern "C" void lv_draw_buf_destroy(lv_draw_buf_t *buf) {
    if (!buf) return;
    free(buf->data);
    free(buf);
}

extern "C" void lv_draw_buf_flush_cache(lv_draw_buf_t *, const void *) {}

extern "C" lv_obj_t *lv_canvas_create(lv_obj_t *) {
    lv_obj_t *o = (lv_obj_t *)calloc(1, sizeof(lv_obj_t));
    return o;
}

extern "C" void lv_canvas_set_buffer(lv_obj_t *canvas, void *buf, int32_t w, int32_t h, lv_color_format_t cf) {
    if (!canvas) return;
    if (!canvas->draw_buf) canvas->draw_buf = (lv_draw_buf_t *)calloc(1, sizeof(lv_draw_buf_t));
    canvas->draw_buf->data = buf;
    canvas->draw_buf->header.w = w;
    canvas->draw_buf->header.h = h;
    canvas->draw_buf->header.cf = cf;
    canvas->draw_buf->header.stride = lv_draw_buf_width_to_stride(w, cf);
}

extern "C" void lv_canvas_fill_bg(lv_obj_t *, lv_color_t, lv_opa_t) {}

extern "C" lv_draw_buf_t *lv_canvas_get_draw_buf(lv_obj_t *canvas) {
    return canvas ? canvas->draw_buf : nullptr;
}

extern "C" uint16_t lv_font_get_glyph_width(const lv_font_t *, uint32_t, uint32_t) { return 8; }
extern "C" uint16_t lv_font_get_line_height(const lv_font_t *font) {
    return font ? font->line_height : 15;
}
extern "C" bool lv_font_get_glyph_dsc(const lv_font_t *, lv_font_glyph_dsc_t *, uint32_t, uint32_t) {
    return false;  /* glyph not found -> draw path takes blank/fallback branch */
}
extern "C" const void *lv_font_get_glyph_bitmap(lv_font_glyph_dsc_t *, lv_draw_buf_t *) {
    return nullptr;
}

extern "C" uint32_t lv_tick_get(void) { return 0; }
extern "C" void lv_timer_handler(void) {}
