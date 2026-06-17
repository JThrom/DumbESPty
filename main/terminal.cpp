#include "terminal.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>

static const char *TAG = "TERM";

/* Serializes terminal_write() across tasks. The parser is a stateful
 * byte-stream machine; concurrent writers (e.g. shell task printing the
 * post-connect clear while the main loop feeds SSH RX data) interleave
 * bytes mid-escape-sequence and corrupt parsing (observed: OSC swallowed,
 * CSI params clobbered, query replies lost). */
static SemaphoreHandle_t s_term_write_lock = NULL;
static constexpr bool kTermVerboseTrace = false;
static int s_csi_trace_budget = kTermVerboseTrace ? 600 : 0;
static int s_missing_glyph_budget = 32;

#if CONFIG_IDF_TARGET_ESP32S3
static constexpr bool kEnableGlyphHalo = true;
#else
static constexpr bool kEnableGlyphHalo = false;
#endif

#define GLYPH_ALPHA_THRESHOLD 1

#define ST_GROUND            0
#define ST_ESC               1
#define ST_CSI               2
#define ST_CSI_PARAM         3
#define ST_CSI_INTERMEDIATE  4
#define ST_OSC               5
#define ST_ESC_CHARSET       6
#define ST_DCS               7

#define RGB565(r, g, b)  ((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))
#define RGB565_BLACK     RGB565(0, 0, 0)
#define RGB565_WHITE     RGB565(255, 255, 255)

// Default terminal foreground color, expressed as an xterm/ANSI 256-color
// index (0-255). Index 10 is ANSI bright green, the default text color.
// Configurable at runtime via terminal_set_default_fg_index().
#define DEFAULT_FG_INDEX_DEFAULT  10
static int s_default_fg_index = DEFAULT_FG_INDEX_DEFAULT;

// Active terminal instance, captured in terminal_init(), so color changes from
// the UI can recolor/redraw existing cells without threading a terminal_t*.
static terminal_t *s_active_term = nullptr;

static const uint16_t ansi_colors[16] = {
    RGB565(0, 0, 0),       RGB565(170, 0, 0),
    RGB565(0, 170, 0),     RGB565(170, 85, 0),
    RGB565(0, 0, 170),     RGB565(170, 0, 170),
    RGB565(0, 170, 170),   RGB565(170, 170, 170),
    RGB565(85, 85, 85),    RGB565(255, 85, 85),
    RGB565(85, 255, 85),   RGB565(255, 255, 85),
    RGB565(85, 85, 255),   RGB565(255, 85, 255),
    RGB565(85, 255, 255),  RGB565(255, 255, 255),
};

static uint16_t rgb_to_565(uint8_t r, uint8_t g, uint8_t b) {
    return RGB565(r, g, b);
}

static inline int clamp_i(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline int luma_565(uint16_t c) {
    int r = ((c >> 11) & 0x1F) * 255 / 31;
    int g = ((c >> 5) & 0x3F) * 255 / 63;
    int b = (c & 0x1F) * 255 / 31;
    return (r * 54 + g * 183 + b * 19) / 256;
}

static uint16_t compensate_text_color(uint16_t fg, uint16_t bg) {
    int r = (fg >> 11) & 0x1F;
    int g = (fg >> 5) & 0x3F;
    int b = fg & 0x1F;
    int fl = luma_565(fg);
    int bl = luma_565(bg);
    int diff = fl - bl;

    bool blue_dom = (b >= r + 4) && (b * 2 >= g);
    bool yellow_dom = (r >= 20) && (g >= 36) && (b <= 8);

    if (g > r || b > r) {
        r = clamp_i(r + 2, 0, 31);
        g = clamp_i(g + 5, 0, 63);
        b = clamp_i(b + 4, 0, 31);
    }

    if (blue_dom) {
        b = clamp_i(b + 6, 0, 31);
        g = clamp_i(g + 3, 0, 63);
        r = clamp_i(r + 1, 0, 31);
    }

    if (yellow_dom) {
        g = clamp_i(g + 5, 0, 63);
        r = clamp_i(r - 2, 0, 31);
        b = clamp_i(b + 1, 0, 31);
    }

    int adiff = diff < 0 ? -diff : diff;
    if (adiff < 96) {
        int push = (96 - adiff + 1) / 2;
        if (diff >= 0) {
            r = clamp_i(r + push, 0, 31);
            g = clamp_i(g + push, 0, 63);
            b = clamp_i(b + push, 0, 31);
        } else {
            r = clamp_i(r - push, 0, 31);
            g = clamp_i(g - push, 0, 63);
            b = clamp_i(b - push, 0, 31);
        }
    }

    return (uint16_t)((r << 11) | (g << 5) | b);
}

static uint16_t halo_color(uint16_t bg, uint16_t fg) {
    int bl = luma_565(bg);
    int fl = luma_565(fg);
    int d = fl - bl;
    if (d >= 0 && d < 56) return RGB565(232, 232, 232);
    if (fl > bl) return RGB565(96, 96, 96);
    return RGB565(216, 216, 216);
}

/* Linear blend of two RGB565 colors. num/den = weight of fg. */
static uint16_t blend_565(uint16_t fg, uint16_t bg, int num, int den) {
    int fr = (fg >> 11) & 0x1F, fgc = (fg >> 5) & 0x3F, fb = fg & 0x1F;
    int br = (bg >> 11) & 0x1F, bgc = (bg >> 5) & 0x3F, bb = bg & 0x1F;
    int r = (fr * num + br * (den - num)) / den;
    int g = (fgc * num + bgc * (den - num)) / den;
    int b = (fb * num + bb * (den - num)) / den;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static uint16_t color_256(int idx) {
    if (idx < 16) return ansi_colors[idx];
    if (idx < 232) {
        idx -= 16;
        int r = (idx / 36) ? (idx / 36) * 42 + 55 : 0;
        int g = ((idx % 36) / 6) ? ((idx % 36) / 6) * 42 + 55 : 0;
        int b = (idx % 6) ? (idx % 6) * 42 + 55 : 0;
        return rgb_to_565(r, g, b);
    }
    int gray = (idx - 232) * 10 + 8;
    return rgb_to_565(gray, gray, gray);
}

// Resolve the configured default foreground color (RGB565) from its 256-color
// index. Used everywhere the terminal resets to its default text color.
static uint16_t default_fg(void) {
    return color_256(s_default_fg_index);
}

static inline bool is_private_use_codepoint(uint32_t code) {
    return (code >= 0xE000 && code <= 0xF8FF) ||
           (code >= 0xF0000 && code <= 0xFFFFD) ||
           (code >= 0x100000 && code <= 0x10FFFD);
}

static uint16_t resolve_fg(terminal_t *term) {
    uint16_t c = term->cur_fg;
    if (term->cur_bold && c < 8) c = ansi_colors[c + 8];
    if (term->cur_reverse) c = term->cur_bg;
    return c;
}

static uint16_t resolve_bg(terminal_t *term) {
    uint16_t c = term->cur_bg;
    if (term->cur_reverse) c = term->cur_fg;
    return c;
}

static term_cell_t *cell_ptr(terminal_t *term, int row, int col) {
    if (row < 0 || row >= term->rows || col < 0 || col >= term->cols) return NULL;
    term_cell_t *buf = term->alt_screen ? term->alt : term->screen;
    return &buf[row * term->cols + col];
}

static void mark_all_dirty(terminal_t *term) {
    term_cell_t *buf = term->alt_screen ? term->alt : term->screen;
    const int total = term->rows * term->cols;
    for (int i = 0; i < total; i++) buf[i].dirty = 1;
}

static int scrollback_line_index(const terminal_t *term, int logical_idx) {
    if (!term->scrollback || term->scrollback_count <= 0) return -1;
    if (logical_idx < 0 || logical_idx >= term->scrollback_count) return -1;
    return (term->scrollback_head + logical_idx) % term->scrollback_capacity;
}

static void scrollback_push_row(terminal_t *term, int row) {
    if (!term->scrollback || term->alt_screen) return;
    if (row < 0 || row >= term->rows) return;

    term_cell_t *buf = term->alt_screen ? term->alt : term->screen;
    term_cell_t *src = &buf[row * term->cols];

    int dst_slot;
    if (term->scrollback_count < term->scrollback_capacity) {
        dst_slot = (term->scrollback_head + term->scrollback_count) % term->scrollback_capacity;
        term->scrollback_count++;
    } else {
        dst_slot = term->scrollback_head;
        term->scrollback_head = (term->scrollback_head + 1) % term->scrollback_capacity;
    }

    term_cell_t *dst = &term->scrollback[dst_slot * term->cols];
    memcpy(dst, src, term->cols * sizeof(term_cell_t));
    for (int c = 0; c < term->cols; c++) dst[c].dirty = 0;
}

static const term_cell_t *view_cell(const terminal_t *term,
                                    const term_cell_t *screen_buf,
                                    int render_row,
                                    int col,
                                    int view_offset) {
    const int total_lines = term->scrollback_count + term->rows;
    int start = total_lines - term->rows - view_offset;
    if (start < 0) start = 0;
    int line = start + render_row;

    if (line < term->scrollback_count) {
        int sb_idx = scrollback_line_index(term, line);
        if (sb_idx >= 0) return &term->scrollback[sb_idx * term->cols + col];
    }

    int screen_row = line - term->scrollback_count;
    if (screen_row < 0) screen_row = 0;
    if (screen_row >= term->rows) screen_row = term->rows - 1;
    return &screen_buf[screen_row * term->cols + col];
}

static void reset_cell_bg(term_cell_t *c, uint16_t bg) {
    c->code = ' ';
    c->fg = default_fg();
    c->bg = bg;
    c->bold = c->dim = c->italic = c->underline = 0;
    c->blink = c->reverse = c->conceal = c->strike = 0;
    c->dirty = 1;
}

static void reset_cell(term_cell_t *c) {
    reset_cell_bg(c, RGB565_BLACK);
}

static void clear_area(terminal_t *term, int r1, int c1, int r2, int c2) {
    /* Erase fills with the current SGR background color (xterm behavior), so
     * apps that clear regions after setting a themed bg (e.g. LazyVim's dark
     * Normal background) paint that color rather than hardcoded black. */
    uint16_t bg = term->cur_bg;
    for (int r = r1; r <= r2; r++) {
        for (int c = c1; c <= c2; c++) {
            term_cell_t *cp = cell_ptr(term, r, c);
            if (cp) reset_cell_bg(cp, bg);
        }
    }
}

static bool resolve_glyph(const terminal_t *term,
                         uint32_t code,
                         lv_font_glyph_dsc_t *dsc,
                         const lv_font_t **active_font) {
    if (lv_font_get_glyph_dsc(term->font, dsc, code, 0)) {
        *active_font = term->font;
        return true;
    }
    if (term->fallback_font && lv_font_get_glyph_dsc(term->fallback_font, dsc, code, 0)) {
        *active_font = term->fallback_font;
        return true;
    }
    return false;
}

void terminal_init(terminal_t *term,
                   int cols,
                   int rows,
                   int cell_w,
                   int cell_h,
                   lv_obj_t *parent,
                   const cozette_bdf_font_t *bitmap_font,
                   const lv_font_t *font,
                   const lv_font_t *fallback_font) {
    if (!s_term_write_lock) {
        s_term_write_lock = xSemaphoreCreateMutex();
    }
    memset(term, 0, sizeof(*term));
    s_active_term = term;
    term->bitmap_font = bitmap_font;
    term->font = font;
    term->fallback_font = fallback_font;

    uint16_t adv = lv_font_get_glyph_width(font, 'M', 'M');
    term->font_w = cell_w > 0 ? cell_w : (adv > 0 ? adv : 1);
    term->font_h = cell_h > 0 ? cell_h : lv_font_get_line_height(font);

    if (cols < 0) cols = 800 / term->font_w;
    if (rows < 0) rows = 480 / term->font_h;
    term->cols = cols;
    term->rows = rows;

    size_t cell_size = rows * cols * sizeof(term_cell_t);
    term->screen = (term_cell_t *)heap_caps_malloc(cell_size, MALLOC_CAP_SPIRAM);
    term->alt = (term_cell_t *)heap_caps_malloc(cell_size, MALLOC_CAP_SPIRAM);
    term->scrollback_capacity = TERM_SCROLLBACK_MAX_LINES;
    term->scrollback = (term_cell_t *)heap_caps_malloc(term->scrollback_capacity * cols * sizeof(term_cell_t), MALLOC_CAP_SPIRAM);

    int cw = cols * term->font_w;
    int ch = rows * term->font_h;
    size_t buf_size = cw * ch * 2;
    void *canvas_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);

    term->canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(term->canvas, canvas_buf, cw, ch, LV_COLOR_FORMAT_RGB565);
    lv_obj_align(term->canvas, LV_ALIGN_TOP_LEFT, 0, 0);

    term->cursor_visible = true;
    term->view_offset = 0;
    term->cursor_blink_phase = true;
    term->last_cursor_row = 0;
    term->last_cursor_col = 0;
    s_missing_glyph_budget = 32;
    term->wraparound = true;
    term->scroll_top = 0;
    term->scroll_bottom = rows - 1;
    term->cur_fg = default_fg();
    term->cur_bg = RGB565_BLACK;

    clear_area(term, 0, 0, rows - 1, cols - 1);
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++)
            reset_cell(cell_ptr(term, r, c));

    lv_canvas_fill_bg(term->canvas, lv_color_make(0, 0, 0), LV_OPA_COVER);
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++)
            cell_ptr(term, r, c)->dirty = 0;
    ESP_LOGI(TAG, "Terminal %dx%d, cell %dx%d, cw %d ch %d",
             cols, rows, term->font_w, term->font_h, cw, ch);
}

static void scroll_region(terminal_t *term, int top, int bottom, int n) {
    int h = bottom - top + 1;
    if (n >= h) {
        clear_area(term, top, 0, bottom, term->cols - 1);
        return;
    }
    term_cell_t *buf = term->alt_screen ? term->alt : term->screen;
    if (!term->alt_screen) {
        for (int i = 0; i < n; i++) {
            scrollback_push_row(term, top + i);
        }
    }
    size_t row_size = term->cols * sizeof(term_cell_t);
    size_t move_size = (h - n) * row_size;
    memmove(buf + top * term->cols, buf + (top + n) * term->cols, move_size);
    clear_area(term, bottom - n + 1, 0, bottom, term->cols - 1);
    for (int r = top; r <= bottom; r++) {
        for (int c = 0; c < term->cols; c++) {
            term_cell_t *cp = cell_ptr(term, r, c);
            if (cp) cp->dirty = 1;
        }
    }
}

static void scroll_down_region(terminal_t *term, int top, int bottom, int n) {
    int h = bottom - top + 1;
    if (n >= h) {
        clear_area(term, top, 0, bottom, term->cols - 1);
        return;
    }
    term_cell_t *buf = term->alt_screen ? term->alt : term->screen;
    size_t row_size = term->cols * sizeof(term_cell_t);
    size_t move_size = (h - n) * row_size;
    memmove(buf + (top + n) * term->cols, buf + top * term->cols, move_size);
    clear_area(term, top, 0, top + n - 1, term->cols - 1);
    for (int r = top; r <= bottom; r++) {
        for (int c = 0; c < term->cols; c++) {
            term_cell_t *cp = cell_ptr(term, r, c);
            if (cp) cp->dirty = 1;
        }
    }
}

static uint32_t dec_special_graphics_map(uint8_t c) {
    switch (c) {
    case 'j': return 0x2518;
    case 'k': return 0x2510;
    case 'l': return 0x250C;
    case 'm': return 0x2514;
    case 'n': return 0x253C;
    case 'q': return 0x2500;
    case 't': return 0x251C;
    case 'u': return 0x2524;
    case 'v': return 0x2534;
    case 'w': return 0x252C;
    case 'x': return 0x2502;
    case 'a': return 0x2592;
    case 'f': return 0x00B0;
    case 'g': return 0x00B1;
    case 'y': return 0x2264;
    case 'z': return 0x2265;
    case '{': return 0x03C0;
    case '|': return 0x2260;
    case '}': return 0x00A3;
    case '~': return 0x00B7;
    default: return c;
    }
}

static void dirty_cell(terminal_t *term, int r, int c) {
    term_cell_t *cp = cell_ptr(term, r, c);
    if (cp) cp->dirty = 1;
}

static void new_line(terminal_t *term) {
    int cur = term->cursor_row;
    if (cur >= term->scroll_bottom) {
        scroll_region(term, term->scroll_top, term->scroll_bottom, 1);
    } else {
        term->cursor_row++;
    }
    term->cursor_col = 0;
}

static void line_feed(terminal_t *term) {
    int cur = term->cursor_row;
    dirty_cell(term, cur, term->cursor_col);
    if (cur >= term->scroll_bottom) {
        scroll_region(term, term->scroll_top, term->scroll_bottom, 1);
    } else {
        term->cursor_row++;
    }
    dirty_cell(term, term->cursor_row, term->cursor_col);
}

static void reverse_line_feed(terminal_t *term) {
    if (term->cursor_row <= term->scroll_top) {
        scroll_down_region(term, term->scroll_top, term->scroll_bottom, 1);
    } else {
        dirty_cell(term, term->cursor_row, term->cursor_col);
        term->cursor_row--;
        dirty_cell(term, term->cursor_row, term->cursor_col);
    }
}

static void carriage_return(terminal_t *term) {
    dirty_cell(term, term->cursor_row, term->cursor_col);
    term->cursor_col = 0;
    dirty_cell(term, term->cursor_row, term->cursor_col);
}

static void put_char(terminal_t *term, uint32_t ch) {
    int r = term->cursor_row;
    int c = term->cursor_col;

    if (c >= term->cols) {
        if (term->wraparound) {
            new_line(term);
            r = term->cursor_row;
            c = 0;
        } else {
            return;
        }
    }

    term_cell_t *cp = cell_ptr(term, r, c);
    if (!cp) return;

    if (term->insert_mode) {
        for (int i = term->cols - 1; i > c; i--) {
            term_cell_t *src = cell_ptr(term, r, i - 1);
            term_cell_t *dst = cell_ptr(term, r, i);
            if (src && dst) {
                memcpy(dst, src, sizeof(term_cell_t));
                dst->dirty = 1;
            }
        }
        term_cell_t *blank = cell_ptr(term, r, c);
        if (blank) reset_cell(blank);
    }

    cp->code = ch;
    cp->fg = resolve_fg(term);
    cp->bg = resolve_bg(term);
    cp->bold = term->cur_bold;
    cp->dim = term->cur_dim;
    cp->italic = term->cur_italic;
    cp->underline = term->cur_underline;
    cp->blink = term->cur_blink;
    cp->reverse = term->cur_reverse;
    cp->conceal = term->cur_conceal;
    cp->strike = term->cur_strike;
    cp->dirty = 1;

    term->cursor_col++;
}

static int pop_param(terminal_t *term, int idx, int def) {
    if (idx < term->param_count) return term->params[idx];
    return def;
}

static void sgr_handler(terminal_t *term) {
    if (term->param_count == 0) {
        term->cur_fg = default_fg();
        term->cur_bg = RGB565_BLACK;
        term->cur_bold = term->cur_dim = term->cur_italic = 0;
        term->cur_underline = term->cur_blink = 0;
        term->cur_reverse = term->cur_conceal = term->cur_strike = 0;
        return;
    }
    for (int i = 0; i < term->param_count; i++) {
        int p = term->params[i];
        if (p == 0) {
            term->cur_fg = default_fg();
            term->cur_bg = RGB565_BLACK;
            term->cur_bold = term->cur_dim = term->cur_italic = 0;
            term->cur_underline = term->cur_blink = 0;
            term->cur_reverse = term->cur_conceal = term->cur_strike = 0;
        } else if (p == 1) term->cur_bold = 1;
        else if (p == 2) term->cur_dim = 1;
        else if (p == 3) term->cur_italic = 1;
        else if (p == 4) term->cur_underline = 1;
        else if (p == 5 || p == 6) term->cur_blink = 1;
        else if (p == 7) term->cur_reverse = 1;
        else if (p == 8) term->cur_conceal = 1;
        else if (p == 9) term->cur_strike = 1;
        else if (p == 22) term->cur_bold = term->cur_dim = 0;
        else if (p == 23) term->cur_italic = 0;
        else if (p == 24) term->cur_underline = 0;
        else if (p == 25 || p == 26) term->cur_blink = 0;
        else if (p == 27) term->cur_reverse = 0;
        else if (p == 28) term->cur_conceal = 0;
        else if (p == 29) term->cur_strike = 0;
        else if (p >= 30 && p <= 37) term->cur_fg = ansi_colors[p - 30];
        else if (p == 38) {
            if (i + 2 < term->param_count && term->params[i + 1] == 5) {
                int idx = term->params[i + 2];
                term->cur_fg = color_256(idx);
                i += 2;
            } else if (i + 4 < term->param_count && term->params[i + 1] == 2) {
                int r = term->params[i + 2];
                int g = term->params[i + 3];
                int b = term->params[i + 4];
                term->cur_fg = rgb_to_565(r, g, b);
                i += 4;
            }
        }
        else if (p == 39) term->cur_fg = default_fg();
        else if (p >= 40 && p <= 47) term->cur_bg = ansi_colors[p - 40];
        else if (p == 48) {
            if (i + 2 < term->param_count && term->params[i + 1] == 5) {
                int idx = term->params[i + 2];
                term->cur_bg = color_256(idx);
                i += 2;
            } else if (i + 4 < term->param_count && term->params[i + 1] == 2) {
                int r = term->params[i + 2];
                int g = term->params[i + 3];
                int b = term->params[i + 4];
                term->cur_bg = rgb_to_565(r, g, b);
                i += 4;
            }
        }
        else if (p == 49) term->cur_bg = RGB565_BLACK;
        else if (p >= 90 && p <= 97) term->cur_fg = ansi_colors[p - 90 + 8];
        else if (p >= 100 && p <= 107) term->cur_bg = ansi_colors[p - 100 + 8];
    }
}

static void insert_lines(terminal_t *term, int n) {
    int r = term->cursor_row;
    int b = term->scroll_bottom;
    int count = n;
    if (r + count > b) count = b - r + 1;
    scroll_down_region(term, r, b, count);
}

static void delete_lines(terminal_t *term, int n) {
    int r = term->cursor_row;
    int b = term->scroll_bottom;
    int count = n;
    if (r + count > b) count = b - r + 1;
    scroll_region(term, r, b, count);
}

static void insert_chars(terminal_t *term, int n) {
    int r = term->cursor_row;
    int c = term->cursor_col;
    int max = term->cols - c;
    if (n > max) n = max;
    term_cell_t *buf = term->alt_screen ? term->alt : term->screen;
    memmove(buf + r * term->cols + c + n, buf + r * term->cols + c, (max - n) * sizeof(term_cell_t));
    clear_area(term, r, c, r, c + n - 1);
}

static void delete_chars(terminal_t *term, int n) {
    int r = term->cursor_row;
    int c = term->cursor_col;
    int max = term->cols - c;
    if (n > max) n = max;
    term_cell_t *buf = term->alt_screen ? term->alt : term->screen;
    memmove(buf + r * term->cols + c, buf + r * term->cols + c + n, (max - n) * sizeof(term_cell_t));
    clear_area(term, r, term->cols - n, r, term->cols - 1);
}

static void log_csi_trace(const terminal_t *term) {
    if (s_csi_trace_budget <= 0) return;

    char params[96];
    int p = 0;
    params[0] = '\0';
    for (int i = 0; i < term->param_count && p < (int)sizeof(params) - 1; i++) {
        p += snprintf(params + p,
                      sizeof(params) - p,
                      "%s%d",
                      (i == 0) ? "" : ";",
                      term->params[i]);
        if (p < 0 || p >= (int)sizeof(params)) {
            params[sizeof(params) - 1] = '\0';
            break;
        }
    }

    ESP_LOGI(TAG,
             "CSI trace: %s%s%s%c (row=%d col=%d)",
             term->question_mark ? "?" : "",
             term->greater_than ? ">" : "",
             params,
             term->final_byte,
             term->cursor_row + 1,
             term->cursor_col + 1);
    s_csi_trace_budget--;
}

static void csi_dispatch(terminal_t *term) {
    char f = term->final_byte;
    log_csi_trace(term);
    if (f == 'A') {
        int n = pop_param(term, 0, 1);
        dirty_cell(term, term->cursor_row, term->cursor_col);
        term->cursor_row -= n;
        if (term->cursor_row < 0) term->cursor_row = 0;
        dirty_cell(term, term->cursor_row, term->cursor_col);
    } else if (f == 'B') {
        int n = pop_param(term, 0, 1);
        dirty_cell(term, term->cursor_row, term->cursor_col);
        term->cursor_row += n;
        if (term->cursor_row >= term->rows) term->cursor_row = term->rows - 1;
        dirty_cell(term, term->cursor_row, term->cursor_col);
    } else if (f == 'C') {
        int n = pop_param(term, 0, 1);
        dirty_cell(term, term->cursor_row, term->cursor_col);
        term->cursor_col += n;
        if (term->cursor_col >= term->cols) term->cursor_col = term->cols - 1;
        dirty_cell(term, term->cursor_row, term->cursor_col);
    } else if (f == 'D') {
        int n = pop_param(term, 0, 1);
        dirty_cell(term, term->cursor_row, term->cursor_col);
        term->cursor_col -= n;
        if (term->cursor_col < 0) term->cursor_col = 0;
        dirty_cell(term, term->cursor_row, term->cursor_col);
    } else if (f == 'E') {
        int n = pop_param(term, 0, 1);
        dirty_cell(term, term->cursor_row, term->cursor_col);
        term->cursor_row += n;
        if (term->cursor_row >= term->rows) term->cursor_row = term->rows - 1;
        term->cursor_col = 0;
        dirty_cell(term, term->cursor_row, term->cursor_col);
    } else if (f == 'F') {
        int n = pop_param(term, 0, 1);
        dirty_cell(term, term->cursor_row, term->cursor_col);
        term->cursor_row -= n;
        if (term->cursor_row < 0) term->cursor_row = 0;
        term->cursor_col = 0;
        dirty_cell(term, term->cursor_row, term->cursor_col);
    } else if (f == 'G') {
        int n = pop_param(term, 0, 1) - 1;
        if (n < 0) n = 0;
        if (n >= term->cols) n = term->cols - 1;
        dirty_cell(term, term->cursor_row, term->cursor_col);
        term->cursor_col = n;
        dirty_cell(term, term->cursor_row, term->cursor_col);
    } else if (f == 'H' || f == 'f') {
        int r = pop_param(term, 0, 1) - 1;
        int c = pop_param(term, 1, 1) - 1;
        if (r < 0) r = 0;
        if (c < 0) c = 0;
        if (r >= term->rows) r = term->rows - 1;
        if (c >= term->cols) c = term->cols - 1;
        if (term->origin_mode) r += term->scroll_top;
        dirty_cell(term, term->cursor_row, term->cursor_col);
        term->cursor_row = r;
        term->cursor_col = c;
        dirty_cell(term, term->cursor_row, term->cursor_col);
    } else if (f == 'd') {
        int r = pop_param(term, 0, 1) - 1;
        if (r < 0) r = 0;
        if (r >= term->rows) r = term->rows - 1;
        dirty_cell(term, term->cursor_row, term->cursor_col);
        term->cursor_row = r;
        dirty_cell(term, term->cursor_row, term->cursor_col);
    } else if (f == 'J') {
        int mode = pop_param(term, 0, 0);
        if (mode == 0)
            clear_area(term, term->cursor_row, term->cursor_col, term->rows - 1, term->cols - 1);
        else if (mode == 1)
            clear_area(term, 0, 0, term->cursor_row, term->cursor_col);
        else if (mode == 2 || mode == 3)
            clear_area(term, 0, 0, term->rows - 1, term->cols - 1);
    } else if (f == 'K') {
        int mode = pop_param(term, 0, 0);
        if (mode == 0)
            clear_area(term, term->cursor_row, term->cursor_col, term->cursor_row, term->cols - 1);
        else if (mode == 1)
            clear_area(term, term->cursor_row, 0, term->cursor_row, term->cursor_col);
        else if (mode == 2)
            clear_area(term, term->cursor_row, 0, term->cursor_row, term->cols - 1);
    } else if (f == 'L') {
        insert_lines(term, pop_param(term, 0, 1));
    } else if (f == 'M') {
        delete_lines(term, pop_param(term, 0, 1));
    } else if (f == 'P') {
        delete_chars(term, pop_param(term, 0, 1));
    } else if (f == 'X') {
        int n = pop_param(term, 0, 1);
        if (n < 1) n = 1;
        int c2 = term->cursor_col + n - 1;
        if (c2 >= term->cols) c2 = term->cols - 1;
        clear_area(term, term->cursor_row, term->cursor_col, term->cursor_row, c2);
    } else if (f == '@') {
        insert_chars(term, pop_param(term, 0, 1));
    } else if (f == 'S') {
        int n = pop_param(term, 0, 1);
        scroll_region(term, term->scroll_top, term->scroll_bottom, n);
    } else if (f == 'T') {
        int n = pop_param(term, 0, 1);
        scroll_down_region(term, term->scroll_top, term->scroll_bottom, n);
    } else if (f == 'm' && !term->greater_than) {
        sgr_handler(term);
    } else if (f == 'm' && term->greater_than) {
        // xterm modifier/resource controls (e.g. CSI > 4 m); accept and ignore.
    } else if (f == 's') {
        term->saved_row = term->cursor_row;
        term->saved_col = term->cursor_col;
    } else if (f == 'u' && !term->question_mark && !term->greater_than) {
        dirty_cell(term, term->cursor_row, term->cursor_col);
        term->cursor_row = term->saved_row;
        term->cursor_col = term->saved_col;
        dirty_cell(term, term->cursor_row, term->cursor_col);
    } else if (f == 'u' && term->question_mark) {
        if (term->output_cb) term->output_cb("\033[?0u", 5);
    } else if (f == 'u' && term->greater_than) {
        // Kitty keyboard protocol enable/disable/query; ignore for now.
    } else if (f == 'n') {
        int q = pop_param(term, 0, 0);
        if (kTermVerboseTrace) {
            ESP_LOGI(TAG, "CSI %s%d n", term->question_mark ? "?" : "", q);
        }
        if (q == 5) {
            if (term->output_cb) {
                if (term->question_mark) term->output_cb("\033[?0n", 5);
                else term->output_cb("\033[0n", 4);
            }
        } else if (q == 6) {
            char buf[32];
            if (term->question_mark) {
                snprintf(buf, sizeof(buf), "\033[?%d;%dR",
                         term->cursor_row + 1, term->cursor_col + 1);
            } else {
                snprintf(buf, sizeof(buf), "\033[%d;%dR",
                         term->cursor_row + 1, term->cursor_col + 1);
            }
            if (term->output_cb) term->output_cb(buf, strlen(buf));
        }
    } else if (f == 'h' && term->question_mark) {
        int mode = pop_param(term, 0, 0);
        if (mode == 1) term->application_cursor_keys = true;
        else if (mode == 7) term->wraparound = true;
        else if (mode == 25) term->cursor_visible = true;
        else if (mode == 2004) {
            if (kTermVerboseTrace) {
                ESP_LOGI(TAG, "Bracketed paste ON");
            }
        }
        else if (mode == 1049 || mode == 47 || mode == 1047) {
            if (term->alt_screen) return;
            memcpy(term->alt, term->screen, term->rows * term->cols * sizeof(term_cell_t));
            clear_area(term, 0, 0, term->rows - 1, term->cols - 1);
            term->cursor_row = 0;
            term->cursor_col = 0;
            term->alt_screen = true;
        }
    } else if (f == 'l' && term->question_mark) {
        int mode = pop_param(term, 0, 0);
        if (mode == 1) term->application_cursor_keys = false;
        else if (mode == 7) term->wraparound = false;
        else if (mode == 25) term->cursor_visible = false;
        else if (mode == 2004) {
            if (kTermVerboseTrace) {
                ESP_LOGI(TAG, "Bracketed paste OFF");
            }
        }
        else if (mode == 1049 || mode == 47 || mode == 1047) {
            if (!term->alt_screen) return;
            memcpy(term->screen, term->alt, term->rows * term->cols * sizeof(term_cell_t));
            term->alt_screen = false;
            term->cursor_row = 0;
            term->cursor_col = 0;
            clear_area(term, 0, 0, term->rows - 1, term->cols - 1);
            for (int r = 0; r < term->rows; r++)
                for (int c = 0; c < term->cols; c++)
                    if (cell_ptr(term, r, c)) cell_ptr(term, r, c)->dirty = 1;
        }
    } else if (f == 'h') {
        int mode = pop_param(term, 0, 0);
        if (mode == 4) term->insert_mode = true;
    } else if (f == 'l') {
        int mode = pop_param(term, 0, 0);
        if (mode == 4) term->insert_mode = false;
    } else if (f == 'r' && term->question_mark) {
        // DECSTBM - set scroll region - handled as scroll_top param
    } else if (f == 'r' && !term->question_mark) {
        int top = pop_param(term, 0, 1) - 1;
        int bottom = pop_param(term, 1, term->rows) - 1;
        if (top < 0) top = 0;
        if (bottom >= term->rows) bottom = term->rows - 1;
        if (top < bottom) {
            term->scroll_top = top;
            term->scroll_bottom = bottom;
            dirty_cell(term, term->cursor_row, term->cursor_col);
            term->cursor_row = 0;
            term->cursor_col = 0;
            dirty_cell(term, term->cursor_row, term->cursor_col);
        }
    } else if (f == 'q' && (term->csi_intermediate == ' ' || term->greater_than)) {
        // DECSCUSR cursor style; accept and ignore.
    } else if (f == 'p' && term->question_mark && term->csi_intermediate == '$') {
        int mode = pop_param(term, 0, 0);
        char buf[32];
        snprintf(buf, sizeof(buf), "\033[?%d;0$y", mode);
        if (term->output_cb) term->output_cb(buf, strlen(buf));
    } else if (f == 't') {
        // Window ops (e.g. CSI 22;0;0t) - acknowledge by ignoring.
    } else if (f == 'c' && !term->question_mark && !term->greater_than) {
        if (term->output_cb) term->output_cb("\033[?1;2c", 7);
    } else if (f == 'c' && term->greater_than) {
        if (term->output_cb) term->output_cb("\033[>0;136;0c", 11);
    } else {
        ESP_LOGW(TAG,
                 "Unhandled CSI: %s%s%d %c",
                 term->question_mark ? "?" : "",
                 term->greater_than ? ">" : "",
                 pop_param(term, 0, 0),
                 f);
    }
}

static void osc_dispatch(terminal_t *term) {
    // Most OSC sequences (window title, etc.) are ignored, but TUI apps
    // (bubbletea/lipgloss, e.g. terminal.shop) query terminal colors at
    // startup and wait for replies before drawing anything:
    //   OSC 10 ; ? -> report default foreground color
    //   OSC 11 ; ? -> report default background color
    // Reply with the terminal's default palette (green on black).
    if (!term->output_cb || term->osc_len < 4) return;

    term->osc_buf[term->osc_len < 255 ? term->osc_len : 255] = '\0';
    const char *s = (const char *)term->osc_buf;

    if (strncmp(s, "10;?", 4) == 0) {
        // Report the configured default foreground color. Expand RGB565 to the
        // 16-bit-per-channel rgb:RRRR/GGGG/BBBB form expected by OSC 10.
        uint16_t c = default_fg();
        int r5 = (c >> 11) & 0x1F;
        int g6 = (c >> 5) & 0x3F;
        int b5 = c & 0x1F;
        int r = (r5 * 255 + 15) / 31;
        int g = (g6 * 255 + 31) / 63;
        int b = (b5 * 255 + 15) / 31;
        char fg_reply[40];
        int n = snprintf(fg_reply, sizeof(fg_reply),
                         "\033]10;rgb:%02x%02x/%02x%02x/%02x%02x\033\\",
                         r, r, g, g, b, b);
        if (n > 0) term->output_cb(fg_reply, (size_t)n);
    } else if (strncmp(s, "11;?", 4) == 0) {
        static const char bg_reply[] = "\033]11;rgb:0000/0000/0000\033\\";
        term->output_cb(bg_reply, sizeof(bg_reply) - 1);
    }
}

static void parse_char(terminal_t *term, uint8_t c);

static void emit_utf8_char(terminal_t *term, uint8_t c) {
    if ((c & 0x80) == 0) {
        put_char(term, c);
        return;
    }

    if (term->utf8_expected == 0) {
        if ((c & 0xE0) == 0xC0) {
            term->utf8_codepoint = c & 0x1F;
            term->utf8_expected = 2;
            term->utf8_seen = 1;
        } else if ((c & 0xF0) == 0xE0) {
            term->utf8_codepoint = c & 0x0F;
            term->utf8_expected = 3;
            term->utf8_seen = 1;
        } else if ((c & 0xF8) == 0xF0) {
            term->utf8_codepoint = c & 0x07;
            term->utf8_expected = 4;
            term->utf8_seen = 1;
        } else {
            put_char(term, '?');
        }
        return;
    }

    if ((c & 0xC0) != 0x80) {
        term->utf8_expected = 0;
        term->utf8_seen = 0;
        term->utf8_codepoint = 0;
        put_char(term, '?');
        return;
    }

    term->utf8_codepoint = (term->utf8_codepoint << 6) | (c & 0x3F);
    term->utf8_seen++;
    if (term->utf8_seen >= term->utf8_expected) {
        uint32_t cp = term->utf8_codepoint;
        term->utf8_expected = 0;
        term->utf8_seen = 0;
        term->utf8_codepoint = 0;

        if (cp >= 0x80 && cp <= 0x9F) {
            parse_char(term, (uint8_t)cp);
        } else {
            put_char(term, cp);
        }
    }
}

static void parse_char(terminal_t *term, uint8_t c) {
    switch (term->state) {
    case ST_GROUND:
        if (term->utf8_expected) {
            // Consume UTF-8 continuation bytes before control-sequence checks.
            // This prevents bytes like 0x9B (C1 CSI) inside multibyte glyphs
            // (e.g. U+F15B => EF 85 9B) from being misparsed as terminal control.
            if ((c & 0xC0) == 0x80) {
                emit_utf8_char(term, c);
                break;
            }
            term->utf8_expected = 0;
            term->utf8_seen = 0;
            term->utf8_codepoint = 0;
        }
        if (c == 0x1B) {
            term->utf8_expected = 0;
            term->utf8_seen = 0;
            term->utf8_codepoint = 0;
            term->state = ST_ESC;
            term->question_mark = false;
            term->greater_than = false;
        } else if (c == 0x9B) {
            term->utf8_expected = 0;
            term->utf8_seen = 0;
            term->utf8_codepoint = 0;
            term->state = ST_CSI;
            term->param_len = 0;
            term->param_count = 0;
            term->params[0] = 0;
            term->csi_intermediate = 0;
            term->question_mark = false;
            term->greater_than = false;
        } else if (c == 0x9D) {
            term->utf8_expected = 0;
            term->utf8_seen = 0;
            term->utf8_codepoint = 0;
            term->state = ST_OSC;
            term->osc_len = 0;
        } else if (c == 0x90) {
            // 8-bit DCS introducer; consume until ST (see ST_DCS).
            term->utf8_expected = 0;
            term->utf8_seen = 0;
            term->utf8_codepoint = 0;
            term->state = ST_DCS;
        } else if (c == 0x0E) {
            term->use_g1 = true;
        } else if (c == 0x0F) {
            term->use_g1 = false;
        } else if (c == '\n') {
            line_feed(term);
            if (term->wraparound) carriage_return(term);
        } else if (c == '\r') {
            carriage_return(term);
        } else if (c == '\b') {
            dirty_cell(term, term->cursor_row, term->cursor_col);
            if (term->cursor_col > 0) term->cursor_col--;
            dirty_cell(term, term->cursor_row, term->cursor_col);
        } else if (c == '\t') {
            dirty_cell(term, term->cursor_row, term->cursor_col);
            int next = (term->cursor_col / 8 + 1) * 8;
            if (next >= term->cols) next = term->cols - 1;
            term->cursor_col = next;
            dirty_cell(term, term->cursor_row, term->cursor_col);
        } else if (c >= 0x20) {
            if (c < 0x80) {
                bool dec_special = term->use_g1 ? term->g1_dec_special : term->g0_dec_special;
                if (dec_special) {
                    put_char(term, dec_special_graphics_map(c));
                    break;
                }
            }
            emit_utf8_char(term, c);
        }
        break;

    case ST_ESC:
        if (c == '[') {
            term->state = ST_CSI;
            term->param_len = 0;
            term->param_count = 0;
            term->params[0] = 0;
            term->csi_intermediate = 0;
            term->question_mark = false;
            term->greater_than = false;
        } else if (c == ']') {
            term->state = ST_OSC;
            term->osc_len = 0;
        } else if (c == 'P') {
            // DCS (Device Control String, e.g. XTGETTCAP "DCS + q ... ST").
            // We do not implement DCS responses; just consume the string up
            // to its ST terminator so it never leaks as ground text. (A prior
            // XTGETTCAP state machine caused a LazyVim rendering regression;
            // silent consumption is the safe behavior.)
            term->state = ST_DCS;
        } else if (c == '(') {
            term->charset_target = 0;
            term->state = ST_ESC_CHARSET;
        } else if (c == ')') {
            term->charset_target = 1;
            term->state = ST_ESC_CHARSET;
        } else if (c == 'D') {
            line_feed(term);
            term->state = ST_GROUND;
        } else if (c == 'M') {
            reverse_line_feed(term);
            term->state = ST_GROUND;
        } else if (c == 'E') {
            new_line(term);
            term->state = ST_GROUND;
        } else if (c == '7') {
            term->saved_row = term->cursor_row;
            term->saved_col = term->cursor_col;
            term->state = ST_GROUND;
        } else if (c == '8') {
            dirty_cell(term, term->cursor_row, term->cursor_col);
            term->cursor_row = term->saved_row;
            term->cursor_col = term->saved_col;
            dirty_cell(term, term->cursor_row, term->cursor_col);
            term->state = ST_GROUND;
        } else if (c == 'c') {
            terminal_init(term,
                          term->cols,
                          term->rows,
                          term->font_w,
                          term->font_h,
                          term->canvas,
                          term->bitmap_font,
                          term->font,
                          term->fallback_font);
            term->state = ST_GROUND;
        } else if (c == '>') {
            // DECPNM - Normal keypad mode.
            term->state = ST_GROUND;
        } else if (c == '=') {
            // DECPAM - Application keypad mode.
            term->state = ST_GROUND;
        } else {
            term->state = ST_GROUND;
        }
        break;

    case ST_ESC_CHARSET:
        if (term->charset_target == 0) term->g0_dec_special = (c == '0');
        else term->g1_dec_special = (c == '0');
        term->state = ST_GROUND;
        break;

    case ST_DCS:
        // Silently consume the DCS payload until its String Terminator.
        // ST is "ESC \" (7-bit) or 0x9C (8-bit); BEL is accepted leniently.
        if (c == 0x1B) {
            term->state = ST_ESC;   // ST_ESC handles '\' -> ST_GROUND
        } else if (c == 0x9C || c == '\007') {
            term->state = ST_GROUND;
        }
        // else: data byte, ignore.
        break;

    case ST_CSI:
        if (c == '[') {
            // Tolerate malformed/redundant "CSI [" after 8-bit CSI (0x9B).
            // Some peers emit UTF-8 C1 CSI (C2 9B) followed by '['.
        } else if (c == '?') {
            term->question_mark = true;
        } else if (c == '>') {
            term->greater_than = true;
        } else if (c >= '0' && c <= '9') {
            term->state = ST_CSI_PARAM;
            term->param_buf[0] = c;
            term->param_len = 1;
        } else if (c == ';') {
            term->state = ST_CSI_PARAM;
            term->param_len = 0;
            term->params[term->param_count++] = 0;
        } else if (c >= 0x20 && c <= 0x2F) {
            term->csi_intermediate = (char)c;
            term->state = ST_CSI_INTERMEDIATE;
        } else if (c >= 0x40 && c <= 0x7E) {
            term->final_byte = c;
            csi_dispatch(term);
            term->state = ST_GROUND;
        } else {
            term->state = ST_GROUND;
        }
        break;

    case ST_CSI_PARAM:
        if (c >= '0' && c <= '9') {
            if (term->param_len < 63) term->param_buf[term->param_len++] = c;
        } else if (c == ';') {
            term->param_buf[term->param_len] = '\0';
            if (term->param_count < 15) {
                term->params[term->param_count++] = atoi(term->param_buf);
            }
            term->param_len = 0;
        } else if (c >= 0x20 && c <= 0x2F) {
            term->param_buf[term->param_len] = '\0';
            if (term->param_count < 15)
                term->params[term->param_count++] = atoi(term->param_buf);
            term->csi_intermediate = (char)c;
            term->state = ST_CSI_INTERMEDIATE;
        } else if (c >= 0x40 && c <= 0x7E) {
            term->param_buf[term->param_len] = '\0';
            if (term->param_count < 15)
                term->params[term->param_count++] = atoi(term->param_buf);
            term->final_byte = c;
            csi_dispatch(term);
            term->state = ST_GROUND;
        } else {
            term->state = ST_GROUND;
        }
        break;

    case ST_CSI_INTERMEDIATE:
        if (c >= 0x20 && c <= 0x2F) {
            term->csi_intermediate = (char)c;
        } else if (c >= 0x40 && c <= 0x7E) {
            term->final_byte = c;
            csi_dispatch(term);
            term->state = ST_GROUND;
        } else if (c < 0x20 || c > 0x2F) {
            term->state = ST_GROUND;
        }
        break;

    case ST_OSC:
        if (c == 0x1B) {
            // ESC here is (almost always) the start of the ST terminator
            // (ESC \). Dispatch the accumulated OSC string before leaving
            // the state so ST-terminated queries (OSC 10/11) get replies.
            osc_dispatch(term);
            term->state = ST_ESC;
        } else if (c == 0x9C) {
            osc_dispatch(term);
            term->state = ST_GROUND;
        } else if (c == '\007') {
            osc_dispatch(term);
            term->state = ST_GROUND;
        } else if (c >= 0x20 && c < 0x7F) {
            if (term->osc_len < 255) term->osc_buf[term->osc_len++] = c;
        }
        break;

    default:
        term->state = ST_GROUND;
        break;
    }
}

void terminal_write(terminal_t *term, const char *data, size_t len) {
    if (len == (size_t)-1) len = strlen(data);

    bool locked = false;
    if (s_term_write_lock) {
        locked = (xSemaphoreTake(s_term_write_lock, pdMS_TO_TICKS(5000)) == pdTRUE);
    }

    for (size_t i = 0; i < len; i++)
        parse_char(term, (uint8_t)data[i]);

    if (locked) {
        xSemaphoreGive(s_term_write_lock);
    }
}

static void draw_cell_glyph(const terminal_t *term,
                            uint16_t *canvas,
                            int stride,
                            int cw,
                            int ch,
                            int cell_x,
                            int cell_y,
                            uint32_t code,
                            uint16_t color,
                            uint16_t surface_bg) {
    if (code == 0x2328) code = 0xF11C;
    if (code == 0xE348) code = 0xF15B;
    if (code == 0xF426) code = 0xF15B;
    if (code == 0xF0B37) code = 0xF15B;
    if (code == 0xF12B7) code = 0xF15B;
    if (code == 0xF1064) code = 0xF15B;
    if (code == 0x2750) code = '*';
    if (code < 0x20) return;

    uint16_t fg = compensate_text_color(color, surface_bg);
    uint16_t halo = 0;
    if (kEnableGlyphHalo) {
        halo = halo_color(surface_bg, fg);
    }

    auto plot_in_cell = [&](int x, int y, uint16_t px) {
        if (x < cell_x || x >= cell_x + term->font_w) return;
        if (y < cell_y || y >= cell_y + term->font_h) return;
        if (x < 0 || x >= cw || y < 0 || y >= ch) return;
        canvas[y * stride + x] = px;
    };

    /* Block Elements (U+2580-U+259F): render synthetically so they tile the
     * full cell exactly. The Cozette/LVGL bitmaps are sized for a 6x13 cell
     * and leave 1px horizontal / 2px vertical gaps inside our 8x15 cell,
     * which makes solid block art (e.g. opencode splash) look dashed. */
    if (code >= 0x2580 && code <= 0x259F) {
        const int x0 = cell_x;
        const int y0 = cell_y;
        const int w = term->font_w;
        const int h = term->font_h;
        const int xm = x0 + w / 2;       /* vertical mid split */
        const int ym = y0 + h / 2;       /* horizontal mid split */

        auto fill_rect = [&](int rx0, int ry0, int rx1, int ry1, uint16_t px) {
            for (int y = ry0; y < ry1; y++)
                for (int x = rx0; x < rx1; x++)
                    plot_in_cell(x, y, px);
        };

        /* Lower n/8 block fills bottom fraction; upper-eighth fills a thin top
         * band. Eighth boundary rounded to nearest row. */
        auto eighth_row = [&](int n) { return y0 + ((8 - n) * h + 4) / 8; };
        auto eighth_col = [&](int n) { return x0 + (n * w + 4) / 8; };

        switch (code) {
        case 0x2580: fill_rect(x0, y0, x0 + w, ym, fg); break;          /* upper half */
        case 0x2581: fill_rect(x0, eighth_row(1), x0 + w, y0 + h, fg); break;
        case 0x2582: fill_rect(x0, eighth_row(2), x0 + w, y0 + h, fg); break;
        case 0x2583: fill_rect(x0, eighth_row(3), x0 + w, y0 + h, fg); break;
        case 0x2584: fill_rect(x0, ym, x0 + w, y0 + h, fg); break;      /* lower half */
        case 0x2585: fill_rect(x0, eighth_row(5), x0 + w, y0 + h, fg); break;
        case 0x2586: fill_rect(x0, eighth_row(6), x0 + w, y0 + h, fg); break;
        case 0x2587: fill_rect(x0, eighth_row(7), x0 + w, y0 + h, fg); break;
        case 0x2588: fill_rect(x0, y0, x0 + w, y0 + h, fg); break;      /* full block */
        case 0x2589: fill_rect(x0, y0, eighth_col(7), y0 + h, fg); break;
        case 0x258A: fill_rect(x0, y0, eighth_col(6), y0 + h, fg); break;
        case 0x258B: fill_rect(x0, y0, eighth_col(5), y0 + h, fg); break;
        case 0x258C: fill_rect(x0, y0, xm, y0 + h, fg); break;          /* left half */
        case 0x258D: fill_rect(x0, y0, eighth_col(3), y0 + h, fg); break;
        case 0x258E: fill_rect(x0, y0, eighth_col(2), y0 + h, fg); break;
        case 0x258F: fill_rect(x0, y0, eighth_col(1), y0 + h, fg); break;
        case 0x2590: fill_rect(xm, y0, x0 + w, y0 + h, fg); break;      /* right half */
        case 0x2591: fill_rect(x0, y0, x0 + w, y0 + h, blend_565(fg, surface_bg, 1, 4)); break; /* light shade */
        case 0x2592: fill_rect(x0, y0, x0 + w, y0 + h, blend_565(fg, surface_bg, 2, 4)); break; /* medium shade */
        case 0x2593: fill_rect(x0, y0, x0 + w, y0 + h, blend_565(fg, surface_bg, 3, 4)); break; /* dark shade */
        case 0x2594: fill_rect(x0, y0, x0 + w, y0 + (h + 4) / 8, fg); break; /* upper 1/8 */
        case 0x2595: fill_rect(eighth_col(7), y0, x0 + w, y0 + h, fg); break; /* right 1/8 */
        case 0x2596: fill_rect(x0, ym, xm, y0 + h, fg); break;          /* quadrant lower left */
        case 0x2597: fill_rect(xm, ym, x0 + w, y0 + h, fg); break;      /* quadrant lower right */
        case 0x2598: fill_rect(x0, y0, xm, ym, fg); break;             /* quadrant upper left */
        case 0x2599: fill_rect(x0, y0, xm, ym, fg);                     /* UL+LL+LR */
                     fill_rect(x0, ym, x0 + w, y0 + h, fg); break;
        case 0x259A: fill_rect(x0, y0, xm, ym, fg);                     /* UL+LR */
                     fill_rect(xm, ym, x0 + w, y0 + h, fg); break;
        case 0x259B: fill_rect(x0, y0, x0 + w, ym, fg);                 /* UL+UR+LL */
                     fill_rect(x0, ym, xm, y0 + h, fg); break;
        case 0x259C: fill_rect(x0, y0, x0 + w, ym, fg);                 /* UL+UR+LR */
                     fill_rect(xm, ym, x0 + w, y0 + h, fg); break;
        case 0x259D: fill_rect(xm, y0, x0 + w, ym, fg); break;          /* quadrant upper right */
        case 0x259E: fill_rect(xm, y0, x0 + w, ym, fg);                 /* UR+LL */
                     fill_rect(x0, ym, xm, y0 + h, fg); break;
        case 0x259F: fill_rect(xm, y0, x0 + w, ym, fg);                 /* UR+LL+LR */
                     fill_rect(x0, ym, x0 + w, y0 + h, fg); break;
        default: break;
        }
        return;
    }

    /* Box Drawing (U+2500-U+257F): render synthetically so lines tile across
     * cell boundaries. The Cozette bitmaps are 13px tall in a 15px cell, so
     * vertical runs (e.g. the input-box left border) show 2px gaps and look
     * dashed. Stems are described per-codepoint as up/right/down/left weights
     * plus dash count, arc and diagonal flags. Weight: 0=none 1=light
     * 2=heavy 3=double. */
    if (code >= 0x2500 && code <= 0x257F) {
        struct box_def_t {
            uint8_t up, right, down, left; /* stem weights */
            uint8_t dash;                  /* 0=solid, else dash segment count */
            uint8_t arc;                   /* 1=rounded corner */
            int8_t diag;                   /* 0=none 1='\' 2='/' 3='X' */
        };
        /* Table indexed by (code - 0x2500). */
        static const box_def_t kBox[128] = {
            /* 2500 ─ */ {0,1,0,1,0,0,0}, /* 2501 ━ */ {0,2,0,2,0,0,0},
            /* 2502 │ */ {1,0,1,0,0,0,0}, /* 2503 ┃ */ {2,0,2,0,0,0,0},
            /* 2504 ┄ */ {0,1,0,1,3,0,0}, /* 2505 ┅ */ {0,2,0,2,3,0,0},
            /* 2506 ┆ */ {1,0,1,0,3,0,0}, /* 2507 ┇ */ {2,0,2,0,3,0,0},
            /* 2508 ┈ */ {0,1,0,1,4,0,0}, /* 2509 ┉ */ {0,2,0,2,4,0,0},
            /* 250A ┊ */ {1,0,1,0,4,0,0}, /* 250B ┋ */ {2,0,2,0,4,0,0},
            /* 250C ┌ */ {0,1,1,0,0,0,0}, /* 250D ┍ */ {0,2,1,0,0,0,0},
            /* 250E ┎ */ {0,1,2,0,0,0,0}, /* 250F ┏ */ {0,2,2,0,0,0,0},
            /* 2510 ┐ */ {0,0,1,1,0,0,0}, /* 2511 ┑ */ {0,0,1,2,0,0,0},
            /* 2512 ┒ */ {0,0,2,1,0,0,0}, /* 2513 ┓ */ {0,0,2,2,0,0,0},
            /* 2514 └ */ {1,1,0,0,0,0,0}, /* 2515 ┕ */ {1,2,0,0,0,0,0},
            /* 2516 ┖ */ {2,1,0,0,0,0,0}, /* 2517 ┗ */ {2,2,0,0,0,0,0},
            /* 2518 ┘ */ {1,0,0,1,0,0,0}, /* 2519 ┙ */ {1,0,0,2,0,0,0},
            /* 251A ┚ */ {2,0,0,1,0,0,0}, /* 251B ┛ */ {2,0,0,2,0,0,0},
            /* 251C ├ */ {1,1,1,0,0,0,0}, /* 251D ┝ */ {1,2,1,0,0,0,0},
            /* 251E ┞ */ {2,1,1,0,0,0,0}, /* 251F ┟ */ {1,1,2,0,0,0,0},
            /* 2520 ┠ */ {2,1,2,0,0,0,0}, /* 2521 ┡ */ {2,2,1,0,0,0,0},
            /* 2522 ┢ */ {1,2,2,0,0,0,0}, /* 2523 ┣ */ {2,2,2,0,0,0,0},
            /* 2524 ┤ */ {1,0,1,1,0,0,0}, /* 2525 ┥ */ {1,0,1,2,0,0,0},
            /* 2526 ┦ */ {2,0,1,1,0,0,0}, /* 2527 ┧ */ {1,0,2,1,0,0,0},
            /* 2528 ┨ */ {2,0,2,1,0,0,0}, /* 2529 ┩ */ {2,0,1,2,0,0,0},
            /* 252A ┪ */ {1,0,2,2,0,0,0}, /* 252B ┫ */ {2,0,2,2,0,0,0},
            /* 252C ┬ */ {0,1,1,1,0,0,0}, /* 252D ┭ */ {0,1,1,2,0,0,0},
            /* 252E ┮ */ {0,2,1,1,0,0,0}, /* 252F ┯ */ {0,2,1,2,0,0,0},
            /* 2530 ┰ */ {0,1,2,1,0,0,0}, /* 2531 ┱ */ {0,1,2,2,0,0,0},
            /* 2532 ┲ */ {0,2,2,1,0,0,0}, /* 2533 ┳ */ {0,2,2,2,0,0,0},
            /* 2534 ┴ */ {1,1,0,1,0,0,0}, /* 2535 ┵ */ {1,1,0,2,0,0,0},
            /* 2536 ┶ */ {1,2,0,1,0,0,0}, /* 2537 ┷ */ {1,2,0,2,0,0,0},
            /* 2538 ┸ */ {2,1,0,1,0,0,0}, /* 2539 ┹ */ {2,1,0,2,0,0,0},
            /* 253A ┺ */ {2,2,0,1,0,0,0}, /* 253B ┻ */ {2,2,0,2,0,0,0},
            /* 253C ┼ */ {1,1,1,1,0,0,0}, /* 253D ┽ */ {1,1,1,2,0,0,0},
            /* 253E ┾ */ {1,2,1,1,0,0,0}, /* 253F ┿ */ {1,2,1,2,0,0,0},
            /* 2540 ╀ */ {2,1,1,1,0,0,0}, /* 2541 ╁ */ {1,1,2,1,0,0,0},
            /* 2542 ╂ */ {2,1,2,1,0,0,0}, /* 2543 ╃ */ {2,1,1,2,0,0,0},
            /* 2544 ╄ */ {2,2,1,1,0,0,0}, /* 2545 ╅ */ {1,1,2,2,0,0,0},
            /* 2546 ╆ */ {1,2,2,1,0,0,0}, /* 2547 ╇ */ {2,2,1,2,0,0,0},
            /* 2548 ╈ */ {1,2,2,2,0,0,0}, /* 2549 ╉ */ {2,1,2,2,0,0,0},
            /* 254A ╊ */ {2,2,2,1,0,0,0}, /* 254B ╋ */ {2,2,2,2,0,0,0},
            /* 254C ╌ */ {0,1,0,1,2,0,0}, /* 254D ╍ */ {0,2,0,2,2,0,0},
            /* 254E ╎ */ {1,0,1,0,2,0,0}, /* 254F ╏ */ {2,0,2,0,2,0,0},
            /* 2550 ═ */ {0,3,0,3,0,0,0}, /* 2551 ║ */ {3,0,3,0,0,0,0},
            /* 2552 ╒ */ {0,3,1,0,0,0,0}, /* 2553 ╓ */ {0,1,3,0,0,0,0},
            /* 2554 ╔ */ {0,3,3,0,0,0,0}, /* 2555 ╕ */ {0,0,1,3,0,0,0},
            /* 2556 ╖ */ {0,0,3,1,0,0,0}, /* 2557 ╗ */ {0,0,3,3,0,0,0},
            /* 2558 ╘ */ {1,3,0,0,0,0,0}, /* 2559 ╙ */ {3,1,0,0,0,0,0},
            /* 255A ╚ */ {3,3,0,0,0,0,0}, /* 255B ╛ */ {1,0,0,3,0,0,0},
            /* 255C ╜ */ {3,0,0,1,0,0,0}, /* 255D ╝ */ {3,0,0,3,0,0,0},
            /* 255E ╞ */ {1,3,1,0,0,0,0}, /* 255F ╟ */ {3,1,3,0,0,0,0},
            /* 2560 ╠ */ {3,3,3,0,0,0,0}, /* 2561 ╡ */ {1,0,1,3,0,0,0},
            /* 2562 ╢ */ {3,0,3,1,0,0,0}, /* 2563 ╣ */ {3,0,3,3,0,0,0},
            /* 2564 ╤ */ {0,3,1,3,0,0,0}, /* 2565 ╥ */ {0,1,3,1,0,0,0},
            /* 2566 ╦ */ {0,3,3,3,0,0,0}, /* 2567 ╧ */ {1,3,0,3,0,0,0},
            /* 2568 ╨ */ {3,1,0,1,0,0,0}, /* 2569 ╩ */ {3,3,0,3,0,0,0},
            /* 256A ╪ */ {1,3,1,3,0,0,0}, /* 256B ╫ */ {3,1,3,1,0,0,0},
            /* 256C ╬ */ {3,3,3,3,0,0,0},
            /* 256D ╭ */ {0,1,1,0,0,1,0}, /* 256E ╮ */ {0,0,1,1,0,1,0},
            /* 256F ╯ */ {1,0,0,1,0,1,0}, /* 2570 ╰ */ {1,1,0,0,0,1,0},
            /* 2571 ╱ */ {0,0,0,0,0,0,2}, /* 2572 ╲ */ {0,0,0,0,0,0,1},
            /* 2573 ╳ */ {0,0,0,0,0,0,3},
            /* 2574 ╴ */ {0,0,0,1,0,0,0}, /* 2575 ╵ */ {1,0,0,0,0,0,0},
            /* 2576 ╶ */ {0,1,0,0,0,0,0}, /* 2577 ╷ */ {0,0,1,0,0,0,0},
            /* 2578 ╸ */ {0,0,0,2,0,0,0}, /* 2579 ╹ */ {2,0,0,0,0,0,0},
            /* 257A ╺ */ {0,2,0,0,0,0,0}, /* 257B ╻ */ {0,0,2,0,0,0,0},
            /* 257C ╼ */ {0,2,0,1,0,0,0}, /* 257D ╽ */ {1,0,2,0,0,0,0},
            /* 257E ╾ */ {0,1,0,2,0,0,0}, /* 257F ╿ */ {2,0,1,0,0,0,0},
        };

        const box_def_t d = kBox[code - 0x2500];
        const int x0 = cell_x, y0 = cell_y;
        const int w = term->font_w, h = term->font_h;
        const int xc = x0 + w / 2;     /* center column */
        const int yc = y0 + h / 2;     /* center row */
        /* Double-line parallel offset (>=1). */
        const int dbl = (w >= 8 || h >= 8) ? 2 : 1;

        auto fill_rect = [&](int rx0, int ry0, int rx1, int ry1) {
            for (int y = ry0; y < ry1; y++)
                for (int x = rx0; x < rx1; x++)
                    plot_in_cell(x, y, fg);
        };
        /* Vertical span [ry0,ry1) of a stem with given weight at center col. */
        auto vstem = [&](int ry0, int ry1, int wt) {
            if (wt == 0) return;
            if (wt == 3) {                 /* double */
                fill_rect(xc - dbl, ry0, xc - dbl + 1, ry1);
                fill_rect(xc + dbl, ry0, xc + dbl + 1, ry1);
            } else if (wt == 2) {          /* heavy */
                fill_rect(xc - 1, ry0, xc + 2, ry1);
            } else {                       /* light */
                fill_rect(xc, ry0, xc + 1, ry1);
            }
        };
        auto hstem = [&](int rx0, int rx1, int wt) {
            if (wt == 0) return;
            if (wt == 3) {
                fill_rect(rx0, yc - dbl, rx1, yc - dbl + 1);
                fill_rect(rx0, yc + dbl, rx1, yc + dbl + 1);
            } else if (wt == 2) {
                fill_rect(rx0, yc - 1, rx1, yc + 2);
            } else {
                fill_rect(rx0, yc, rx1, yc + 1);
            }
        };

        /* Diagonals. */
        if (d.diag) {
            auto line = [&](int ax, int ay, int bx, int by) {
                int dx = bx - ax, dy = by - ay;
                int steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy)
                              ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
                if (steps == 0) { plot_in_cell(ax, ay, fg); return; }
                for (int s = 0; s <= steps; s++) {
                    int px = ax + dx * s / steps;
                    int py = ay + dy * s / steps;
                    plot_in_cell(px, py, fg);
                    plot_in_cell(px + 1, py, fg); /* 2px wide for visibility */
                }
            };
            if (d.diag == 1 || d.diag == 3) line(x0, y0, x0 + w - 1, y0 + h - 1);
            if (d.diag == 2 || d.diag == 3) line(x0 + w - 1, y0, x0, y0 + h - 1);
            return;
        }

        /* Rounded corners: draw the two stems to center, plus a small arc
         * approximation by trimming the inner corner. Falls back to square
         * stems if cell too small. */
        if (d.arc) {
            /* Determine which two directions are present and route stems to a
             * point near center, joined with a short diagonal for the curve. */
            if (d.down)  vstem(yc, y0 + h, 1);
            if (d.up)    vstem(y0, yc + 1, 1);
            if (d.right) hstem(xc, x0 + w, 1);
            if (d.left)  hstem(x0, xc + 1, 1);
            return;
        }

        /* Dashed lines: split the stem run into segments. */
        if (d.dash) {
            int segs = d.dash;
            if (d.left || d.right) {
                int wt = d.right ? d.right : d.left;
                int span = w;
                int on = (span / segs) - 1; if (on < 1) on = 1;
                for (int s = 0; s < segs; s++) {
                    int sx = x0 + s * span / segs;
                    hstem(sx, sx + on, wt);
                }
            }
            if (d.up || d.down) {
                int wt = d.down ? d.down : d.up;
                int span = h;
                int on = (span / segs) - 1; if (on < 1) on = 1;
                for (int s = 0; s < segs; s++) {
                    int sy = y0 + s * span / segs;
                    vstem(sy, sy + on, wt);
                }
            }
            return;
        }

        /* Solid stems from center to the relevant edges. Each present stem
         * extends through the center so junctions connect cleanly. */
        if (d.up)    vstem(y0, yc + 1, d.up);
        if (d.down)  vstem(yc, y0 + h, d.down);
        if (d.left)  hstem(x0, xc + 1, d.left);
        if (d.right) hstem(xc, x0 + w, d.right);
        return;
    }

    if (term->bitmap_font && !is_private_use_codepoint(code)) {
        const cozette_bdf_glyph_t *bg = cozette_bdf_lookup(term->bitmap_font, code);
        if (bg && bg->box_w > 0 && bg->box_h > 0) {
            int bytes_per_row = (bg->box_w + 7) / 8;
            int vpad = (term->font_h - (int)term->bitmap_font->line_height) > 0
                         ? (term->font_h - (int)term->bitmap_font->line_height) / 2
                         : 0;
            int gy = cell_y + vpad + (int)term->bitmap_font->ascent - (int)bg->box_h - (int)bg->ofs_y;
            int gx = cell_x + (int)bg->ofs_x;
            const uint8_t *src = term->bitmap_font->bitmap + bg->bitmap_index;

            for (int py = 0; py < (int)bg->box_h && gy + py < ch; py++) {
                int yp = gy + py;
                if (yp < 0) continue;
                const uint8_t *row = src + py * bytes_per_row;
                for (int px = 0; px < (int)bg->box_w && gx + px < cw; px++) {
                    int xp = gx + px;
                    uint8_t b = row[px >> 3];
                    if (kEnableGlyphHalo && (b & (0x80 >> (px & 7)))) {
                        for (int dy = -1; dy <= 1; dy++)
                            for (int dx = -1; dx <= 1; dx++)
                                if (dx || dy) plot_in_cell(xp + dx, yp + dy, halo);
                    }
                }
            }

            for (int py = 0; py < (int)bg->box_h && gy + py < ch; py++) {
                int yp = gy + py;
                if (yp < 0) continue;
                const uint8_t *row = src + py * bytes_per_row;
                for (int px = 0; px < (int)bg->box_w && gx + px < cw; px++) {
                    int xp = gx + px;
                    uint8_t b = row[px >> 3];
                    if (b & (0x80 >> (px & 7))) {
                        plot_in_cell(xp, yp, fg);
                    }
                }
            }
            return;
        }
    }

    lv_font_glyph_dsc_t dsc;
    const lv_font_t *active_font = NULL;
    if (!resolve_glyph(term, code, &dsc, &active_font)) {
        if (code >= 0x80 && s_missing_glyph_budget > 0) {
            ESP_LOGW(TAG, "Missing glyph U+%04X", (unsigned)code);
            s_missing_glyph_budget--;
        }
        return;
    }
    if (dsc.box_w == 0 || dsc.box_h == 0) return;

    lv_draw_buf_t *glyph_buf = lv_draw_buf_create(dsc.box_w, dsc.box_h, LV_COLOR_FORMAT_A8, 0);
    if (!glyph_buf) return;

    const void *ret = lv_font_get_glyph_bitmap(&dsc, glyph_buf);
    if (!ret || !glyph_buf->data) {
        lv_draw_buf_destroy(glyph_buf);
        return;
    }

    const int font_h = active_font->line_height;
    const int ascent = active_font->line_height - active_font->base_line;
    const int vpad = (term->font_h - font_h) > 0 ? (term->font_h - font_h) / 2 : 0;
    const int gx = cell_x + dsc.ofs_x;
    const int gy = cell_y + vpad + ascent - dsc.box_h - dsc.ofs_y;
    const uint8_t *a8 = (const uint8_t *)glyph_buf->data;
    const int gstride = lv_draw_buf_width_to_stride(dsc.box_w, LV_COLOR_FORMAT_A8);

    for (int py = 0; py < dsc.box_h && gy + py < ch; py++) {
        int yp = gy + py;
        if (yp < 0) continue;
        for (int px = 0; px < dsc.box_w && gx + px < cw; px++) {
            int xp = gx + px;
            int a = a8[py * gstride + px];
            if (kEnableGlyphHalo && a >= GLYPH_ALPHA_THRESHOLD) {
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                        if (dx || dy) plot_in_cell(xp + dx, yp + dy, halo);
            }
        }
    }

    for (int py = 0; py < dsc.box_h && gy + py < ch; py++) {
        int yp = gy + py;
        if (yp < 0) continue;
        for (int px = 0; px < dsc.box_w && gx + px < cw; px++) {
            int xp = gx + px;
            int a = a8[py * gstride + px];
            if (a >= GLYPH_ALPHA_THRESHOLD) {
                plot_in_cell(xp, yp, fg);
            }
        }
    }

    lv_draw_buf_destroy(glyph_buf);
}

void terminal_render(terminal_t *term) {
    if (!term->canvas) return;

    term_cell_t *buf = term->alt_screen ? term->alt : term->screen;
    static int s_last_view_offset = -1;
    const int view_offset = term->view_offset;
    const bool showing_history = view_offset > 0;
    if (s_last_view_offset != view_offset) {
        mark_all_dirty(term);
        s_last_view_offset = view_offset;
    }

    bool blink_phase = ((lv_tick_get() / 500U) & 1U) == 0;
    if (term->cursor_visible && !showing_history) {
        if (term->last_cursor_row != term->cursor_row || term->last_cursor_col != term->cursor_col) {
            dirty_cell(term, term->last_cursor_row, term->last_cursor_col);
            dirty_cell(term, term->cursor_row, term->cursor_col);
            term->last_cursor_row = term->cursor_row;
            term->last_cursor_col = term->cursor_col;
        }
        if (term->cursor_blink_phase != blink_phase) {
            term->cursor_blink_phase = blink_phase;
            dirty_cell(term, term->cursor_row, term->cursor_col);
        }
    }

    bool has_dirty = false;
    for (int r = 0; r < term->rows && !has_dirty; r++)
        for (int c = 0; c < term->cols && !has_dirty; c++)
            if (buf[r * term->cols + c].dirty) has_dirty = true;
    if (!has_dirty) return;

    lv_draw_buf_t *db = lv_canvas_get_draw_buf(term->canvas);
    if (!db) return;
    uint16_t *canvas = (uint16_t *)db->data;
    int stride = db->header.stride / 2;
    int cw = term->cols * term->font_w;
    int ch = term->rows * term->font_h;

    for (int r = 0; r < term->rows; r++) {
        for (int c = 0; c < term->cols; c++) {
            term_cell_t *cp = &buf[r * term->cols + c];
            if (!cp->dirty) continue;

            const term_cell_t *src = showing_history ? view_cell(term, buf, r, c, view_offset) : cp;

            int cell_x = c * term->font_w;
            int cell_y = r * term->font_h;

            uint16_t bg = src->bg;
            for (int py = cell_y; py < cell_y + term->font_h && py < ch; py++)
                for (int px = cell_x; px < cell_x + term->font_w && px < cw; px++)
                    canvas[py * stride + px] = bg;

            draw_cell_glyph(term, canvas, stride, cw, ch, cell_x, cell_y, src->code, src->fg, src->bg);

            cp->dirty = 0;
        }
    }

    // Cursor
    if (term->cursor_visible && term->cursor_blink_phase && !showing_history) {
        int cr = term->cursor_row;
        int cc = term->cursor_col;
        if (cr >= 0 && cr < term->rows && cc >= 0 && cc < term->cols) {
            int cx = cc * term->font_w;
            int cy = cr * term->font_h;
            term_cell_t *cp = &buf[cr * term->cols + cc];

            // Draw cursor rect (inverted fg color)
            uint16_t cur_fg = cp->fg;
            for (int py = cy; py < cy + term->font_h && py < ch; py++)
                for (int px = cx; px < cx + term->font_w && px < cw; px++)
                    canvas[py * stride + px] = cur_fg;

            draw_cell_glyph(term, canvas, stride, cw, ch, cx, cy, cp->code, cp->bg, cur_fg);
        }
    }

    lv_draw_buf_flush_cache(db, NULL);
    lv_obj_invalidate(term->canvas);
}

void terminal_set_output_cb(terminal_t *term, void (*cb)(const char *, size_t)) {
    term->output_cb = cb;
}

void terminal_scrollback_step(terminal_t *term, int delta_lines) {
    if (!term || term->scrollback_count <= 0) return;
    int new_offset = term->view_offset + delta_lines;
    if (new_offset < 0) new_offset = 0;
    if (new_offset > term->scrollback_count) new_offset = term->scrollback_count;
    if (new_offset == term->view_offset) return;
    term->view_offset = new_offset;
    mark_all_dirty(term);
}

void terminal_scrollback_reset(terminal_t *term) {
    if (!term) return;
    if (term->view_offset == 0) return;
    term->view_offset = 0;
    mark_all_dirty(term);
}

bool terminal_scrollback_active(const terminal_t *term) {
    return term && term->view_offset > 0;
}

// Remap every cell whose fg matches old_fg to new_fg, across the screen, alt,
// and scrollback buffers, then mark the visible buffer for redraw. This
// recolors text that was drawn in the default color (prompt, command output)
// while leaving explicitly-colored text untouched.
static void recolor_default_fg(terminal_t *term, uint16_t old_fg, uint16_t new_fg) {
    if (!term || old_fg == new_fg) return;

    bool locked = false;
    if (s_term_write_lock) {
        locked = (xSemaphoreTake(s_term_write_lock, pdMS_TO_TICKS(5000)) == pdTRUE);
    }

    // If the live pen is still on the old default, move it to the new default
    // so subsequent un-colored output uses the new color too.
    if (term->cur_fg == old_fg) term->cur_fg = new_fg;

    const int screen_total = term->rows * term->cols;
    if (term->screen) {
        for (int i = 0; i < screen_total; i++) {
            if (term->screen[i].fg == old_fg) { term->screen[i].fg = new_fg; term->screen[i].dirty = 1; }
        }
    }
    if (term->alt) {
        for (int i = 0; i < screen_total; i++) {
            if (term->alt[i].fg == old_fg) { term->alt[i].fg = new_fg; term->alt[i].dirty = 1; }
        }
    }
    if (term->scrollback) {
        const int sb_total = term->scrollback_capacity * term->cols;
        for (int i = 0; i < sb_total; i++) {
            if (term->scrollback[i].fg == old_fg) term->scrollback[i].fg = new_fg;
        }
    }
    mark_all_dirty(term);

    if (locked) {
        xSemaphoreGive(s_term_write_lock);
    }
}

void terminal_set_default_fg_index(int index) {
    if (index < 0) index = 0;
    if (index > 255) index = 255;
    if (index == s_default_fg_index) return;

    uint16_t old_fg = default_fg();
    s_default_fg_index = index;
    uint16_t new_fg = default_fg();

    recolor_default_fg(s_active_term, old_fg, new_fg);
}

uint32_t terminal_color_256_rgb888(int index) {
    if (index < 0) index = 0;
    if (index > 255) index = 255;
    uint16_t c = color_256(index);
    int r5 = (c >> 11) & 0x1F;
    int g6 = (c >> 5) & 0x3F;
    int b5 = c & 0x1F;
    int r = (r5 * 255 + 15) / 31;
    int g = (g6 * 255 + 31) / 63;
    int b = (b5 * 255 + 15) / 31;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

int terminal_get_default_fg_index(void) {
    return s_default_fg_index;
}

// NVS persistence for the default-fg color index. Stored as a u8 in the
// shared "devicecfg" namespace under key "termfg".
static constexpr char TERM_CFG_NS[] = "devicecfg";
static constexpr char TERM_FG_KEY[] = "termfg";

void terminal_load_default_fg_index(void) {
    nvs_handle_t nvs = 0;
    if (nvs_open(TERM_CFG_NS, NVS_READONLY, &nvs) != ESP_OK) return;
    uint8_t idx = DEFAULT_FG_INDEX_DEFAULT;
    if (nvs_get_u8(nvs, TERM_FG_KEY, &idx) == ESP_OK) {
        terminal_set_default_fg_index(idx);
    }
    nvs_close(nvs);
}

esp_err_t terminal_save_default_fg_index(int index) {
    if (index < 0) index = 0;
    if (index > 255) index = 255;
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(TERM_CFG_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(nvs, TERM_FG_KEY, (uint8_t)index);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}
