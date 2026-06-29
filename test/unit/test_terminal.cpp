/* Unit tests for main/terminal.cpp.
 *
 * Strategy: terminal_write() feeds the full escape/CSI/OSC/UTF-8 parser, which
 * is hardware-independent. We construct a terminal_t with host-allocated
 * buffers (bypassing terminal_init(), which needs LVGL) and drive bytes
 * through the public terminal_write() API, then assert on the resulting screen
 * cells / cursor / attribute state. The public 256-color helpers are tested
 * directly. */
#include "gtest/gtest.h"

extern "C" {
#include "terminal.hpp"
#include "nvs.h"
}

#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

// Build a usable terminal_t without terminal_init()'s LVGL/canvas setup.
class Term {
public:
    explicit Term(int cols = 20, int rows = 6) {
        memset(&t, 0, sizeof(t));
        t.cols = cols;
        t.rows = rows;
        t.scroll_top = 0;
        t.scroll_bottom = rows - 1;
        t.wraparound = true;
        t.cursor_visible = true;
        t.cur_fg = 0xFFFF;  // white-ish default for tests
        t.cur_bg = 0;
        t.scrollback_capacity = 8;
        t.screen = (term_cell_t *)calloc(rows * cols, sizeof(term_cell_t));
        t.alt = (term_cell_t *)calloc(rows * cols, sizeof(term_cell_t));
        t.scrollback = (term_cell_t *)calloc(t.scrollback_capacity * cols, sizeof(term_cell_t));
    }
    ~Term() {
        free(t.screen);
        free(t.alt);
        free(t.scrollback);
    }
    void write(const std::string &s) { terminal_write(&t, s.data(), s.size()); }
    void write(const char *s, size_t n) { terminal_write(&t, s, n); }

    term_cell_t *cell(int r, int c) {
        term_cell_t *buf = t.alt_screen ? t.alt : t.screen;
        return &buf[r * t.cols + c];
    }
    std::string row_text(int r) {
        std::string out;
        for (int c = 0; c < t.cols; c++) {
            uint32_t code = cell(r, c)->code;
            out.push_back(code ? (char)code : ' ');
        }
        return out;
    }
    terminal_t t;
};

std::string g_cb;
void capture_cb(const char *data, size_t len) { g_cb.append(data, len); }

}  // namespace

/* --------------------------- plain text ---------------------------- */
TEST(Terminal, PrintsAsciiAtCursor) {
    Term term;
    term.write("Hi");
    EXPECT_EQ(term.cell(0, 0)->code, 'H');
    EXPECT_EQ(term.cell(0, 1)->code, 'i');
    EXPECT_EQ(term.t.cursor_col, 2);
    EXPECT_EQ(term.t.cursor_row, 0);
}

TEST(Terminal, CarriageReturnAndLineFeed) {
    Term term;
    term.write("ab\r\ncd");
    EXPECT_EQ(term.row_text(0).substr(0, 2), "ab");
    EXPECT_EQ(term.row_text(1).substr(0, 2), "cd");
    EXPECT_EQ(term.t.cursor_row, 1);
}

TEST(Terminal, BackspaceMovesCursorLeft) {
    Term term;
    term.write("ab\b");
    EXPECT_EQ(term.t.cursor_col, 1);
}

TEST(Terminal, TabAdvancesToNextStop) {
    Term term;
    term.write("a\t");
    EXPECT_EQ(term.t.cursor_col, 8);
}

TEST(Terminal, WrapsAtRightMargin) {
    Term term(4, 4);
    term.write("abcde");  // 5 chars in 4 cols -> wrap to row 1
    EXPECT_EQ(term.t.cursor_row, 1);
    EXPECT_EQ(term.cell(1, 0)->code, 'e');
}

/* --------------------------- CSI cursor ---------------------------- */
TEST(Terminal, CursorPositionAbsolute) {
    Term term;
    term.write("\033[3;5H");  // row 3 col 5 (1-based)
    EXPECT_EQ(term.t.cursor_row, 2);
    EXPECT_EQ(term.t.cursor_col, 4);
}

TEST(Terminal, CursorForwardAndUp) {
    Term term;
    term.write("\033[5C");  // forward 5
    EXPECT_EQ(term.t.cursor_col, 5);
    term.write("\033[2;1H\033[1A");  // to (2,1) then up 1
    EXPECT_EQ(term.t.cursor_row, 0);
}

TEST(Terminal, EraseInLineClearsToEnd) {
    Term term(10, 3);
    term.write("hello");
    term.write("\033[1;3H");  // cursor at col 3 (0-based col 2)
    term.write("\033[0K");    // erase to end of line
    EXPECT_EQ(term.cell(0, 0)->code, 'h');
    EXPECT_EQ(term.cell(0, 1)->code, 'e');
    // col 2.. cleared to space
    EXPECT_EQ(term.cell(0, 2)->code, ' ');
    EXPECT_EQ(term.cell(0, 4)->code, ' ');
}

TEST(Terminal, EraseDisplayClearsScreen) {
    Term term(6, 3);
    term.write("abc\r\ndef");
    term.write("\033[2J");
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 6; c++)
            EXPECT_EQ(term.cell(r, c)->code, ' ');
}

/* --------------------------- SGR / color --------------------------- */
TEST(Terminal, SgrBoldAndReset) {
    Term term;
    term.write("\033[1m");
    EXPECT_EQ(term.t.cur_bold, 1u);
    term.write("\033[0m");
    EXPECT_EQ(term.t.cur_bold, 0u);
}

TEST(Terminal, SgrUnderlineItalicReverse) {
    Term term;
    term.write("\033[3;4;7m");
    EXPECT_EQ(term.t.cur_italic, 1u);
    EXPECT_EQ(term.t.cur_underline, 1u);
    EXPECT_EQ(term.t.cur_reverse, 1u);
    term.write("\033[23;24;27m");
    EXPECT_EQ(term.t.cur_italic, 0u);
    EXPECT_EQ(term.t.cur_underline, 0u);
    EXPECT_EQ(term.t.cur_reverse, 0u);
}

TEST(Terminal, Sgr256ColorForeground) {
    Term term;
    term.write("\033[38;5;196m");  // bright red in 256 palette
    // index 196 -> cube color, should be a non-zero RGB565
    EXPECT_NE(term.t.cur_fg, 0);
    term.write("X");
    EXPECT_EQ(term.cell(0, 0)->fg, term.t.cur_fg);
}

TEST(Terminal, SgrTrueColorForeground) {
    Term term;
    term.write("\033[38;2;255;0;0m");  // pure red truecolor
    // RGB565 for (255,0,0) == 0xF800
    EXPECT_EQ(term.t.cur_fg, 0xF800);
}

TEST(Terminal, SgrBasicAnsiColors) {
    Term term;
    term.write("\033[31m");  // red fg
    uint16_t red = term.t.cur_fg;
    term.write("\033[39m");  // default fg
    EXPECT_NE(term.t.cur_fg, red);
}

/* --------------------------- UTF-8 --------------------------------- */
TEST(Terminal, Utf8TwoByteDecoded) {
    Term term;
    term.write("\xC3\xA9");  // U+00E9 e-acute
    EXPECT_EQ(term.cell(0, 0)->code, 0xE9u);
}

TEST(Terminal, Utf8ThreeByteDecoded) {
    Term term;
    term.write("\xE2\x82\xAC");  // U+20AC euro sign
    EXPECT_EQ(term.cell(0, 0)->code, 0x20ACu);
}

TEST(Terminal, InvalidUtf8ContinuationResyncsToAscii) {
    Term term;
    // 0xC3 starts a 2-byte sequence; 0x41 ('A') is not a valid continuation.
    // The parser abandons the partial sequence and prints the ASCII byte.
    term.write("\xC3\x41");
    EXPECT_EQ(term.cell(0, 0)->code, 'A');
}

TEST(Terminal, LoneContinuationByteBecomesReplacement) {
    Term term;
    // A continuation byte (0x80-0xBF) with no lead byte -> '?'.
    term.write("\x80");
    EXPECT_EQ(term.cell(0, 0)->code, '?');
}

/* --------------------------- OSC queries --------------------------- */
TEST(Terminal, Osc11BackgroundQueryReplies) {
    Term term;
    g_cb.clear();
    terminal_set_output_cb(&term.t, capture_cb);
    term.write("\033]11;?\033\\");  // OSC 11 query, ST-terminated
    EXPECT_NE(g_cb.find("rgb:0000/0000/0000"), std::string::npos);
}

TEST(Terminal, Osc10ForegroundQueryReplies) {
    Term term;
    g_cb.clear();
    terminal_set_output_cb(&term.t, capture_cb);
    term.write("\033]10;?\033\\");
    EXPECT_NE(g_cb.find("\033]10;rgb:"), std::string::npos);
}

/* --------------------------- DEC special graphics ------------------ */
TEST(Terminal, DecSpecialLineDrawing) {
    Term term;
    term.write("\033(0");  // G0 = DEC special graphics
    term.write("q");        // maps to U+2500 horizontal line
    EXPECT_EQ(term.cell(0, 0)->code, 0x2500u);
    term.write("\033(B");  // back to ASCII
}

/* --------------------------- DCS consumed -------------------------- */
TEST(Terminal, DcsStringSilentlyConsumed) {
    Term term;
    term.write("\033P1$r0m\033\\X");  // DCS ... ST then 'X'
    EXPECT_EQ(term.cell(0, 0)->code, 'X');
    EXPECT_EQ(term.t.cursor_col, 1);
}

/* --------------------------- save/restore cursor ------------------- */
TEST(Terminal, SaveRestoreCursor) {
    Term term;
    term.write("\033[3;3H");  // move
    term.write("\0337");      // save (ESC 7)
    term.write("\033[1;1H");  // move home
    term.write("\0338");      // restore
    EXPECT_EQ(term.t.cursor_row, 2);
    EXPECT_EQ(term.t.cursor_col, 2);
}

/* --------------------------- public color API ---------------------- */
TEST(TerminalColor, BasicAnsiIndexMapsToExpectedRgb) {
    // Index 0 is black, index 15 is white in the base 16-color block.
    EXPECT_EQ(terminal_color_256_rgb888(0), 0x000000u);
    uint32_t white = terminal_color_256_rgb888(15);
    EXPECT_GT((white >> 16) & 0xFF, 0xF0u);
    EXPECT_GT((white >> 8) & 0xFF, 0xF0u);
    EXPECT_GT(white & 0xFF, 0xF0u);
}

TEST(TerminalColor, CubeRedHasDominantRedChannel) {
    // 6x6x6 cube entry with max red, zero green/blue: index 16 + 5*36 = 196.
    uint32_t rgb = terminal_color_256_rgb888(196);
    uint8_t r = (rgb >> 16) & 0xFF;
    uint8_t g = (rgb >> 8) & 0xFF;
    uint8_t b = rgb & 0xFF;
    EXPECT_GT(r, g);
    EXPECT_GT(r, b);
}

TEST(TerminalColor, GrayscaleRampMonotonic) {
    uint32_t low = terminal_color_256_rgb888(232);
    uint32_t high = terminal_color_256_rgb888(255);
    EXPECT_LT(low & 0xFF, high & 0xFF);
}

TEST(TerminalColor, DefaultFgIndexClampsAndPersists) {
    terminal_set_default_fg_index(-5);
    EXPECT_EQ(terminal_get_default_fg_index(), 0);
    terminal_set_default_fg_index(999);
    EXPECT_EQ(terminal_get_default_fg_index(), 255);
    terminal_set_default_fg_index(123);
    EXPECT_EQ(terminal_get_default_fg_index(), 123);
}

TEST(TerminalColor, SaveAndLoadDefaultFgViaNvs) {
    mock_nvs_reset();
    terminal_set_default_fg_index(200);
    EXPECT_EQ(terminal_save_default_fg_index(200), ESP_OK);
    terminal_set_default_fg_index(0);
    terminal_load_default_fg_index();
    EXPECT_EQ(terminal_get_default_fg_index(), 200);
}

/* --------------------- CSI cursor movement breadth ----------------- */
TEST(TerminalCsi, CursorDownBackForwardClamped) {
    Term term(10, 5);
    term.write("\033[99B");  // down far -> clamp to last row
    EXPECT_EQ(term.t.cursor_row, 4);
    term.write("\033[99C");  // forward far -> clamp to last col
    EXPECT_EQ(term.t.cursor_col, 9);
    term.write("\033[99D");  // back far -> col 0
    EXPECT_EQ(term.t.cursor_col, 0);
}

TEST(TerminalCsi, CursorNextAndPrevLine) {
    Term term(10, 5);
    term.write("\033[3;5H");  // row2 col4
    term.write("\033[1E");    // next line, col 0
    EXPECT_EQ(term.t.cursor_row, 3);
    EXPECT_EQ(term.t.cursor_col, 0);
    term.write("\033[3;5H\033[1F");  // prev line, col 0
    EXPECT_EQ(term.t.cursor_row, 1);
    EXPECT_EQ(term.t.cursor_col, 0);
}

TEST(TerminalCsi, CursorColumnAbsoluteG) {
    Term term(10, 3);
    term.write("\033[5G");  // column 5 (1-based) -> col 4
    EXPECT_EQ(term.t.cursor_col, 4);
}

TEST(TerminalCsi, CursorLineAbsoluteD) {
    Term term(10, 5);
    term.write("\033[4d");  // line 4 (1-based) -> row 3
    EXPECT_EQ(term.t.cursor_row, 3);
}

TEST(TerminalCsi, EraseInLineModes1And2) {
    Term term(8, 2);
    term.write("abcdefgh");
    term.write("\033[1;4H");  // col 3 (0-based)
    term.write("\033[1K");    // erase from start of line to cursor
    EXPECT_EQ(term.cell(0, 0)->code, ' ');
    EXPECT_EQ(term.cell(0, 3)->code, ' ');
    EXPECT_EQ(term.cell(0, 4)->code, 'e');
    term.write("\033[2K");  // erase whole line
    for (int c = 0; c < 8; c++) EXPECT_EQ(term.cell(0, c)->code, ' ');
}

TEST(TerminalCsi, EraseInDisplayMode1) {
    Term term(4, 3);
    term.write("ab\r\ncd\r\nef");
    term.write("\033[2;2H");  // row1 col1
    term.write("\033[1J");    // erase from start to cursor
    EXPECT_EQ(term.cell(0, 0)->code, ' ');
    EXPECT_EQ(term.cell(1, 1)->code, ' ');
}

TEST(TerminalCsi, EraseCharactersX) {
    Term term(8, 1);
    term.write("abcdefgh");
    term.write("\033[1;1H");
    term.write("\033[3X");  // erase 3 chars
    EXPECT_EQ(term.cell(0, 0)->code, ' ');
    EXPECT_EQ(term.cell(0, 2)->code, ' ');
    EXPECT_EQ(term.cell(0, 3)->code, 'd');
}

TEST(TerminalCsi, InsertAndDeleteCharacters) {
    Term term(8, 1);
    term.write("abcdef");
    term.write("\033[1;1H");
    term.write("\033[2@");  // insert 2 blanks at start
    EXPECT_EQ(term.cell(0, 0)->code, ' ');
    EXPECT_EQ(term.cell(0, 2)->code, 'a');
    term.write("\033[1;1H");
    term.write("\033[2P");  // delete 2 chars
    EXPECT_EQ(term.cell(0, 0)->code, 'a');
}

TEST(TerminalCsi, InsertAndDeleteLines) {
    Term term(4, 4);
    term.write("aaaa\r\nbbbb\r\ncccc");
    term.write("\033[1;1H");
    term.write("\033[1L");  // insert blank line at top
    EXPECT_EQ(term.cell(0, 0)->code, ' ');
    EXPECT_EQ(term.cell(1, 0)->code, 'a');
    term.write("\033[1;1H");
    term.write("\033[1M");  // delete top line
    EXPECT_EQ(term.cell(0, 0)->code, 'a');
}

TEST(TerminalCsi, DsrStatusReport) {
    Term term;
    g_cb.clear();
    terminal_set_output_cb(&term.t, capture_cb);
    term.write("\033[5n");  // device status report
    EXPECT_NE(g_cb.find("\033[0n"), std::string::npos);
}

TEST(TerminalCsi, CursorPositionReport) {
    Term term(20, 10);
    g_cb.clear();
    terminal_set_output_cb(&term.t, capture_cb);
    term.write("\033[3;7H");  // move to row3 col7
    term.write("\033[6n");    // request cursor position
    EXPECT_NE(g_cb.find("\033[3;7R"), std::string::npos);
}

TEST(TerminalCsi, DeviceAttributesPrimary) {
    Term term;
    g_cb.clear();
    terminal_set_output_cb(&term.t, capture_cb);
    term.write("\033[c");  // primary DA
    EXPECT_NE(g_cb.find("\033[?1;2c"), std::string::npos);
}

TEST(TerminalCsi, DeviceAttributesSecondary) {
    Term term;
    g_cb.clear();
    terminal_set_output_cb(&term.t, capture_cb);
    term.write("\033[>c");  // secondary DA
    EXPECT_NE(g_cb.find("\033[>0;136;0c"), std::string::npos);
}

TEST(TerminalCsi, AltScreenSwitchAndBack) {
    Term term(4, 2);
    term.write("main");          // on primary
    term.write("\033[?1049h");   // switch to alt (saves+clears)
    EXPECT_TRUE(term.t.alt_screen);
    term.write("alt!");          // draw on alt screen
    term.write("\033[?1049l");   // back to primary
    EXPECT_FALSE(term.t.alt_screen);
    // 1049 restore path resets cursor to home for the primary screen.
    EXPECT_EQ(term.t.cursor_row, 0);
    EXPECT_EQ(term.t.cursor_col, 0);
}

TEST(TerminalCsi, CursorVisibilityToggle) {
    Term term;
    term.write("\033[?25l");
    EXPECT_FALSE(term.t.cursor_visible);
    term.write("\033[?25h");
    EXPECT_TRUE(term.t.cursor_visible);
}

TEST(TerminalCsi, WraparoundModeToggle) {
    Term term;
    term.write("\033[?7l");
    EXPECT_FALSE(term.t.wraparound);
    term.write("\033[?7h");
    EXPECT_TRUE(term.t.wraparound);
}

TEST(TerminalCsi, ApplicationCursorKeysToggle) {
    Term term;
    term.write("\033[?1h");
    EXPECT_TRUE(term.t.application_cursor_keys);
    term.write("\033[?1l");
    EXPECT_FALSE(term.t.application_cursor_keys);
}

TEST(TerminalCsi, InsertModeToggle) {
    Term term(8, 1);
    term.write("\033[4h");  // insert mode on
    EXPECT_TRUE(term.t.insert_mode);
    term.write("\033[4l");  // off
    EXPECT_FALSE(term.t.insert_mode);
}

TEST(TerminalCsi, SetScrollRegion) {
    Term term(8, 6);
    term.write("\033[2;5r");  // scroll region rows 2..5 (1-based)
    EXPECT_EQ(term.t.scroll_top, 1);
    EXPECT_EQ(term.t.scroll_bottom, 4);
    EXPECT_EQ(term.t.cursor_row, 0);
}

TEST(TerminalCsi, SaveRestoreCursorViaSU) {
    Term term(10, 5);
    term.write("\033[3;4H");  // move
    term.write("\033[s");     // save
    term.write("\033[1;1H");  // home
    term.write("\033[u");     // restore
    EXPECT_EQ(term.t.cursor_row, 2);
    EXPECT_EQ(term.t.cursor_col, 3);
}

TEST(TerminalCsi, UnknownCsiIgnoredGracefully) {
    Term term;
    term.write("\033[99Z");  // unhandled final byte
    term.write("X");
    EXPECT_EQ(term.cell(0, 0)->code, 'X');
}

/* --------------------------- ESC sequences ------------------------- */
TEST(TerminalEsc, IndexAndReverseIndex) {
    Term term(4, 3);
    term.write("\033[2;1H");  // row1
    term.write("\033D");      // IND (line feed)
    EXPECT_EQ(term.t.cursor_row, 2);
    term.write("\033M");      // RI (reverse line feed)
    EXPECT_EQ(term.t.cursor_row, 1);
}

TEST(TerminalEsc, NextLineNEL) {
    Term term(4, 3);
    term.write("ab");
    term.write("\033E");  // NEL: newline + CR
    EXPECT_EQ(term.t.cursor_row, 1);
    EXPECT_EQ(term.t.cursor_col, 0);
}

/* --------------------------- scroll / linefeed --------------------- */
TEST(Terminal, ScrollsWhenCursorPastBottom) {
    Term term(4, 2);
    term.write("aa\r\nbb\r\ncc");  // 3rd line forces scroll
    // After scroll, top line lost; "bb" on row0, "cc" on row1
    EXPECT_EQ(term.row_text(0).substr(0, 2), "bb");
    EXPECT_EQ(term.row_text(1).substr(0, 2), "cc");
}

/* --------------------------- scrollback API ------------------------ */
TEST(TerminalScrollback, StepAndReset) {
    Term term(4, 2);
    // Generate scrollback by scrolling several lines.
    term.write("l1\r\nl2\r\nl3\r\nl4\r\nl5");
    terminal_scrollback_step(&term.t, 1);
    EXPECT_TRUE(terminal_scrollback_active(&term.t));
    terminal_scrollback_reset(&term.t);
    EXPECT_FALSE(terminal_scrollback_active(&term.t));
}
