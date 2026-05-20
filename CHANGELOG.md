# Changelog

All notable changes to DumbESPty are documented in this file.

## [Unreleased]

### Added
- Touch status menu module with expanded drawer UI, outside-tap dismiss, and updated hit zones.
- BLE keyboard management UI flow:
  - scan for keyboards,
  - pair by selection,
  - disconnect/forget behavior,
  - persistent single-device pairing metadata in NVS.
- Wi-Fi SSID status display integration in status menu.
- BLE reconnect strategy that prefers saved full address and falls back to prefix-assisted scan.

### Changed
- Status menu visual styling updated to dark-themed controls and list rows.
- BLE device labels in UI/status now use `Keyboard #` slot naming.
- Battery status elements are currently hidden in UI while underlying support remains in code.
- Terminal compatibility handling improved for noisy modern sequences (`CSI > ... q`, `CSI > ... u`) and `CSI X` erase-character.
- Collapsed status panel icon layout updated with Wi-Fi/BLE moved to the top and centered on a shared vertical axis now that battery icon display is disabled.
- SSH channel startup now switches to non-blocking mode before PTY/shell startup calls to keep timeout wrappers effective and avoid blocking connect paths.

### Fixed
- ESC reliability restored for compact BLE keyboards that emit `Fn+Esc` via short vendor-style report (`00 80 00`) rather than HID keycode `0x29`.
- HID usage-to-ASCII punctuation mapping corrected by preserving required `0x32` placeholder alignment in `hid_to_ascii`.
- Character-key regressions for `/`, `?`, `;`, and `'` caused by shifted HID usage indexing.
- Missing icon fallback remaps expanded for observed glyph warnings (`U+F0B37`, `U+F12B7`, `U+F1064`).
- Added `CSI d` (VPA) handling to reduce unhandled-sequence warnings during tmux/Neovim usage.
- Fixed UTF-8 parser ordering so continuation bytes (including `0x9B`) are consumed before C1 control handling, preventing Nerd icon bytes like `U+F15B` (`EF 85 9B`) from becoming stray CSI text (`[12;22H`).
- Private-use icon codepoints now skip Cozette bitmap-first lookup so Nerd Font glyphs render consistently.
- Added `U+2328` keyboard-sign fallback remap to Nerd keyboard icon `U+F11C`.

### Known Issues / Quirks
- BLE keyboard may disconnect or pause after Wi-Fi connect and/or SSH activity due to current coexistence acquire/release behavior.
- Practical workaround: press any key on the keyboard to wake and trigger reconnect.
