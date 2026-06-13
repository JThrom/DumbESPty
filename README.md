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

Name meaning:

- `Dumb`: a classic dumb terminal interaction model.
- `ESP`: powered by an Espressif ESP32 MCU family target (primary: ESP32-P4).
- `PTY`: pseudo-terminal behavior over SSH (remote shell/editor terminal endpoint).

## Overview

DumbESPty is a portable, color dumb-terminal style system built for Waveshare 7-inch ESP32 touch display boards, with primary active work on ESP32-P4-WIFI6-Touch-LCD-7B. It combines:

- Wi-Fi station connectivity
- BLE HID keyboard input
- VT100/xterm-style color terminal emulation rendered via LVGL
- SSHv2 transport via libssh2

Primary integration goal in the current phase: keep LazyVim rendering stable while driving SSH compatibility toward Linux-client behavior across mixed server/auth configurations.

## Current Status (2026-05)

- ESP32-P4 build/flash baseline is stable on ESP-IDF 6.1.
- Display geometry is now derived from panel resolution at runtime:
  - P4: `1024x600` panel -> terminal grid `128x40` at `8x15`.
- Shell SSH command accepts both OpenSSH-like forms:
  - `ssh host[:port]`
  - `ssh user@host[:port]`
- SSH auth flow now probes server methods before prompting:
  - supports `none` probe,
  - supports `keyboard-interactive` fallback,
  - prompts for password only when password-style auth is actually required.
- SSH now attempts auth in Linux-like order when methods are offered:
  - `none` -> `publickey` (when a key is loaded) -> `keyboard-interactive`/`password`.
- SSH connect diagnostics now include:
  - dynamic hostkey preference ordering across available `ssh-ed25519`/ECDSA/RSA algorithms,
  - server hostkey fingerprint logging (SHA1) as groundwork for future `known_hosts` pinning.
- Host trust now has an initial TOFU check:
  - first-seen host fingerprint is stored,
  - mismatched future fingerprint for same `host:port` is refused.
- `ssh-ed25519` host keys are now supported (2026-06):
  - implemented in the libssh2 fork's mbedTLS backend via vendored ref10
    ed25519 verification,
  - ed25519-only servers (e.g. `terminal.shop`) handshake successfully.
- Go-based SSH servers (x/crypto/ssh, wish/charm, e.g. `terminal.shop`) are
  now fully compatible:
  - terminal dimensions are sent inside `pty-req`
    (`libssh2_channel_request_pty_ex`); no separate `window-change` request
    is sent during session startup (it wedges Go servers' request loop),
  - terminal answers bubbletea/lipgloss startup probes (`OSC 10;?`,
    `OSC 11;?`, `CSI 6n`, `CSI c`) so TUI apps draw instead of waiting
    forever,
  - `terminal_write` is mutex-serialized so concurrent writers cannot
    corrupt escape-sequence parsing.

## SSH Compatibility Roadmap (Phased)

Phase 1 - Host Key Compatibility (DONE 2026-06)
- `ssh-ed25519` host key support enabled in the device SSH stack
  (libssh2 fork, ref10 verify in mbedTLS backend).
- ECDSA/RSA host key compatibility retained.

Phase 2 - Auth Method Matrix
- Ensure robust method negotiation across `publickey`, `keyboard-interactive`, `password`, and `none`.
- Keep prompt behavior minimal and method-aware.

Phase 3 - Key Management
- Add encrypted private-key storage in vault.
- Add key import/select UX and optional passphrase support.

Phase 4 - Host Trust Model
- Add `known_hosts` style fingerprint pinning (TOFU + persisted trust).
- Add mismatch warnings and safe refusal path.

Phase 5 - Linux-like UX Polish
- Improve per-host defaults, algorithm fallback behavior, and failure diagnostics.
- Preserve terminal rendering stability while increasing server compatibility breadth.

## Recent Feature Additions

- Touch status menu with quick access to Wi-Fi and BLE state.
- Expanded BLE keyboard management UI:
  - scan for keyboards,
  - pair by list selection,
  - persist single paired keyboard in NVS,
  - disconnect/forget flow from UI.
- BLE status labels now use keyboard slot naming (`Keyboard #`) instead of raw device names.
- Dark-themed status menu controls and improved touch-open hit zones.
- Terminal parser compatibility updates:
  - consume `CSI > ... q`/`CSI > ... u` variants without noisy warnings,
  - implement `CSI X` erase-character,
  - implement `CSI d` (VPA),
  - guard UTF-8 continuation handling so bytes like `0x9B` inside icon glyphs are not misparsed as C1 CSI,
  - retain DSR replies in parser and SSH fast path.
- Glyph fallback remaps expanded for observed Nerd Font gaps.
- Private-use icon codepoints (Nerd Font ranges) now bypass Cozette bitmap lookup so Nerd symbols are preferred.

## Go SSH Server Compatibility Fixes (2026-06)

`ssh terminal.shop` (Go server, ed25519-only host key, bubbletea TUI) now
works end-to-end. Three bugs were fixed, each verified with a host-side
Linux build of the forked libssh2 against the live server:

1. `ssh-ed25519` host key support (Phase 1 complete):
   - vendored ref10 ed25519 verification added to the libssh2 fork's
     mbedTLS backend (`../../libssh2_esp`, used via `override_path`),
   - fork also propagates specific KEX failure reasons instead of the
     generic `-8 Unable to exchange encryption keys`.
2. Channel-startup ordering:
   - a `window-change` request between `pty-req` and `shell` causes Go
     SSH servers to stop answering channel requests entirely,
   - dimensions now ride inside `pty-req` via
     `libssh2_channel_request_pty_ex`; startup window-change requests
     removed.
3. Terminal startup probes and parser thread safety:
   - OSC 10/11 color queries are answered and ST-terminated OSC strings
     dispatch correctly, unblocking bubbletea/lipgloss apps,
   - `terminal_write` is mutex-serialized; previously the SSH connect
     task and main loop interleaved bytes mid-escape-sequence, corrupting
     queries and dropping replies.

Note: the libssh2 fork carries the ed25519/kex patches as local changes;
commit or vendor that tree for reproducible builds.

## Recent SSH/Tailnet Stabilization (2026-05)

- SSH handshake/auth path was repaired for ESP-IDF 6.1 + mbedTLS v3 by fixing libssh2 mbedTLS backend issues:
  - corrected cipher direction selection,
  - corrected HMAC setup flow for mbedTLS v3.
- Strict-KEX behavior was corrected by removing unilateral strict-mode activation from server-only KEX tokens, preventing sequence reset mismatches after NEWKEYS.
- SSH startup/rendering reliability improved:
  - if background `ssh_recv` task creation fails, RX now falls back to foreground pump in main loop,
  - remote shell output still renders on display under constrained memory/task conditions.
- SSH debug trace verbosity is now reduced by default (`SSH_VERBOSE_LOGS=0`).
- DERP/WireGuard periodic log chatter was reduced for cleaner monitor output:
  - DERP reconnect/connect failures are warnings (with errno text),
  - periodic WG/disco timing logs only print on slow runs.
- A recent immediate SSH disconnect regression (`rc=-4`, `transport read`) was
  mitigated by restoring conservative method preference ordering:
  - ciphers: `aes128-cbc,aes128-ctr,aes256-ctr,aes192-ctr`
  - MACs: `hmac-sha1,hmac-sha2-256,hmac-sha2-512`

Current note:
- intermittent runtime `rc=-4` disconnects can still occur under some command
  workloads; this remains an active investigation item.

## Status Menu and BLE Usage

1. Open the status menu from the top status strip touch area.
2. If no keyboard is paired:
   - Tap `Scan`.
   - Tap a `Keyboard #` result to pair/connect.
3. If a keyboard is paired:
   - Tap `Disconnect` to disconnect current keyboard.
   - Use forget flow (when exposed in UI path) to clear pairing and return to scan mode.

Behavior notes:

- Only one BLE keyboard is persisted at a time.
- Pairing data is stored in NVS (`blehid` namespace), including address prefix and slot metadata.
- Reconnect prefers full saved address when available, otherwise falls back to prefix-assisted scan/connect.
- FN-layer ESC compatibility is implemented for keyboards that emit vendor-style short reports instead of HID keycode `0x29`.

## Project Snapshot

- Project: `DumbESPty`
- Branch: `master`
- Hardware target: ESP32-P4 (primary active target)
- Typical flash port: `/dev/ttyACM0`
- Semantic firmware version: `1.0.0`

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

- Process BLE queue
- Process Wi-Fi queue
- Process SSH RX queue
- Render terminal
- Run LVGL timer handler

## Active Modules

Compiled source set is defined in `main/CMakeLists.txt`.

Core modules in active use:

- `main/console_base.cpp`
- `main/terminal.cpp`, `main/terminal.hpp`
- `main/ssh_client.cpp`, `main/ssh_client.hpp`
- `main/shell.cpp`, `main/shell.hpp`
- `main/wifi_mgr.cpp`, `main/wifi_mgr.hpp`
- `main/ble_hid_host.cpp`, `main/ble_hid_host.hpp`
- `main/secret_vault.cpp`, `main/secret_vault.hpp`
- `main/coex_manager.cpp`, `main/coex_manager_stub.cpp`, `main/coex_manager.hpp`
- `main/waveshare_display_p4.cpp`, `main/include/waveshare_display.hpp`
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
- OSC 10/11 color query replies (default palette) for bubbletea/lipgloss
  TUI startup probing
- ST-terminated (`ESC \`) OSC dispatch
- Thread-safe `terminal_write` (mutex-serialized parser input)

Rendering notes:

- Cozette bitmap primary + LVGL fallback flow
- Grid is derived from display resolution using cell `8 x 15`
- Icon fallback remaps include `U+F426`, `U+E348`, `U+F0B37`, `U+F12B7`, and `U+F1064` to `U+F15B`

### SSH Client (`main/ssh_client.cpp`, `main/ssh_client.hpp`)

- Opens TCP + libssh2 session/channel
- Dedicated RX task with queued messages to main loop
- Outbound write path for shell/keyboard
- RX queue fallback depths: `128, 96, 64, 48, 32, 24`
- `ssh_write_mutex` for serialized writes
- Fast DSR filter/reply path for `CSI 5n` and `CSI ?5n`
- Optional RX trace for compatibility debugging
- Auth behavior:
  - server auth-method probe before prompting,
  - `keyboard-interactive` auth support,
  - deferred password prompt when password-style auth is required.
- Host key support: `ssh-ed25519` (via libssh2 fork), ECDSA, RSA.
- PTY startup: dimensions sent inside `pty-req`
  (`libssh2_channel_request_pty_ex`); no separate `window-change` during
  session startup (Go SSH server compatibility).
- Known limitation: `curve25519-sha256` KEX requires PSA crypto
  (`MBEDTLS_PSA_CRYPTO_C`), not enabled in ESP-IDF mbedTLS config; the
  conservative profile negotiates `ecdh-sha2-nistp256` instead.

### Shell (`main/shell.cpp`, `main/shell.hpp`)

- Local command handling (`help`, `wifi`, `ssh`, `clear`, `about`)
- SSH passthrough mode toggling
- SSH target parsing supports both `user@host` and host-only forms
- Password capture callback (only when method probe indicates it is needed)
- `sshkey` command supports vault-backed key import/load/clear for SSH publickey auth
- Escape/arrow/backspace behavior integrated with terminal output

### BLE HID Host (`main/ble_hid_host.cpp`, `main/ble_hid_host.hpp`)

- BLE keyboard scan/connect/report processing
- Key translation into shell input stream
- Enter and keypad Enter mapped to carriage return (`\r`)
- HID usage table alignment fixed for punctuation reliability (`/`, `;`, `'`, etc.)
- Includes explicit guard comments for:
  - FN-layer ESC surrogate mapping,
  - HID usage index alignment (`0x32` placeholder)

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

- Product and MCU strings selected by build target (ESP32-P4 vs ESP32-S3)
- DumbESPty version: runtime app metadata (currently `1.0.0`)
- Author: `Jason Throm`
- GitHub: `https://github.com/JThrom/DumbESPty`
- License: `GNU/GPL v2`
- Display details: `7-inch panel`, live terminal grid and cell size values
- Controller helper: `CH422G I2C expander init path enabled`
- MCU: target-dependent runtime string (ESP32-P4 or ESP32-S3)
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
- `espressif/esp_lcd_touch`: `1.2.1`
- `espressif/esp_lcd_touch_gt911`: `1.2.0~2`

Direct dependency set:

- `espressif/esp_lcd_touch_gt911`
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
  - `skuodi/libssh2_esp ^1.1.0`

## Build, Flash, and Monitor

Use this flow:

```bash
export IDF_PATH="$HOME/projects/esp-idf"
. "$IDF_PATH/export.sh"
idf.py build
idf.py -p /dev/ttyACM0 flash
```

Optional monitor:

```bash
idf.py -p /dev/ttyACM0 monitor
```

ESP32-P4 bring-up note (important):

- Waveshare ESP32-P4-WIFI6-Touch-LCD-7B reports `32MB` flash.
- If image header is left at `8MB`, boot logs show:
  - `Detected size(32768k) larger than the size in the binary image header(8192k)`
- This project now pins P4 defaults to `32MB` in `sdkconfig.defaults.esp32p4`:
  - `CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y`
  - `CONFIG_ESPTOOLPY_FLASHSIZE="32MB"`
- Keep these settings. This mismatch consumed multiple failed bring-up attempts previously.

## Known-Good Baseline

- LazyVim main screen currently loads correctly.
- Terminal geometry is computed from display resolution using cell `8 x 15`.
- Keep Cozette primary + LVGL fallback flow.
- Firmware builds and flashes successfully to `/dev/ttyACM0`.

## Known Quirks and Operational Notes

- BLE keyboard may disconnect or appear paused during Wi-Fi connect and active SSH usage because coexistence management can intentionally pause scan/disconnect BLE while network activity is acquired.
- After Wi-Fi/SSH transitions, some keyboards require a keypress to wake and trigger reconnect.
- This behavior is currently expected in the active coexistence model; if keyboard input appears dead after SSH/Wi-Fi events, press any key to prompt reconnect.

## Current Priority Bugs

1. Neovim DSR warning
   - Message: `Did not detect DSR response from terminal`
   - DSR replies exist in parser and SSH fast path; issue appears timing/order/capability related.
   - Reproduced with both `TERM=xterm` and `TERM=xterm-256color` using `nvim --clean`.
   - Detailed attempt history and rollback notes are tracked in `SPEC.md`.

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
