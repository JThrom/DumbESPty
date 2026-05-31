# Changelog

All notable changes to DumbESPty are documented in this file.

## [Unreleased]

### Added (SSH)
- SSH auth method probing now runs before prompting for credentials.
- Keyboard-interactive authentication fallback support was added.
- SSH diagnostics now log supported client hostkey algorithms and explicit
  warning when `ssh-ed25519` host keys are unavailable in current build.

### Changed (Shell/UI)
- `ssh` command now accepts both `ssh [user@]host[:port]` forms.
- `about` command display line now reports live terminal geometry and cell size.
- Terminal grid sizing now derives from active display resolution instead of
  fixed `100x32`.

### Documentation
- Updated `README.md`, `SPEC.md`, and `AGENTS.md` for current ESP32-P4 status,
  active serial port, and phased SSH compatibility roadmap.

### Added (UI/Terminal)
- Local credential/system managers and shell integration modules:
  - `hostname_mgr`
  - `secret_vault`
  - `tailscale_mgr`
- SSH foreground RX pump fallback when background `ssh_recv` task cannot be created, so remote shell rendering still works on constrained heap/task conditions.
- Focused SSH/transport diagnostics used for handshake bring-up and packet flow verification.

### Changed (UI/Terminal)
- SSH/libssh2 handshake compatibility for ESP-IDF 6.1 + mbedTLS v3:
  - fixed cipher encrypt/decrypt direction in libssh2 mbedTLS backend,
  - fixed HMAC init path for mbedTLS v3 (`mbedtls_md_hmac_setup` flow),
  - retained stable cipher-context reset behavior.
- Removed unilateral strict-KEX activation paths that were resetting packet sequence numbers and causing server disconnect after first encrypted userauth packet.
- SSH logging defaults reduced:
  - libssh2 trace spam disabled by default (`SSH_VERBOSE_LOGS=0`),
  - high-frequency RX queue/read debug output removed from normal logs.
- DERP and WireGuard manager log verbosity tuned for normal operation:
  - DERP TCP reconnect failures reported as warnings with errno text,
  - periodic WG/disco timing logs now only emit when slow.
- SSH method preference ordering adjusted back to conservative prior order after
  regression from CTR/SHA2-first ordering:
  - ciphers: `aes128-cbc,aes128-ctr,aes256-ctr,aes192-ctr`
  - MACs: `hmac-sha1,hmac-sha2-256,hmac-sha2-512`

### Fixed (UI/Terminal)
- SSH handshake no longer fails at `Failed to get response to ssh-userauth request` after NEWKEYS due to strict-KEX sequence handling mismatch.
- Restored interactive remote shell rendering on-device even when `ssh_recv` task allocation fails.
- Eliminated immediate post-connect server FIN pattern caused by malformed/sequence-misaligned first encrypted userauth packet path.
- Fixed immediate SSH blank-screen/disconnect regression (`rc=-4`, `transport read`) introduced by recent algorithm-preference iterations.
- Added runtime SSH failure context logging (socket errno, libssh2 error text,
  negotiated methods) to speed root-cause analysis of remaining intermittent
  transport disconnects.

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
