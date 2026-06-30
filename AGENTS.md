# DumbESPty Agent Guide

This file is the working guide for OpenCode/Codex-style agents operating in
this repository.

## Project Snapshot

- Project: `DumbESPty`
- Primary branch for active work: `master`
- Hardware target: ESP32-P4 (primary), ESP32-S3 (legacy path)
- Typical flash port: `/dev/ttyACM0`
- Current goal: keep terminal rendering stable for LazyVim while executing the
  phased SSH compatibility plan toward Linux-like server/auth coverage.

## Build and Flash

Use this exact flow unless the user says otherwise:

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

## Development Process (Current)

- The user handles serial monitor capture manually after each build/flash cycle.
- Agents should not block on interactive monitor usage in this environment.
- After each firmware iteration, wait for the user's captured runtime logs before
  concluding SSH handshake behavior.

## Active Runtime Architecture

Current compiled sources are defined in `main/CMakeLists.txt`.

Core modules in active use:
- `main/console_base.cpp`
- `main/terminal.cpp`, `main/terminal.hpp`
- `main/ssh_client.cpp`, `main/ssh_client.hpp`
- `main/shell.cpp`, `main/shell.hpp`
- `main/wifi_mgr.cpp`, `main/wifi_mgr.hpp`
- `main/power_mgr.cpp`, `main/power_mgr.hpp`
- `main/ble_hid_host.cpp`, `main/ble_hid_host.hpp`
- `main/usb_hid_host.cpp`, `main/usb_hid_host.hpp`
- `main/coex_manager.cpp`, `main/coex_manager_stub.cpp`, `main/coex_manager.hpp`
- `main/waveshare_display_p4.cpp`, `main/include/waveshare_display.hpp`
- `main/ch422g_init.cpp`, `main/include/ch422g_init.hpp`

Active font assets:
- `main/fonts/cozette_bdf_13.c`
- `main/fonts/cozette_bdf.h`
- `main/fonts/lv_font_term_mono_10.c`
- `main/fonts/lv_font_nerd_symbols_10.c`

## Known-Good Rendering Baseline

- LazyVim main screen currently loads correctly.
- Keep terminal geometry derived from display resolution using cell size `8 x 15`.
  - On current P4 panel (`1024 x 600`), baseline grid is `128 x 40`.
- Keep Cozette primary + LVGL fallback font flow.
- Default terminal foreground is now an xterm/ANSI 256-color index (default
  `255`, white), configurable via the status-menu slider and
  persisted in NVS (`devicecfg`/`termfg`). The default resolves through
  `color_256()` in `main/terminal.cpp`; see `terminal_set/get/load/save_default_fg_index`
  and `terminal_color_256_rgb888` in `main/terminal.hpp`.

## Recently Resolved (2026-06): terminal.shop / Go SSH server support

`ssh terminal.shop` now works end-to-end (handshake, publickey auth, full
TUI rendering). Three distinct bugs were fixed; all were root-caused with a
host-side reproduction harness that compiles the forked libssh2 + ESP-IDF's
mbedTLS 3.6.4 natively on Linux (scratch dir `/tmp/opencode/sshdbg`).

1. `ssh-ed25519` host key support:
   - terminal.shop offers ONLY `ssh-ed25519` host keys (verified via
     `ssh -vv`); the old build had `LIBSSH2_ED25519 0` so KEX negotiation
     had no hostkey overlap (`rc=-5`/`rc=-8`).
   - The libssh2 fork at `../../libssh2_esp` (see `override_path` in
     `main/idf_component.yml`) now implements ed25519 hostkey verification
     in the mbedTLS backend using vendored ref10 code
     (`src/ed25519_ref10_verify.c`, `src/third_party/ed25519_ref10/`).
   - The fork also improves kex error reporting (inner error codes/messages
     propagate instead of generic `-8 Unable to exchange encryption keys`).

2. Go SSH server channel-startup hang (`shell` request never answered):
   - Sending a `window-change` request between `pty-req` and `shell` wedges
     Go-based SSH servers (x/crypto/ssh, wish/charm stacks): the server
     stops servicing channel requests and the session stays silent forever.
   - Fix: terminal dimensions are now passed inside `pty-req` itself via
     `libssh2_channel_request_pty_ex` and ALL separate window-change
     requests during session startup were removed (`main/ssh_client.cpp`).

3. Blank screen with TUI servers (bubbletea/lipgloss apps):
   - These apps probe the terminal at startup (`OSC 10;?`, `OSC 11;?`,
     `CSI 6n`, `CSI c`) and wait for each reply before drawing anything.
   - Terminal now replies to OSC 10/11 color queries and dispatches
     ST-terminated (`ESC \`) OSC strings (`main/terminal.cpp`).

4. Terminal parser data race:
   - `terminal_write` was called concurrently from the SSH connect task
     (post-connect clear screen) and the main loop (SSH RX queue),
     interleaving bytes mid-escape-sequence: OSC queries rendered as text,
     CSI params got clobbered, query replies were lost.
   - Fix: `terminal_write` is now serialized with a mutex created in
     `terminal_init`.

Known limitation discovered during this work:
- `curve25519-sha256` KEX in the fork uses PSA crypto, but
  `MBEDTLS_PSA_CRYPTO_C` is not enabled in ESP-IDF's mbedTLS config, so
  that KEX path is non-functional on device. The conservative method
  profile prefers `ecdh-sha2-nistp256` first, which works. Enable PSA or
  avoid curve25519-first profiles if this ever surfaces.

Build reproducibility warning:
- The libssh2 fork (`../../libssh2_esp`) currently carries UNCOMMITTED
  changes in both the component repo and its `libssh2` submodule
  (mbedtls.c/h, kex.c, session.c, channel.c/h, ed25519 files). The
  firmware cannot be rebuilt identically without that tree. Commit that
  fork or vendor it before relying on clean checkouts.

## Open Bugs (Current Priority)

1. Neovim DSR warning:
   - Message: `Did not detect DSR response from terminal`
   - Existing code replies to DSR in both terminal parser and SSH fast path.
   - Remaining issue appears timing/order/capability related.
   - Host-side check: warning reproduced with both `TERM=xterm` and
     `TERM=xterm-256color` using `nvim --clean`.
   - Investigation is currently paused; see `SPEC.md` for full attempt log.

2. Nerd Font gaps:
   - Symbol font was regenerated to include observed missing codepoints:
     `U+F09AA`, `U+F0AF5`, `U+F0E2D`.
   - If new `Missing glyph U+....` warnings appear, add exact codepoints to
     font generation range and reflash.

3. Intermittent SSH runtime `rc=-4` disconnects (see `SPEC.md`).

## SSH Authentication & Trust (implemented)

Full supported-mode reference is in `SPEC.md` "SSH Authentication & Trust".
Summary of what exists; do not regress these:

- Host keys: `ssh-ed25519` (via libssh2 fork, ref10 verify in mbedTLS), ECDSA
  (nistp256/384/521), RSA. Preference list built dynamically and applied on
  every handshake attempt (`build_hostkey_method_pref`).
- Auth methods: `none` (probed first), `publickey` (key from runtime slot),
  `keyboard-interactive`, `password`, with sensible fallback ordering in the
  `ssh_connect` auth block.
- Key management: `sshkey status|import|load|clear|erase`; keys stored
  AES-encrypted in the secret vault (`secret_vault.cpp`), passphrase-capable.
- Host trust: TOFU SHA256 pinning per `host:port` (NVS `sshtrust`), mismatch
  hard-fail, legacy SHA1 auto-upgrade; managed via
  `sshknown list|remove|trust|clear`.
- Transport: `TCP_NODELAY`, app-level keepalive
  (`libssh2_keepalive_config/send`), decoded transport rc logging
  (`ssh_transport_rc_str`), graded handshake fallback ladder, PTY dimensions
  sent inside `pty-req` (no separate startup `window-change`).
- Test harness: `test/ssh/server/` (Docker, multi-endpoint, all auth modes).

## Power-Path / USB->Battery Reboot (Hardware, Not Firmware)

- Symptom: with a Li-ion pack on the MX1.25 connector AND USB connected,
  unplugging USB reboots the device (reset_reason=POWERON). Battery-alone cold
  start works fine.
- Root-caused (2026-06) to a board-level power-path handover transient: the
  battery-boost pass FET (Q3, AO3401, U18/R74/R80 bias) only turns on after
  USB-derived `VCC_5V` decays, creating a dead-time dip that briefly
  power-cycles the P4 core rail via the `EN_DCDC`/soft-start + SW2/Q8 latch.
- This is NOT fixable in firmware. Every SoC reset source was ruled out:
  brownout (disabled-diagnostic still rebooted), PSDET (eFuse `POWERGLITCH_EN`
  off), VDD_BAT (RTC-only domain), USB/WDT (wrong reset reason).
- Do NOT spend effort trying to fix this in firmware. The fix is a hardware
  bulk cap on `Core_5V` (or a Schottky `Boost_5V`->`VCC_5V`). The CAN jack `H11`
  and RS485 jack `H9` expose `Core_5V` on pin 1 / `GND` on pin 2 for attaching
  a cap. Full details + topology in `SPEC.md` "Power-Path / USB->Battery
  Handover Reboot".
- Keep the boot-time reset-reason log in `main/console_base.cpp`; it is the
  diagnostic that attributes any future power/reset anomaly.

## Input Compatibility Notes

- Wired keyboard input:
  - USB HID host over onboard OTG port is active on ESP32-P4 builds.
  - BLE and wired USB keyboard inputs run in parallel; keep both queue pumps in
    `main/console_base.cpp` main loop.

- ESC can be keyboard-specific:
  - Some compact keyboards emit `Fn+Esc` via short vendor-style reports
    (for example `00 80 00`) instead of HID keycode `0x29`.
  - The BLE host includes an ESC surrogate mapping for this path.
  - Do not remove this mapping unless replacing it with full HID
    report-descriptor parsing.

- HID usage mapping table must remain index-aligned:
  - The `hid_to_ascii` table in `main/ble_hid_host.cpp` is indexed by usage
    code and must keep placeholder slots (notably usage `0x32`, Non-US key).
  - Removing placeholder entries shifts punctuation mappings and breaks `/`,
    `;`, and related keys.

## Coexistence Quirk (Current)

- BLE keyboard may disconnect or pause after Wi-Fi connect / SSH activity.
- This is expected with current coexistence acquire/release behavior.
- In practice, press any key on the keyboard to wake and trigger reconnect.

## Regressions to Avoid

- Do not reintroduce DCS parser-state extensions that break LazyVim screen
  layout without a clearly isolated and validated fix.
- A previous XTGETTCAP DCS state-machine attempt caused rendering regression
  and was reverted.
- Do not reorder parser checks in `ST_GROUND` so C1 control-byte handling runs
  before UTF-8 continuation handling; this can corrupt Nerd icon glyphs
  (`U+F15B`/`EF 85 9B`) into stray CSI text like `[12;22H`.
- Keep private-use icon codepoints routed to the LVGL/Nerd fallback path
  instead of Cozette bitmap-first lookup to avoid PUA glyph mismatches.
- Do not reintroduce a separate `window-change` (pty size) request between
  `pty-req` and `shell` during SSH session startup; this hangs Go-based SSH
  servers (terminal.shop). Dimensions belong inside `pty-req`
  (`libssh2_channel_request_pty_ex`).
- Do not remove the `terminal_write` serialization mutex; concurrent parser
  writes corrupt escape sequences and drop terminal query replies.
- Do not remove the SSH application keepalive (`libssh2_keepalive_config` at
  connect + periodic `libssh2_keepalive_send` in `ssh_process_queue`) or
  `TCP_NODELAY`; both are transport-stability/latency measures.
- Keep the graded handshake fallback ladder in `ssh_client.cpp` (attempt 1
  conservative, attempt 2 broadened modern, attempt 3 libssh2 defaults) and
  apply the hostkey preference list on every attempt so ed25519-only servers
  keep negotiating.
- Do not remove the OSC 10/11 color-query replies or ST-terminated OSC
  dispatch in `terminal.cpp`; bubbletea/lipgloss TUI servers block on them.
  The OSC 10 reply now reflects the configured default fg index; keep it in
  sync with `default_fg()` rather than hardcoding a color again.
- Do not revert the terminal default foreground to the old custom pure green
  (`RGB565(0,255,0)`); it was display-specific. Default fg is an xterm/ANSI
  256-color index (default `255`, white) routed through `color_256()`.
- Keep idle low-power activity hooks intact: `shell_handle_key` and the
  `ssh_process_queue` drain (`drained > 0`) must call `power_mark_activity()`,
  and the main loop must apply the `power_mgr_step()` return as its
  `vTaskDelay`. Removing these makes the screen sleep during active SSH output
  or never wake on input. Backlight changes must go through
  `waveshare_display_set_brightness` (not raw GPIO) so the backlight keepalive
  task does not revert them. Idle timeout is `POWER_IDLE_TIMEOUT_MS` (30s).

## Git and Safety Guidance

- Do not push unless the user explicitly asks.
- Keep commits focused and checkpoint known-good behavior frequently.


## Cleanup Policy for This Repo

- Remove dead code that is not referenced by active build targets.
- Prefer deleting deprecated duplicate modules over keeping parallel versions.
- Keep `.gitignore` updated for generated artifacts and local scratch files.
