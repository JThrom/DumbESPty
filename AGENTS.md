# DumbESPty Agent Guide

This file is the working guide for OpenCode/Codex-style agents operating in
this repository.

## Project Snapshot

- Project: `DumbESPty`
- Primary branch for active work: `master`
- Hardware target: ESP32-S3
- Typical flash port: `/dev/ttyACM1`
- Current goal: keep terminal rendering stable for LazyVim while improving
  terminal compatibility (especially Neovim/DSR behavior).

## Build and Flash

Use this exact flow unless the user says otherwise:

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

## Active Runtime Architecture

Current compiled sources are defined in `main/CMakeLists.txt`.

Core modules in active use:
- `main/console_base.cpp`
- `main/terminal.cpp`, `main/terminal.hpp`
- `main/ssh_client.cpp`, `main/ssh_client.hpp`
- `main/shell.cpp`, `main/shell.hpp`
- `main/wifi_mgr.cpp`, `main/wifi_mgr.hpp`
- `main/ble_hid_host.cpp`, `main/ble_hid_host.hpp`
- `main/coex_manager.cpp`, `main/coex_manager.hpp`
- `main/waveshare_display.cpp`, `main/include/waveshare_display.hpp`
- `main/ch422g_init.cpp`, `main/include/ch422g_init.hpp`

Active font assets:
- `main/fonts/cozette_bdf_13.c`
- `main/fonts/cozette_bdf.h`
- `main/fonts/lv_font_term_mono_10.c`
- `main/fonts/lv_font_nerd_symbols_10.c`

## Known-Good Rendering Baseline

- LazyVim main screen currently loads correctly.
- Keep terminal grid and geometry aligned with current baseline:
  - 100 columns x 32 rows
  - cell size 8 x 15
- Keep Cozette primary + LVGL fallback font flow.

## Open Bugs (Current Priority)

1. Neovim DSR warning:
   - Message: `Did not detect DSR response from terminal`
   - Existing code replies to DSR in both terminal parser and SSH fast path.
   - Remaining issue appears timing/order/capability related.

2. Nerd Font gaps:
   - Symbol font was regenerated to include observed missing codepoints:
     `U+F09AA`, `U+F0AF5`, `U+F0E2D`.
   - If new `Missing glyph U+....` warnings appear, add exact codepoints to
     font generation range and reflash.

## Regressions to Avoid

- Do not reintroduce DCS parser-state extensions that break LazyVim screen
  layout without a clearly isolated and validated fix.
- A previous XTGETTCAP DCS state-machine attempt caused rendering regression
  and was reverted.

## Git and Safety Guidance

- Do not push unless the user explicitly asks.
- Keep commits focused and checkpoint known-good behavior frequently.


## Cleanup Policy for This Repo

- Remove dead code that is not referenced by active build targets.
- Prefer deleting deprecated duplicate modules over keeping parallel versions.
- Keep `.gitignore` updated for generated artifacts and local scratch files.
