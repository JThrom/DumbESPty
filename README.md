```text
8888888b.                         888      8888888888 .d8888b.  8888888b.  888
888  "Y88b                        888      888       d88P  Y88b 888   Y88b 888
888    888                        888      888       Y88b.      888    888 888
888    888 888  888 88888b.d88b.  88888b.  8888888    "Y888b.   888   d88P 888888 888  888
888    888 888  888 888 "888 "88b 888 "88b 888           "Y88b. 8888888P"  888    888  888
888    888 888  888 888  888  888 888  888 888             "888 888        888    888  888
888  .d88P Y88b 888 888  888  888 888 d88P 888       Y88b  d88P 888        Y88b.  Y88b 888
8888888P"   "Y88888 888  888  888 88888P"  8888888888 "Y8888P"  888         "Y888  "Y88888
                                                                                       888
                                                                                  Y8b d88P
                                                                                   "Y88P"
```

# DumbESPty

## Overview

DumbESPty is a portable, color dumb-terminal style system built on a Waveshare 7-inch ESP32-S3 touch display board. It combines:

- Wi-Fi station connectivity
- BLE HID keyboard input
- VT100/xterm-style color terminal emulation rendered via LVGL
- SSHv2 transport via libssh2

Primary integration goal in the current phase: keep LazyVim rendering stable while improving compatibility with modern `zsh`/`oh-my-zsh` and `neovim` terminal behavior (especially DSR-related behavior).

## Project Snapshot

- Project: `DumbESPty`
- Branch: `master`
- Hardware target: ESP32-S3
- Typical flash port: `/dev/ttyACM1`
- Semantic firmware version: `1.0.0`

## Runtime Architecture

```text
deck_base.cpp
  -> terminal (parser + renderer)
  -> shell (local CLI + SSH passthrough)
  -> ssh_client (libssh2 session + recv queue)
  -> wifi_mgr (station management)
  -> ble_hid_host (keyboard input)
  -> waveshare_display/ch422g (display + control lines)
```

Main loop responsibilities:

- Process BLE queue
- Process Wi-Fi queue
- Process SSH RX queue
- Render terminal
- Run LVGL timer handler

## Active Modules

Compiled source set is defined in `main/CMakeLists.txt`.

Core modules in active use:

- `main/deck_base.cpp`
- `main/terminal.cpp`, `main/terminal.hpp`
- `main/ssh_client.cpp`, `main/ssh_client.hpp`
- `main/shell.cpp`, `main/shell.hpp`
- `main/wifi_mgr.cpp`, `main/wifi_mgr.hpp`
- `main/ble_hid_host.cpp`, `main/ble_hid_host.hpp`
- `main/coex_manager.cpp`, `main/coex_manager.hpp`
- `main/waveshare_display.cpp`, `main/include/waveshare_display.hpp`
- `main/ch422g_init.cpp`, `main/include/ch422g_init.hpp`

### Terminal (`main/terminal.cpp`, `main/terminal.hpp`)

Responsibilities:

- Maintains terminal cell model (codepoint + fg/bg + attributes + dirty bit)
- Supports normal and alternate screen buffers
- Parses control stream via state machine (`GROUND`, `ESC`, `CSI`, `CSI_PARAM`, `CSI_INTERMEDIATE`, `OSC`, `ESC_CHARSET`)
- Renders dirty cells into RGB565 canvas buffer

Implemented compatibility areas:

- Cursor movement/positioning CSI set
- Erase/insert/delete line and char operations
- SGR for 16-color, 256-color, and truecolor
- DEC private modes (`?1`, `?7`, `?25`, `?1047`, `?1049`, etc.)
- Scroll region (`CSI Ps;Ps r`)
- Device attributes (`DA1`, secondary DA)
- DSR replies (`CSI 5n`, `CSI ?5n`, cursor position reports)
- DECRQM response (`CSI ? Ps $ p` -> `CSI ? Ps;0$y`)
- Distinct `CSI ?u` handling vs restore-cursor `CSI u`
- Keypad mode escapes `ESC >` / `ESC =`

Rendering notes:

- Cozette bitmap primary + LVGL fallback flow
- Grid baseline: `100 x 32`, cell size `8 x 15`
- Icon fallback remaps include `U+F426` and `U+E348` to `U+F15B`

### SSH Client (`main/ssh_client.cpp`, `main/ssh_client.hpp`)

- Opens TCP + libssh2 session/channel
- Dedicated RX task with queued messages to main loop
- Outbound write path for shell/keyboard
- RX queue fallback depths: `128, 96, 64, 48, 32, 24`
- `ssh_write_mutex` for serialized writes
- Fast DSR filter/reply path for `CSI 5n` and `CSI ?5n`
- Optional RX trace for compatibility debugging

### Shell (`main/shell.cpp`, `main/shell.hpp`)

- Local command handling (`help`, `wifi`, `ssh`, `clear`, `about`)
- SSH passthrough mode toggling
- Password capture callback
- Escape/arrow/backspace behavior integrated with terminal output

### BLE HID Host (`main/ble_hid_host.cpp`, `main/ble_hid_host.hpp`)

- BLE keyboard scan/connect/report processing
- Key translation into shell input stream
- Enter and keypad Enter mapped to carriage return (`\r`)

### Wi-Fi Manager (`main/wifi_mgr.cpp`)

- Station mode setup and connection lifecycle
- Event-driven connectivity state for SSH availability

## Fonts and Glyph Assets

Active font assets:

- `main/fonts/cozette_bdf_13.c`
- `main/fonts/cozette_bdf.h`
- `main/fonts/lv_font_term_mono_10.c`
- `main/fonts/lv_font_nerd_symbols_10.c`

Known symbol additions for LazyVim compatibility:

- `U+F09AA`
- `U+F0AF5`
- `U+F0E2D`

## About Command Reference

The `about` command prints a full-color info screen including:

- Product: `Waveshare ESP32-S3-Touch-LCD-7`
- DumbESPty version: runtime app metadata (currently `1.0.0`)
- Author: `Jason Throm`
- GitHub: `https://github.com/JThrom/DumbESPty`
- License: `GNU General Public License v2.0`
- Display details: `7-inch RGB panel`, terminal grid `100 x 32`, cell `8 x 15`
- Controller helper: `CH422G I2C expander init path enabled`
- MCU: ESP32-S3 (dual-core Xtensa LX7, Wi-Fi + BLE)
- ESP-IDF version (runtime)
- FreeRTOS version (runtime)
- LVGL version (runtime macro values)
- SSH transport: libssh2 version + non-blocking channel mode
- Input paths: BLE HID host + USB serial console

## Dependency Versions

Locked dependency versions from `dependencies.lock`:

- `idf`: `6.1.0`
- `lvgl/lvgl`: `9.5.0`
- `skuodi/libssh2_esp`: `1.1.0`
- `espressif/usb`: `1.4.0`
- `espressif/esp_lcd_touch`: `1.2.1`
- `espressif/esp_lcd_touch_gt911`: `1.2.0~2`

Direct dependency set:

- `espressif/esp_lcd_touch_gt911`
- `espressif/usb`
- `idf`
- `lvgl/lvgl`
- `skuodi/libssh2_esp`

Project/component declarations:

- Project CMake version: `cmake_minimum_required(VERSION 3.16)`
- Project semantic version: `PROJECT_VER = 1.0.0`
- Main component manifest constraints (`main/idf_component.yml`):
  - `idf >= 5.3.0`
  - `espressif/esp_lcd_touch_gt911 ^1.2.0~2`
  - `lvgl/lvgl ^9.4.0`
  - `espressif/usb *`
  - `skuodi/libssh2_esp ^1.1.0`

## Build, Flash, and Monitor

Use this flow:

```bash
export IDF_PATH="$HOME/projects/esp-idf"
. "$IDF_PATH/export.sh"
idf.py build
idf.py -p /dev/ttyACM1 flash
```

Optional monitor:

```bash
idf.py -p /dev/ttyACM1 monitor
```

## Known-Good Baseline

- LazyVim main screen currently loads correctly.
- Keep terminal geometry at `100 x 32`, cell size `8 x 15`.
- Keep Cozette primary + LVGL fallback flow.
- Firmware builds and flashes successfully to `/dev/ttyACM1`.

## Current Priority Bugs

1. Neovim DSR warning
   - Message: `Did not detect DSR response from terminal`
   - DSR replies exist in parser and SSH fast path; issue appears timing/order/capability related.

2. Nerd Font gaps
   - Missing codepoints may still appear in future runs.
   - Add exact new `U+....` values to symbol font range as observed.

3. DCS parser regression risk
   - Prior XTGETTCAP DCS state-machine experiment regressed LazyVim rendering and was reverted.

## Regressions to Avoid

- Do not reintroduce DCS parser-state extensions unless isolated and validated.
- Preserve current known-good LazyVim rendering behavior.

## Branch and Maintenance Policy

- Do not push unless explicitly requested.
- Keep commits focused and checkpoint known-good behavior.
- Remove dead code not used by active build targets.
- Prefer deleting deprecated duplicate modules.
- Keep `.gitignore` current for generated/local scratch artifacts.
