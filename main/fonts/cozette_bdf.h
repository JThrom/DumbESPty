#ifndef COZETTE_BDF_H
#define COZETTE_BDF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t code;
    uint32_t bitmap_index;
    uint8_t box_w;
    uint8_t box_h;
    int8_t ofs_x;
    int8_t ofs_y;
    uint8_t adv_w;
} cozette_bdf_glyph_t;

typedef struct {
    uint16_t line_height;
    uint16_t ascent;
    uint32_t glyph_count;
    const cozette_bdf_glyph_t *glyphs;
    const uint8_t *bitmap;
} cozette_bdf_font_t;

extern const cozette_bdf_font_t g_cozette_bdf_13;

const cozette_bdf_glyph_t *cozette_bdf_lookup(const cozette_bdf_font_t *font, uint32_t code);

#ifdef __cplusplus
}
#endif

#endif
