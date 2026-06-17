#ifndef TERMINAL_HPP
#define TERMINAL_HPP

#include "lvgl.h"
#include "esp_err.h"
#include "fonts/cozette_bdf.h"
#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

#define TERM_MAX_ROWS 128
#define TERM_MAX_COLS 256
#define TERM_SCROLLBACK_MAX_LINES 256

typedef struct {
    uint32_t code;
    uint16_t fg;
    uint16_t bg;
    unsigned bold : 1;
    unsigned dim : 1;
    unsigned italic : 1;
    unsigned underline : 1;
    unsigned blink : 1;
    unsigned reverse : 1;
    unsigned conceal : 1;
    unsigned strike : 1;
    unsigned dirty : 1;
} term_cell_t;

struct terminal_t {
    term_cell_t *screen;
    term_cell_t *alt;
    int rows, cols;
    int cursor_row, cursor_col;
    int saved_row, saved_col;
    int scroll_top, scroll_bottom;
    bool cursor_visible;
    bool origin_mode;
    bool wraparound;
    bool insert_mode;
    bool application_cursor_keys;
    bool alt_screen;
    bool cursor_dirty;
    int view_offset;

    uint16_t cur_fg, cur_bg;
    unsigned cur_bold : 1;
    unsigned cur_dim : 1;
    unsigned cur_italic : 1;
    unsigned cur_underline : 1;
    unsigned cur_blink : 1;
    unsigned cur_reverse : 1;
    unsigned cur_conceal : 1;
    unsigned cur_strike : 1;

    uint8_t state;
    uint8_t charset_target;
    char param_buf[64];
    int param_len;
    int params[16];
    int param_count;
    char final_byte;
    char csi_intermediate;
    bool question_mark;
    bool greater_than;

    uint8_t osc_buf[256];
    int osc_len;

    lv_obj_t *canvas;
    lv_layer_t layer;
    const cozette_bdf_font_t *bitmap_font;
    const lv_font_t *font;
    const lv_font_t *fallback_font;
    int font_w, font_h;
    uint32_t utf8_codepoint;
    uint8_t utf8_expected;
    uint8_t utf8_seen;
    bool g0_dec_special;
    bool g1_dec_special;
    bool use_g1;
    bool cursor_blink_phase;
    int last_cursor_row;
    int last_cursor_col;

    term_cell_t *scrollback;
    int scrollback_capacity;
    int scrollback_count;
    int scrollback_head;

    void (*output_cb)(const char *data, size_t len);
};

void terminal_init(terminal_t *term,
                   int cols,
                   int rows,
                   int cell_w,
                   int cell_h,
                   lv_obj_t *parent,
                   const cozette_bdf_font_t *bitmap_font,
                   const lv_font_t *font,
                   const lv_font_t *fallback_font);
void terminal_write(terminal_t *term, const char *data, size_t len);
void terminal_render(terminal_t *term);
void terminal_set_output_cb(terminal_t *term, void (*cb)(const char *, size_t));
void terminal_scrollback_step(terminal_t *term, int delta_lines);
void terminal_scrollback_reset(terminal_t *term);
bool terminal_scrollback_active(const terminal_t *term);

// Default terminal foreground color, as an xterm/ANSI 256-color index (0-255).
// Setting it changes the color used wherever the terminal resets to its
// default text color (new cells, SGR reset, SGR 39). Existing already-drawn
// cells keep their stored color until rewritten. Default index is 10 (ANSI
// bright green).
void terminal_set_default_fg_index(int index);
int terminal_get_default_fg_index(void);

// Load the saved default-fg index from NVS (call once at startup before
// terminal_init). Falls back to the built-in default if none is stored.
void terminal_load_default_fg_index(void);
// Persist the given default-fg index to NVS.
esp_err_t terminal_save_default_fg_index(int index);
// Resolve an xterm/ANSI 256-color index (0-255) to a packed 0xRRGGBB value,
// e.g. for previewing the color in UI. Matches the terminal's own palette.
uint32_t terminal_color_256_rgb888(int index);

#ifdef __cplusplus
}
#endif

#endif
