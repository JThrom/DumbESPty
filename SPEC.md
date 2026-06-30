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

## SSH Authentication & Trust

The SSH client implements Linux-like authentication, key management, and host
trust. All logic lives in `main/ssh_client.cpp` (transport/auth/trust) and
`main/shell.cpp` (`ssh`, `sshkey`, `sshknown` commands), with encrypted key
storage in `main/secret_vault.cpp`.

### Connecting

```
ssh [user@]host[:port]
```

- Default user is `root` if `user@` is omitted; default port is `22`.
- The user/host/port parser lives in `cmd_ssh_handler` (`main/shell.cpp`).

### Host key algorithms

- Supported server host key types: `ssh-ed25519`, ECDSA
  (nistp256/384/521), and RSA.
- `ssh-ed25519` is provided by the forked libssh2 (`../../libssh2_esp`,
  `override_path` in `main/idf_component.yml`) using vendored ref10 ed25519
  verification in the mbedTLS backend. This makes ed25519-only servers (e.g.
  `terminal.shop`) work end-to-end.
- The host key preference list is built dynamically from the algorithms the
  client build actually supports, ordered `ssh-ed25519` -> ECDSA -> RSA, and
  is applied on every handshake attempt so ed25519-only servers always have an
  overlap (`build_hostkey_method_pref`).

### Authentication methods

The client probes and negotiates the following methods, in this order
(`ssh_connect` auth block in `main/ssh_client.cpp`):

1. `none` - probed first; if the server grants access with no credentials the
   session proceeds immediately (`ssh_get_auth_methods_with_timeout` reports
   `none` success).
2. `publickey` - used when the server offers it and a private key is loaded.
   The key is taken from the runtime slot (see Key management). On failure the
   client falls back to password/keyboard-interactive if the server offers
   them and a password is available.
   (`ssh_userauth_publickey_with_timeout`,
   `libssh2_userauth_publickey_frommemory`)
3. `keyboard-interactive` - prompt-driven; the configured password answers the
   server's prompts via the `ssh_kbdint_cb` callback.
   (`ssh_userauth_kbdint_with_timeout`)
4. `password` - plain password auth.
   (`ssh_userauth_with_timeout`, `libssh2_userauth_password`)

Behavior details:
- The server's offered method list is queried up front
  (`libssh2_userauth_list`) and drives which path is taken.
- If the server requires `publickey` and no key is loaded, the connect fails
  with a clear message: `server requires publickey auth; load key with
  sshkey load/import`.
- If the server requires a password and none was provided, the connect
  signals `requires password` so the shell can prompt for one
  (`ssh_last_connect_requires_password`).
- Passwords are captured on demand via a hidden-input shell callback and are
  not stored persistently.

### Key management (`sshkey` command)

Private keys are stored encrypted in NVS via the secret vault
(`main/secret_vault.cpp`): AES-encrypted blob, key derived from a
user-supplied vault password, with rekey support (`vault` command).

```
sshkey status   - show runtime key (loaded?) and vault key (present?)
sshkey import   - prompt for vault password, paste PEM private key, optional
                  key passphrase; encrypts and stores the key in the vault
sshkey load     - prompt for vault password; decrypts and loads the key into
                  the runtime slot for use during connect
sshkey clear    - wipe the runtime key from RAM (vault copy retained)
sshkey erase    - wipe the runtime key AND remove it from the vault
```

- Imported PEM keys are validated for a `-----BEGIN` header.
- Passphrase-protected keys are supported (passphrase stored in the vault
  alongside the key, used at `publickey` auth time).
- Runtime key API: `ssh_set_private_key_pem`, `ssh_clear_private_key`,
  `ssh_has_private_key` (`main/ssh_client.hpp`).

### Host trust (`sshknown` command)

TOFU (trust-on-first-use) host key pinning, keyed by `host:port`, stored in
NVS namespace `sshtrust`:

- On first connect to a host the SHA256 (OpenSSH-style `SHA256:base64`)
  fingerprint and key type are pinned (`verify_or_store_host_fingerprint`).
- On reconnect the fingerprint must match; a mismatch hard-fails the connect
  with `host key mismatch` and instructions to accept the new key.
- Legacy SHA1-only records (from earlier firmware) auto-upgrade to SHA256 on
  the next successful connect.
- Both SHA256 and SHA1 fingerprints are logged after each handshake.

```
sshknown list                  - list stored host trust records
                                 (host:port, key type, fingerprint)
sshknown remove <host[:port]>  - remove one pinned record
sshknown trust  <host[:port]>  - reset the pin so the next connect re-pins the
                                 server's current key (accept a changed key)
sshknown clear                 - remove all pinned records
```

Trust API: `ssh_known_hosts_foreach`, `ssh_known_host_remove`,
`ssh_known_hosts_clear` (`main/ssh_client.hpp`).

### Transport robustness

- `TCP_NODELAY` is set on the SSH socket so small interactive writes
  (keystrokes, terminal query replies) are not coalesced/delayed.
- Application-level keepalive: `libssh2_keepalive_config` at connect plus a
  periodic `libssh2_keepalive_send` driven from the main-loop pump
  (`ssh_process_queue`, ~30s) to keep idle links alive and surface dead peers.
- Decoded transport diagnostics: libssh2 return codes are logged with
  human-readable names (`ssh_transport_rc_str`), e.g.
  `rc=-43 (socket-disconnect (transport read/EOF))`.
- Graded handshake fallback ladder: attempt 1 conservative method profile
  (CBC/sha1-first, stable on ESP/mbedTLS), attempt 2 broadened modern profile
  (CTR/sha2-first), attempt 3 libssh2 defaults; the host key preference list
  is applied on every attempt.
- Conservative cipher/MAC set on the first attempt:
  ciphers `aes128-cbc,aes128-ctr,aes256-ctr,aes192-ctr`,
  MACs `hmac-sha1,hmac-sha2-256,hmac-sha2-512`.
- PTY dimensions are sent inside `pty-req`; no separate `window-change`
  request is issued during session startup (Go SSH server compatibility).

### Testing

A Docker-based multi-endpoint SSH test harness for exercising every host key
type and auth method lives in `test/ssh/server/` (see that directory's
README and the "SSH Test Harness" section below).

### Known limitation

`curve25519-sha256` KEX in the libssh2 fork requires PSA crypto
(`MBEDTLS_PSA_CRYPTO_C`), which is not enabled in ESP-IDF's mbedTLS config, so
that KEX path is non-functional on device. The conservative profile prefers
`ecdh-sha2-nistp256` first, which works.

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
  -> power_mgr (idle low-power / backlight management)
```

Main loop responsibilities:
- process BLE queue
- process USB HID queue
- process Wi-Fi queue
- process Tailscale queue
- process SSH RX queue
- render terminal
- run LVGL timer handler
- run idle power step (`power_mgr_step`) and apply its returned loop delay

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
- OSC 10/11 color queries (`OSC 10;?` / `OSC 11;?`) are answered; the OSC 10
  (default foreground) reply reflects the configured default fg color, and
  OSC 11 reports the background (`rgb:0000/0000/0000`). bubbletea/lipgloss
  TUI apps block at startup waiting for these replies
- OSC strings terminated with ST (`ESC \`) are now dispatched (previously
  only BEL/0x9C terminators dispatched, so ST-terminated queries were lost)
- `terminal_write` is serialized with a mutex (created in `terminal_init`);
  the parser is a stateful byte-stream machine and concurrent writers
  (SSH connect task vs main-loop RX drain) corrupted escape sequences
- Configurable default foreground color:
  - stored as an xterm/ANSI 256-color index (default `255`, white),
    resolved via `color_256()`,
  - public API: `terminal_set/get/load/save_default_fg_index()` and
    `terminal_color_256_rgb888()` (`main/terminal.hpp`),
  - persisted in NVS (`devicecfg`/`termfg`); loaded at boot before
    `terminal_init`,
  - changing the index recolors existing cells (screen/alt/scrollback) whose
    fg matches the previous default, then marks all dirty for redraw

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
- server hostkey fingerprint is logged after handshake (SHA256 OpenSSH-style
  plus SHA1)
- host trust uses TOFU persistence keyed by `host:port`, pins the
  SHA256 fingerprint + key type, and rejects host key mismatches; legacy SHA1
  records auto-upgrade to SHA256 on the next connect. Trust records are
  managed with the `sshknown` shell command
  (`ssh_known_hosts_foreach/ssh_known_host_remove/ssh_known_hosts_clear`)
- transport polish: `TCP_NODELAY` enabled on the socket;
  application-level keepalive via `libssh2_keepalive_config/send` (30s) pumped
  from the main loop; libssh2 transport return codes are decoded to readable
  names in logs (`ssh_transport_rc_str`); handshake uses a graded fallback
  ladder (conservative -> broadened modern -> libssh2 defaults)
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
- `sshknown` shell command manages host-trust records
  (`list|remove <host[:port]>|trust <host[:port]>|clear`); `trust` resets a
  pin so the next connect re-TOFUs a changed host key
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
- No `STATUS` title in the expanded drawer
- Dark-themed Wi-Fi/BLE controls
- Expanded drawer order (top to bottom): terminal text-color control,
  brightness control, then Wi-Fi/Tailscale/BLE status lines
- Terminal text-color control:
  - slider selects xterm/ANSI 256-color index (`0`-`255`), default `255`,
  - live color swatch preview,
  - applies + persists via `terminal_set/save_default_fg_index()`,
  - recolors/redraws existing default-colored screen text on change
- Includes backlight brightness slider on ESP32-P4 (`5%` to `100%`, default `25%`)
- Includes Tailscale status line and live state color coding
- All status-drawer text lines capped at 30 characters
- BLE flow:
  - `Scan` shown when unpaired,
  - list rows shown as `Keyboard #`,
  - `Disconnect` shown when paired
  - connected device names shortened (`Bluetooth` -> `BT`, trailing `(id)` stripped)
- Wi-Fi label formatting (status value capitalized):
  - connected: `WiFi: <ssid>`
  - disconnected: `WiFi: Disconnected`
- Tailscale: compact status form in the drawer
  (`tailscale_mgr_get_status_short()`); shell `tailscale status` keeps the
  full detailed line
- Battery status/icon currently hidden in UI (feature code retained)

### Wi-Fi Manager (`main/wifi_mgr.cpp`)

- Station mode setup and connection lifecycle
- Event-driven connectivity state updates for SSH availability
- Shell UX progress output for blocking scan/connect actions
  (`scanning...` / `connecting...` with per-second dot updates)
- Status string reports connected IP when available (`connected as <ip>`)
- STA MAC get/set support for shell-level MAC-clone workflows

### Idle Low-Power Manager (`main/power_mgr.cpp`, `main/power_mgr.hpp`)

Saves battery by dimming the display and slowing the main loop when the device
is unattended. "Activity" is any user HID key (USB or BLE) or any SSH terminal
output reaching the screen.

- **Idle timeout:** `POWER_IDLE_TIMEOUT_MS` (currently `30000`, i.e. 30s with
  no input and no SSH output).
- **Low-power entry:** backlight turned off via
  `waveshare_display_set_brightness(0)` (the current brightness is saved first),
  and the main-loop delay rises from `POWER_LOOP_DELAY_ACTIVE_MS` (10ms / 100Hz)
  to `POWER_LOOP_DELAY_IDLE_MS` (100ms / 10Hz) to cut CPU wakeups.
- **Wake:** the next activity restores the saved brightness and the fast loop
  cadence immediately. Wake is instant; there is no animated ramp.
- **Activity sources / chokepoints:**
  - `shell_handle_key()` (`main/shell.cpp`) calls `power_mark_activity()` for
    every key from either HID transport.
  - `ssh_process_queue()` (`main/ssh_client.cpp`) calls `power_mark_activity()`
    whenever it drains > 0 RX messages to the terminal. SSH keepalives are
    outbound and do not enqueue RX bytes, so they do not falsely keep the screen
    awake.
- **Design / testability:** the decision logic in `power_mgr.cpp` is
  hardware-independent. The clock and backlight are injected via
  `power_mgr_hooks_t` (`power_mgr_init`). `console_base.cpp` installs the real
  hooks (FreeRTOS tick clock + Waveshare backlight). Host unit tests
  (`test/unit/test_power_mgr.cpp`) inject deterministic fakes and cover the full
  state machine including timeout boundary, brightness save/restore, idempotent
  steps, the unsupported-backlight path, and millisecond-clock wraparound.
- **Scope (deliberate):** this is software-level power saving only (backlight +
  loop cadence). It does NOT enable `esp_pm` / `esp_light_sleep`; that path is
  intentionally avoided because of the active USB-host, NimBLE, and MIPI-DSI
  subsystems on the P4 (see the power-path notes below). Backlight changes go
  through `waveshare_display_set_brightness` so they coexist with the backlight
  keepalive task rather than being reverted by raw GPIO writes.

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

## SSH Test Harness

A Docker-based multi-endpoint SSH test harness lives in
`test/ssh/server/`. It launches five OpenSSH endpoints with distinct host key
types and auth policies to exercise every host key type and auth method from
the device:

- A (`:2201`) ed25519 hostkey, password auth
- B (`:2202`) rsa hostkey, publickey-only auth
- C (`:2203`) ecdsa hostkey, keyboard-interactive auth
- D (`:2204`) ed25519 hostkey, all auth methods
- E (`:2205`) rsa hostkey, empty-password (approximate `none`)

`./up.sh` builds/starts the endpoints and prints per-endpoint device commands
plus the client key to import; `./rotate-hostkey.sh <endpoint>` simulates a
changed host key for the host-trust mismatch test. See
`test/ssh/server/README.md`.

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
- transport mitigations in place (application keepalive, TCP_NODELAY, decoded
  transport rc logging) to keep idle links alive and make any recurrence
  attributable; long-run stability to be re-confirmed against the test
  harness.
- immediate regression is fixed.

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

## Power-Path / USB->Battery Handover Reboot (2026-06)

### Symptom

With a Li-ion pack on the MX1.25 battery connector AND USB connected, unplugging
the USB cable causes the device to reboot (LazyVim/boot screen reappears). The
board boots and runs fine on battery alone (cold start), and survives normal
operation; only the live USB->battery transition triggers the reset. The reset
occurs whether unplugging the USB-C (USB 1.1 FS) port or the USB-to-UART port,
and regardless of whether a serial monitor is attached.

Notable observation: with the battery present, the screen backlight fades out
slowly on unplug; with NO battery present, the screen goes black instantly.

### Root cause (confirmed): board power-path handover transient

This is a board-level hardware behavior, not a firmware bug and not an ESP32-P4
configuration problem. Every SoC-side reset source was positively ruled out:

- Brownout detector (TRM Ch23): IDF runs it in Mode 0 with max noise filter
  (`reset_wait=0x3ff`); threshold lowered to the P4 config floor 2.42V
  (`ESP_BROWNOUT_DET_LVL_SEL_5`). A diagnostic build that fully disabled the
  brownout reset (`brownout_ll_reset_config(false,...)` +
  `brownout_ll_ana_reset_enable(false)`) STILL rebooted on unplug -> not
  brownout.
- PSDET / power-glitch detector (TRM Ch24): eFuse `POWERGLITCH_EN = False`
  (verified with `espefuse.py summary`) -> disabled in silicon, cannot fire.
- VDD_BAT / RTC battery (TRM 14.4.1.4, 14.4.2.8): on the P4, VDD_BAT is an
  RTC/AON backup domain only (button cell, 2.5-3.6V), powering the LP/AON
  domain in deep sleep. It cannot run the HP system. IDF's "RTC Backup
  Battery" (`ESP_VBAT_INIT_AUTO`) is disabled and irrelevant to runtime power.
  On this board ESP_VBAT is a separate CR1220 (1220) holder, isolated from the
  main battery path.
- USB-Serial-JTAG reset (codes 0x16/0x17), watchdogs, lockup: ruled out by the
  captured reset reason.

The decisive evidence: a boot-time `esp_reset_reason()` log
(`=== BOOT reset_reason=N (...) ===`, added in `main/console_base.cpp`)
reported `reset_reason=1 (POWERON)` after an unplug event, with the brownout
reset disabled. POWERON (chip power-up, TRM Ch10 code 0x01) means the P4 core
supply was momentarily lost and re-applied.

Board power-path topology (from the Waveshare schematic):

```
USB0_5V (USB-C)  --[Q2 AO3401 P-FET, U4/R25/R26 bias]--\
                                                        >-- VCC'_5V -- VCC_5V
USB1_5V (UART-C) --[Q5 AO3401 P-FET, U23 bias]---------/                 |
                                                                         |
BAT (MX1.25) --[U21 sync boost, EN tied on via R131]--> Boost_5V         |
                          --[Q3 AO3401 P-FET, U18 MMDT3906DW/R74/R80]-----+
                                  (auto-handover, biased OFF while VCC_5V present)

VCC_5V --[load switch, EN_DCDC + 4.7uF soft-start C19; SW2/Q8 soft-latch]--> Core_5V
Core_5V --[buck DCDC]--> ESP_3V3 / ESP_VDD_HP --> ESP32-P4
Core_5V --[boost]--> ~9.6V display/backlight rail (large caps)

Separate: ESP_VBAT <- CR1220 RTC cell only (isolated)
```

Mechanism: the source handover is active-transistor-biased, not an ideal-diode
OR. The battery boost (U21) is always enabled, but its pass FET Q3 is biased
OFF while USB-derived `VCC_5V` is present and only turns ON after `VCC_5V`
decays enough to flip the U18/R74/R80 bias network. During that bias-flip
dead-time `VCC_5V` dips. The dip is enough that the `Core_5V` enable path
(GPIO-driven `EN_DCDC` with the 4.7uF soft-start cap C19, plus the SW2/Q8
latch) momentarily drops the P4 core rail, power-cycling the chip -> POWERON.

This is consistent with the backlight observation: the ~9.6V display rail has
large bulk caps and fades slowly, while the lightly-buffered P4 core rail
collapses for a few ms during the handover dead-time.

### Hardware fix options (no firmware fix is possible)

Because the P4 loses its own core supply during the handover, the remedy is on
the board:

1. Bulk capacitance on `Core_5V` (recommended first try): a large low-ESR cap
   (e.g. 470-1000uF) holds `Core_5V`/`VCC_5V` above the buck's dropout/enable
   through the Q3 bias-flip dead-time.
2. Schottky diode `Boost_5V -> VCC_5V`: lets the always-on battery boost catch
   `VCC_5V` with near-zero dead-time, so the enable path never sees the dip.
   Cleanest targeted mod.
3. Reduce Q3 turn-on delay (smaller R74) - riskier board rework.

### Attaching a bulk cap via a peripheral pigtail jack

The CAN and RS485 PH2.0 jacks both expose `Core_5V` directly:

- CAN jack `H11`: pin 1 = `Core_5V`, pin 2 = `GND` (pin 3 CANH, pin 4 CANL).
- RS485 jack `H9`: pin 1 = `Core_5V`, pin 2 = `GND` (pin 3 A, pin 4 B).

Use H11 pin1/pin2 or H9 pin1/pin2 (cap + to `Core_5V`, - to `GND`). Observe
capacitor polarity and voltage rating (>= 10V for a 5V rail).

Do NOT use these for the cap (wrong rail):
- I2C jack `H10` / UART jack `H8` pin 1 "VCC": defaults to `ESP_3V3` via the
  `H3` 0R jumper (post-buck 3.3V), not `Core_5V`.
- 12-pin headers `P1`/`P3`: expose only `ESP_3V3`, `BAT`, `ESP_LDO_VO4`, `GND`
  - no 5V rail.
- Raw `USB0_5V`/`USB1_5V`/`VBUS_OUT`: before the OR/handover stage.

`Core_5V` is the correct node because it is post-OR, post-load-switch, and the
exact rail that dips during the USB->battery handover.

### Firmware disposition

- Permanent: boot-time reset-reason logging in `main/console_base.cpp`
  (`reset_reason_str()` decodes all P4 reset causes incl. PWR_GLITCH/PSDET,
  BROWNOUT, USB, POWERON).
- Brownout threshold lowered to 2.42V (`ESP_BROWNOUT_DET_LVL_SEL_5`) in
  `sdkconfig.defaults.esp32p4`; harmless and slightly more tolerant, though it
  was not the cause. Brownout reset protection remains ENABLED.
- The TEST-ONLY brownout-disable diagnostic was removed after root-causing.

## Known Quirks and Workarounds

- BLE coexistence with Wi-Fi/SSH: during network acquire/release windows, BLE scan may pause and active BLE HID connection may be terminated.
- Practical symptom: keyboard can appear disconnected after Wi-Fi connect or SSH activity.
- Current expected recovery: press any key on keyboard to wake/re-trigger reconnect.
- This is a known tradeoff in the current coexistence model and is documented behavior, not a random regression.

## Branch Safety

Working branch: `master`
