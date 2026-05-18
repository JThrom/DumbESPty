# DumbESPty - Technical Specification

## Overview

DumbESPty is a Waveshare 7 inch touch LCD powered by an ESP32-S3. This project is a new take on an old dumb terminal console. It combines a color terminal interface with a simple shell with an SSH client so your favorite terminal experience can go with you anywhere.  

- Wi-Fi station connectivity
- BLE HID keyboard input
- VT100/xterm-style color terminal emulation rendered to LVGL canvas
- SSHv2 client transport via libssh2

Primary integration objective in this phase is compatibility with modern
`zsh`/`oh-my-zsh` and `neovim`/LazyVim terminal expectations while preserving
rendering stability.

## Runtime Architecture

```text
console_base.cpp
  -> terminal (parser + renderer)
  -> shell (local CLI + SSH passthrough)
  -> ssh_client (libssh2 session + recv queue)
  -> wifi_mgr (station management)
  -> ble_hid_host (keyboard input)
  -> waveshare_display/ch422g (display + control lines)
```

Main loop responsibilities:
- process BLE queue
- process Wi-Fi queue
- process SSH RX queue
- render terminal
- run LVGL timer handler

## Key Modules

### Terminal (`main/terminal.cpp`, `main/terminal.hpp`)

Responsibilities:
- Maintains terminal cell model (codepoint + fg/bg + attributes + dirty bit)
- Supports normal and alternate screen buffers
- Parses control stream with state machine:
  - `GROUND`, `ESC`, `CSI`, `CSI_PARAM`, `CSI_INTERMEDIATE`, `OSC`, `ESC_CHARSET`
- Renders dirty cells directly into RGB565 canvas buffer

Compatibility implemented for active editor/shell scenarios:
- Cursor movement and positioning CSI set
- Erase/insert/delete line and char operations
- SGR handling including 16-color, 256-color, and true-color variants
- DEC private modes (`?1`, `?7`, `?25`, `?1047`, `?1049`, etc.)
- Scroll region handling (`CSI Ps;Ps r`)
- Device attribute replies (`DA1`, secondary DA)
- DSR replies (`CSI 5n`, `CSI ?5n`, cursor position reports)
- DECRQM query response (`CSI ? Ps $ p` -> `CSI ? Ps;0$y`)
- `CSI ?u` handled distinctly from restore-cursor `CSI u`
- Keypad mode escapes `ESC >` / `ESC =` consumed correctly

Rendering notes:
- Uses Cozette bitmap font with LVGL fallback font path
- Fixed grid target currently tuned for compact editor use (`100x32`, `8x15`)
- Icon fallback remaps currently include `U+F426` and `U+E348` to `U+F15B`

### SSH Client (`main/ssh_client.cpp`, `main/ssh_client.hpp`)

Responsibilities:
- Opens TCP + libssh2 session/channel
- Receives remote stream in dedicated task
- Queues RX messages for main-loop processing
- Provides outbound write path for shell/keyboard input

Stability hardening:
- RX queue creation fallback depths: `128, 96, 64, 48, 32, 24`
- `ssh_write_mutex` serializes concurrent writes
- Fast DSR query filter/reply path for `CSI 5n` and `CSI ?5n`
- Optional RX trace logging for terminal compatibility debugging

### Shell (`main/shell.cpp`, `main/shell.hpp`)

- Local command processing (`help`, `wifi`, `ssh`, etc.)
- SSH passthrough mode toggling
- Password capture callback support
- Escape/arrow/backspace handling integrated with terminal write path

### BLE HID Host (`main/ble_hid_host.cpp`, `main/ble_hid_host.hpp`)

- BLE keyboard scan/connect/report handling
- Key translation into shell input stream
- Enter and keypad Enter both mapped to carriage return (`\r`)

### Wi-Fi Manager (`main/wifi_mgr.cpp`)

- Station mode setup and connection lifecycle
- Event-driven connectivity state updates for SSH availability

## Font and Glyph Assets

Primary symbol asset:
- `main/fonts/lv_font_nerd_symbols_10.c`

During current stabilization, this symbol font was regenerated to include
previously missing LazyVim glyphs:
- `U+F09AA`
- `U+F0AF5`
- `U+F0E2D`

## Build, Flash, and Target

Target board connection:
- serial port: `/dev/ttyACM1`

Typical cycle:

```bash
export IDF_PATH="$HOME/projects/esp-idf"
. "$IDF_PATH/export.sh"
idf.py build
idf.py -p /dev/ttyACM1 flash
```

## Current Bug Worklist

### 1) Neovim DSR warning

Observed warning:
- `Did not detect DSR response from terminal`

Status:
- Requests/replies have been observed in logs.
- Root cause is likely sequence timing/ordering or related capability probing.

Planned next steps:
1. collect startup trace with current stable parser baseline
2. verify response ordering for DSR and related terminal queries
3. apply minimal compatibility patch without destabilizing LazyVim rendering

### 2) LazyVim icon/glyph gaps

Observed in prior runs as `Missing glyph U+...` warnings.

Status:
- Known missing codepoints were added to symbol font generation.
- Re-test required to confirm no additional codepoints are missing.

Planned next steps:
1. capture any new missing codepoint logs
2. extend symbol font ranges for newly observed glyphs
3. keep fallback remaps targeted and minimal

### 3) Parser regression risk around DCS

An XTGETTCAP DCS responder was attempted and reverted because it broke LazyVim
main screen rendering.

Status:
- DCS parser-state extension is currently disabled.
- This rollback is part of the known-good baseline.

## Known-Good Baseline (Current)

- LazyVim main screen loads after DCS rollback.
- Updated Nerd symbol font includes the three known missing glyphs.
- Firmware builds and flashes successfully to `/dev/ttyACM1`.

## Branch Safety

Working branch: `master`
