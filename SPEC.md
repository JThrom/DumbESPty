# DumbESPty - Technical Specification

## Overview

DumbESPty is a Waveshare 7 inch touch LCD terminal platform powered primarily by ESP32-P4 (legacy path on ESP32-S3). This project is a new take on an old dumb terminal console. It combines a color terminal interface with a simple shell and SSH client so your terminal experience can go with you anywhere.  

- Wi-Fi station connectivity
- Tailscale overlay networking (tailnet reachability)
- BLE HID keyboard input
- USB OTG HID keyboard input (wired)
- VT100/xterm-style color terminal emulation rendered to LVGL canvas
- SSHv2 client transport via libssh2

Primary integration objective in this phase is Linux-like SSH client
compatibility across mixed server/auth configurations while preserving
LazyVim rendering stability.

## Current Status

- Active hardware target for current work: ESP32-P4-WIFI6-Touch-LCD-7B.
- Build baseline: ESP-IDF 6.1.
- Terminal geometry derives from display resolution at runtime.
  - P4 baseline: `1024x600` -> `128x40` grid at cell `8x15`.
- Shell SSH parser supports both:
  - `ssh host[:port]`
  - `ssh user@host[:port]`
- SSH auth behavior probes first, then uses `none` / `keyboard-interactive` /
  `password` paths as available.
- `ssh-ed25519` host keys are now supported (libssh2 fork, ref10 verify in
  mbedTLS backend); ed25519-only servers such as `terminal.shop` work
  end-to-end (2026-06).
- Go-based SSH servers (x/crypto/ssh, wish/charm) are now compatible:
  terminal dimensions are sent inside `pty-req` and no separate
  `window-change` request is issued during session startup.
- Wired USB keyboard input over onboard OTG host port is supported.
- BLE and wired USB keyboard inputs can be used in parallel.

## SSH Compatibility Roadmap (Phased)

1. Phase 1: host key compatibility (DONE 2026-06)
   - Client supports `ssh-ed25519` host keys via libssh2 fork.
2. Phase 2: auth method coverage
   - Ensure robust `publickey`, `keyboard-interactive`, `password`, `none`
     negotiation behavior.
3. Phase 3: key management
   - Add vault-backed private-key import/storage and passphrase support.
4. Phase 4: host trust model
   - Add `known_hosts`-style fingerprint pinning and mismatch protections.
5. Phase 5: compatibility polish
   - Improve defaults/fallbacks and diagnostics to match Linux SSH behavior.

## Runtime Architecture

```text
console_base.cpp
  -> terminal (parser + renderer)
  -> shell (local CLI + SSH passthrough)
  -> ssh_client (libssh2 session + recv queue)
  -> wifi_mgr (station management)
  -> tailscale_mgr (overlay networking + tailnet status)
  -> ble_hid_host (keyboard input)
  -> usb_hid_host (wired keyboard input)
  -> waveshare_display/ch422g (display + control lines)
```

Main loop responsibilities:
- process BLE queue
- process USB HID queue
- process Wi-Fi queue
- process Tailscale queue
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
- `CSI X` (Erase Character) implemented
- `CSI d` (VPA: vertical position absolute) implemented
- `CSI > ... q` and `CSI > ... u` variants consumed to reduce compatibility warning noise
- UTF-8 continuation bytes are consumed before C1 checks in ground state, preventing icon bytes (for example `U+F15B` -> `EF 85 9B`) from being misparsed as CSI
- OSC 10/11 color queries (`OSC 10;?` / `OSC 11;?`) are answered with the
  default palette (`rgb:0000/ffff/0000` fg, `rgb:0000/0000/0000` bg);
  bubbletea/lipgloss TUI apps block at startup waiting for these replies
- OSC strings terminated with ST (`ESC \`) are now dispatched (previously
  only BEL/0x9C terminators dispatched, so ST-terminated queries were lost)
- `terminal_write` is serialized with a mutex (created in `terminal_init`);
  the parser is a stateful byte-stream machine and concurrent writers
  (SSH connect task vs main-loop RX drain) corrupted escape sequences

Rendering notes:
- Uses Cozette bitmap font with LVGL fallback font path
- Private-use codepoints (Nerd icon ranges) skip Cozette bitmap lookup so Nerd symbol glyphs render consistently
- Grid is derived from active display resolution using `8x15` cells
- Icon fallback remaps currently include `U+F426`, `U+E348`, `U+F0B37`, `U+F12B7`, and `U+F1064` to `U+F15B`

### SSH Client (`main/ssh_client.cpp`, `main/ssh_client.hpp`)

Responsibilities:
- Opens TCP + libssh2 session/channel
- Receives remote stream in dedicated task when available
- Queues RX messages for main-loop processing
- Provides outbound write path for shell/keyboard input

Stability hardening:
- RX queue creation fallback depths: `128, 96, 64, 48, 32, 24`
- `ssh_write_mutex` serializes concurrent writes
- Fast DSR query filter/reply path for `CSI 5n` and `CSI ?5n`
- Foreground RX pump fallback in main loop when `ssh_recv` task creation fails
- Optional RX/libssh2 trace logging for terminal compatibility debugging (disabled by default in normal operation)
- Default runtime monitor output is tuned for daily use:
  - terminal CSI trace logging is disabled,
  - bracketed-paste state trace logging is disabled,
  - SSH RX preview/trace and periodic IO-diag logs are disabled unless `SSH_VERBOSE_RX_LOGS=1`.
- libssh2/mbedTLS compatibility fixes for ESP-IDF 6.1:
  - corrected cipher direction handling,
  - corrected mbedTLS v3 HMAC setup flow
- strict-KEX handling corrected to avoid unilateral sequence resets after NEWKEYS
- auth flow includes method probe and keyboard-interactive fallback
- auth order now prefers `none`, then `publickey` (when a key is loaded), then password-style fallback
- hostkey preference ordering is now built dynamically from client-supported algorithms (prefers `ssh-ed25519`, then ECDSA, then RSA where available)
- server hostkey fingerprint (SHA1) is logged after handshake as Phase 4 trust-model groundwork
- host trust uses initial TOFU persistence and rejects host key mismatches for previously seen `host:port`
- `ssh-ed25519` host keys supported via libssh2 fork (`../../libssh2_esp`,
  `override_path` in `main/idf_component.yml`): ed25519 verification uses
  vendored ref10 code in the mbedTLS backend
- PTY request sends terminal dimensions inside `pty-req` itself
  (`libssh2_channel_request_pty_ex`); no separate `window-change` request is
  sent during session startup because Go-based SSH servers stop servicing
  channel requests in that ordering (shell request never answered)
- known limitation: `curve25519-sha256` KEX in the fork requires PSA crypto
  (`MBEDTLS_PSA_CRYPTO_C`), which is not enabled in ESP-IDF's mbedTLS config;
  the conservative profile prefers `ecdh-sha2-nistp256`, which works

### Shell (`main/shell.cpp`, `main/shell.hpp`)

- Local command processing (`help`, `wifi`, `ssh`, etc.)
- Local MAC management command for captive-portal / MAC-clone workflows:
  - `mac`
  - `mac set <xx:xx:xx:xx:xx:xx>`
- SSH passthrough mode toggling
- Password capture callback support (deferred until required by auth probe)
- `sshkey` shell command adds vault-backed private-key import/load and runtime key management
- Escape/arrow/backspace handling integrated with terminal write path
- Bash-like TAB completion behavior with alphabetized command/subcommand/SSID
  candidate lists and double-TAB candidate display
- `help` output sorted alphabetically
- Line-editing behavior corrected for cursor-middle insert/delete and
  backspace/delete shift semantics
- Scrollback view controls:
  - `Ctrl+Shift+Up` (line up)
  - `Ctrl+Shift+Down` (line down)
  - any regular key returns to bottom/live input

### BLE HID Host (`main/ble_hid_host.cpp`, `main/ble_hid_host.hpp`)

- BLE keyboard scan/connect/report handling
- Key translation into shell input stream
- Enter and keypad Enter both mapped to carriage return (`\r`)
- Manual keyboard scan API for UI list-based pairing
- Single-device persistent pairing in NVS (`blehid`) with prefix/name/slot and optional full BLE address
- Auto reconnect path:
  - direct reconnect by saved full address when available,
  - fallback scan/connect via saved prefix
- FN-layer ESC compatibility path for keyboards that emit vendor-style short reports (e.g. `00 80 00`)
- HID usage table corrected to preserve index alignment (including required `0x32` placeholder) so punctuation keys map correctly

### USB HID Host (`main/usb_hid_host.cpp`, `main/usb_hid_host.hpp`)

- Initializes USB Host and USB HID host driver on ESP32-P4
- Handles wired keyboard connect/disconnect events from OTG host stack
- Decodes boot keyboard reports and maps keys into shell input stream
- Reuses control/navigation mappings used by BLE keyboard path
- Supports concurrent input with BLE keyboard path

### Status Menu UI (`main/ui_status_menu.cpp`, `main/ui_status_menu.hpp`)

- Touch-open status drawer with outside-tap dismiss
- Open gesture is top-right-corner touch zone only
- Dedicated close button removed (outside-touch dismiss retained)
- Dark-themed Wi-Fi/BLE controls
- Includes Tailscale status line and live state color coding
- Includes backlight brightness slider on ESP32-P4 (`5%` to `100%`, default `25%`)
- BLE flow:
  - `Scan` shown when unpaired,
  - list rows shown as `Keyboard #`,
  - `Disconnect` shown when paired
- Wi-Fi label formatting:
  - connected: `WiFi: <ssid>`
  - disconnected: `WiFi: disconnected`
- Battery status/icon currently hidden in UI (feature code retained)

### Wi-Fi Manager (`main/wifi_mgr.cpp`)

- Station mode setup and connection lifecycle
- Event-driven connectivity state updates for SSH availability
- Shell UX progress output for blocking scan/connect actions
  (`scanning...` / `connecting...` with per-second dot updates)
- Status string reports connected IP when available (`connected as <ip>`)
- STA MAC get/set support for shell-level MAC-clone workflows

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
- serial port: `/dev/ttyACM0`

Typical cycle:

```bash
export IDF_PATH="$HOME/projects/esp-idf"
. "$IDF_PATH/export.sh"
idf.py build
idf.py -p /dev/ttyACM0 flash
```

## Current Bug Worklist

### 0) terminal.shop / Go SSH server compatibility (resolved 2026-06)

Originally observed:
- servers offering only `ssh-ed25519` host key failed at handshake with
  `session_handshake rc=-5` / `rc=-8`, `Unable to exchange encryption keys`.
- after hostkey support was added, sessions connected but the screen stayed
  blank: `shell` request timed out and zero RX bytes ever arrived.
- after the shell fix, the OSC color query rendered as literal `]10;?` text
  and the TUI never started.

Root causes found (validated with a host-side Linux build of the forked
libssh2 + ESP-IDF mbedTLS 3.6.4 connecting to terminal.shop):

1. No `ssh-ed25519` support in libssh2 mbedTLS backend
   (`LIBSSH2_ED25519 0`): no hostkey overlap with ed25519-only servers.
   Fixed in the libssh2 fork with vendored ref10 ed25519 verification.
2. `window-change` request sent between `pty-req` and `shell` wedges
   Go-based SSH servers: the server stops servicing channel requests and
   never answers `shell`. Fixed by passing dimensions inside `pty-req`
   (`libssh2_channel_request_pty_ex`) and removing all startup
   window-change requests.
3. bubbletea/lipgloss TUI apps probe `OSC 10;?`, `OSC 11;?`, `CSI 6n`,
   `CSI c` at startup and wait for each reply before drawing. The terminal
   did not answer OSC color queries and dropped ST-terminated OSC strings.
   Fixed in `terminal.cpp` (OSC 10/11 replies + ST dispatch).
4. Concurrent `terminal_write` calls (SSH connect task post-connect clear
   vs main-loop RX drain) interleaved bytes mid-escape-sequence, printing
   query text on screen and clobbering CSI params so replies were skipped.
   Fixed with a `terminal_write` mutex.

Current status:
- resolved; `ssh terminal.shop` renders the full TUI on device.

### 0.1) SSH handshake/auth regression (resolved in prior cycle)

Previously observed:
- handshake stalled/failure after NEWKEYS while waiting for `SERVICE_ACCEPT`
- errors such as `Failed to get response to ssh-userauth request` and immediate server FIN after first encrypted userauth packet

Resolution summary:
- fixed libssh2 mbedTLS crypto/HMAC backend issues,
- removed unilateral strict-KEX enable paths that reset sequence numbers incorrectly,
- validated successful remote shell bring-up (tmux/nvim launching).

Current status:
- resolved for current baseline.

### 0.2) SSH immediate post-connect `rc=-4` regression (resolved)

Observed in recent iterations:
- session disconnected almost immediately with:
  - `SSH rx read error: rc=-4`
  - `libssh2 err=-4 msg=transport read`
- negotiated set in failing runs commonly included:
  - `c2s/s2c aes128-ctr`
  - `mac_c2s/mac_s2c hmac-sha2-256`

Fix used:
- reverted SSH algorithm preference ordering in `main/ssh_client.cpp` to the
  conservative set that had been stable earlier:
  - ciphers: `aes128-cbc,aes128-ctr,aes256-ctr,aes192-ctr`
  - MACs: `hmac-sha1,hmac-sha2-256,hmac-sha2-512`
- added runtime negotiated-method logging and richer `rc<0` read/write context
  logging to make future regressions attributable.

Current status:
- immediate blank-screen/immediate-disconnect behavior is resolved with the
  above preference rollback.

### 1) Neovim DSR warning

Observed warning:
- `Did not detect DSR response from terminal`

Status:
- Requests/replies have been observed in logs.
- Root cause is likely sequence timing/ordering or related capability probing.

Investigation log (2026-05):
- Baseline behavior:
  - LazyVim main screen renders with current stable parser baseline.
  - Warning still appears at startup: `Did not detect DSR response from terminal`.
- Implemented/validated paths:
  - Parser-side DSR handling in `terminal.cpp` replies to:
    - `CSI 5n` -> `CSI 0n`
    - `CSI ?5n` -> `CSI ?0n`
    - `CSI 6n` / `CSI ?6n` -> CPR (`CSI row;colR` variants)
  - SSH RX fast path in `ssh_client.cpp` currently handles `CSI 5n` and `CSI ?5n`.
- Experiments attempted during this cycle:
  - Added additional SSH-side fast handling/probing for DSR/CPR-related startup queries.
  - Added XTGETTCAP/OSC/DA response experiments to satisfy Neovim startup probing.
  - Added passive RX tracing for sequence ordering.
- Outcomes and regressions observed:
  - DSR warning persisted through those experiments.
  - Aggressive SSH receive-path interception caused instability (`task_wdt` on `ssh_recv` via lwIP mutex path) and/or partial rendering regressions.
  - Experimental scanner/intercept logic was rolled back to preserve known-good rendering stability.
- Additional host-side test result:
  - `nvim --clean` was tested with both `TERM=xterm` and `TERM=xterm-256color`.
  - DSR warning appeared in both cases.
- Current disposition:
  - DSR issue is documented and intentionally paused.
  - Keep stable baseline and avoid further risky SSH RX interception until resumed.

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

### 4) Intermittent SSH runtime `rc=-4` disconnects (open)

Observed:
- sessions can still terminate intermittently during normal command output
  (example: running `git status` in remote shell).
- recent captured failure:
  - `rc=-4` / `transport read`
  - negotiated `aes128-ctr` + `hmac-sha1`

Status:
- unresolved.
- immediate regression is fixed, but long-run transport stability issue remains.

Planned next steps:
1. correlate next `rc=-4` with rekey timing/packet flow using existing runtime
   negotiated/error logs and host-side capture.
2. test narrower method pinning variants one axis at a time (MAC-only and
   cipher-only permutations) to isolate the unstable combination.
3. if needed, instrument libssh2 transport boundary around MAC verify failure
   path for sequence/packet-size correlation.

## Known-Good Baseline (Current)

- LazyVim main screen loads after DCS rollback.
- `ssh terminal.shop` (Go server, ed25519-only hostkey, bubbletea TUI)
  connects and renders end-to-end.
- Password-auth servers (e.g. local OpenSSH hosts) continue to work with
  the same flow.
- Updated Nerd symbol font includes the three known missing glyphs.
- Firmware builds and flashes successfully to `/dev/ttyACM0`.

## Build Reproducibility Note

The libssh2 fork at `../../libssh2_esp` (used via `override_path` in
`main/idf_component.yml`) currently carries uncommitted changes in both the
component repo and its `libssh2` submodule (ed25519 ref10 files, mbedtls
backend, kex/session/channel patches). The firmware cannot be rebuilt
identically from a clean checkout without that tree. Commit or vendor the
fork before relying on fresh clones.

## Known Quirks and Workarounds

- BLE coexistence with Wi-Fi/SSH: during network acquire/release windows, BLE scan may pause and active BLE HID connection may be terminated.
- Practical symptom: keyboard can appear disconnected after Wi-Fi connect or SSH activity.
- Current expected recovery: press any key on keyboard to wake/re-trigger reconnect.
- This is a known tradeoff in the current coexistence model and is documented behavior, not a random regression.

## Branch Safety

Working branch: `master`
