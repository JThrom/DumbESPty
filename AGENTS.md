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

## Active Runtime Architecture

Current compiled sources are defined in `main/CMakeLists.txt`.

Core modules in active use:
- `main/console_base.cpp`
- `main/terminal.cpp`, `main/terminal.hpp`
- `main/ssh_client.cpp`, `main/ssh_client.hpp`
- `main/shell.cpp`, `main/shell.hpp`
- `main/wifi_mgr.cpp`, `main/wifi_mgr.hpp`
- `main/ble_hid_host.cpp`, `main/ble_hid_host.hpp`
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

## Open Bugs (Current Priority)

1. SSH host key compatibility gap (highest priority):
   - Current libssh2+mbedTLS build does not support `ssh-ed25519` host keys.
   - Servers offering only `ssh-ed25519` fail handshake with
     `Unable to exchange encryption keys`.
   - Short-term workaround is server-side ECDSA/RSA host key enablement.
   - Long-term fix is Phase 1 of SSH roadmap (client-side ed25519 support).

2. Neovim DSR warning:
   - Message: `Did not detect DSR response from terminal`
   - Existing code replies to DSR in both terminal parser and SSH fast path.
   - Remaining issue appears timing/order/capability related.
   - Host-side check: warning reproduced with both `TERM=xterm` and
     `TERM=xterm-256color` using `nvim --clean`.
   - Investigation is currently paused; see `SPEC.md` for full attempt log.

3. Nerd Font gaps:
   - Symbol font was regenerated to include observed missing codepoints:
     `U+F09AA`, `U+F0AF5`, `U+F0E2D`.
   - If new `Missing glyph U+....` warnings appear, add exact codepoints to
     font generation range and reflash.

## SSH Compatibility Roadmap (Phased)

1. Phase 1: host key compatibility
   - Add client support for `ssh-ed25519` host keys.
2. Phase 2: auth method coverage
   - Ensure robust `publickey`, `keyboard-interactive`, `password`, `none`
     negotiation behavior.
3. Phase 3: key management
   - Vault-backed key storage/import and passphrase handling.
4. Phase 4: host trust model
   - `known_hosts`/fingerprint pinning and mismatch protections.
5. Phase 5: compatibility polish
   - Improve default preferences/fallbacks to match Linux-client expectations.

## Input Compatibility Notes

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

## Git and Safety Guidance

- Do not push unless the user explicitly asks.
- Keep commits focused and checkpoint known-good behavior frequently.


## Cleanup Policy for This Repo

- Remove dead code that is not referenced by active build targets.
- Prefer deleting deprecated duplicate modules over keeping parallel versions.
- Keep `.gitignore` updated for generated artifacts and local scratch files.
