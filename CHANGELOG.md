# Changelog

All notable changes to DumbESPty are documented in this file.

## [Unreleased]

### Added (Input/USB HID) - 2026-06
- Added wired keyboard support over onboard USB OTG host port (`main/usb_hid_host.cpp`, `main/usb_hid_host.hpp`).
- Wired USB HID and BLE HID keyboard input now run in parallel and feed the same shell input path.
- Added USB HID host initialization and queue processing in `main/console_base.cpp`.

### Changed (Build/Dependencies) - 2026-06
- Added ESP32-P4 USB host dependencies for wired HID path:
  - `espressif/usb_host_hid`
  - `espressif/usb`
- Pinned `espressif/usb` to `1.1.0` in `main/idf_component.yml` for reproducible ESP-IDF 6.1 compatibility in this project.

### Added (Shell/Wi-Fi) - 2026-06
- New `mac` command for station MAC management:
  - `mac` prints current STA MAC address (`XX:XX:XX:XX:XX:XX`).
  - `mac set <xx:xx:xx:xx:xx:xx>` validates and applies a custom STA MAC for
    captive-portal / MAC-clone workflows.
- Wi-Fi manager now exposes MAC get/set helpers used by shell command paths.

### Changed (Shell) - 2026-06
- TAB completion now follows Bash-like behavior:
  - single TAB completes unique matches,
  - double TAB on unchanged input prints vertical candidate lists,
  - command/subcommand/SSID suggestions are sorted alphabetically.
- `help` command output is now sorted alphabetically.
- Line editing fixes for backspace/delete/insert-at-cursor behavior; middle-line
  edits now shift characters correctly with no stale trailing glyphs.
- Added terminal scrollback view controls from keyboard:
  - `Ctrl+Shift+Up` scrolls up one line,
  - `Ctrl+Shift+Down` scrolls down one line,
  - any regular key returns view to live bottom input.

### Changed (Wi-Fi UX) - 2026-06
- `wifi scan` now prints progress immediately (`scanning` + per-second dots),
  then prints a vertical SSID list on completion.
- `wifi connect <ssid>` now prints progress immediately (`connecting to <ssid>`
  + per-second dots) and reports `connected as <ip>` when complete.
- `wifi status` now reports `connected as <ip>` when IP is available.

### Changed (UI) - 2026-06
- Status drawer close button removed; outside-touch dismiss remains.
- Drawer open gesture is now top-right-corner only (left-edge open removed).
- Top-right drawer open hotzone enlarged for easier touch activation.

### Fixed (SSH) - 2026-06 Go server / terminal.shop compatibility
- `ssh-ed25519` host key support added via the libssh2 fork
  (`../../libssh2_esp`): ed25519 hostkey verification implemented in the
  mbedTLS backend with vendored ref10 code. ed25519-only servers
  (e.g. `terminal.shop`) now handshake successfully (SSH roadmap Phase 1).
- libssh2 fork now propagates specific KEX failure codes/messages instead of
  the generic `-8 Unable to exchange encryption keys`, including client/server
  algorithm lists on negotiation mismatch.
- PTY startup no longer sends a separate `window-change` request between
  `pty-req` and `shell`; terminal dimensions are passed inside `pty-req`
  via `libssh2_channel_request_pty_ex`. The old ordering wedged Go-based
  SSH servers (x/crypto/ssh, wish/charm): the `shell` request was never
  answered and sessions stayed silent.

### Fixed (Terminal) - 2026-06
- OSC 10/11 color queries (`OSC 10;?` / `OSC 11;?`) are now answered with the
  default palette. bubbletea/lipgloss TUI apps probe these at startup and wait
  for replies before drawing.
- OSC strings terminated with ST (`ESC \`) now dispatch; previously only
  BEL/0x9C-terminated OSC dispatched, so ST-terminated queries were dropped.
- `terminal_write` is now serialized with a mutex. Concurrent writers (SSH
  connect task post-connect clear vs main-loop SSH RX drain) interleaved bytes
  mid-escape-sequence, rendering query text on screen, clobbering CSI params,
  and losing query replies.

### Security
- `.tsauth` (local Tailscale auth key) added to `.gitignore`.

### Added (SSH)
- SSH auth method probing now runs before prompting for credentials.
- Keyboard-interactive authentication fallback support was added.
- SSH diagnostics now log supported client hostkey algorithms and explicit
  warning when `ssh-ed25519` host keys are unavailable in current build.
- SSH now logs negotiated server hostkey fingerprint (SHA1) after handshake,
  laying groundwork for future `known_hosts` trust pinning.
- Initial TOFU host trust behavior now persists first-seen host fingerprint by
  `host:port` and refuses future mismatches.

### Changed (Shell/UI)
- `ssh` command now accepts both `ssh [user@]host[:port]` forms.
- `about` command display line now reports live terminal geometry and cell size.
- Terminal grid sizing now derives from active display resolution instead of
  fixed `100x32`.
- SSH connect failure messages in shell now surface specific cause text when
  available (for example hostkey/auth compatibility issues).

### Changed (SSH)
- Hostkey method preference list is now built from algorithms actually supported
  by the current libssh2 build, preferring `ssh-ed25519`, ECDSA, then RSA.
- Auth flow now attempts methods in order: `none` -> `publickey` (if runtime key
  is loaded) -> password-style fallback.

### Added (Shell)
- New `sshkey` command for vault-backed SSH key management:
  - `sshkey status`
  - `sshkey import`
  - `sshkey load`
  - `sshkey clear`
  - `sshkey erase`

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
