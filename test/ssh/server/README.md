# DumbESPty SSH Auth/Trust Test Harness

Disposable Docker OpenSSH endpoints for validating the SSH client's
authentication and host-trust behavior on the ESP32-P4 device. Each endpoint
offers a distinct host key type and auth policy so every client code path is
exercised.

## Requirements

- Docker + Docker Compose v2
- Device and this host on the same LAN (device targets `<host-lan-ip>:<port>`)

## Quick start

```bash
cd test/ssh/server
./up.sh        # builds image, starts endpoints, prints a cheat sheet
./status.sh    # show running endpoints + host key fingerprints
./down.sh      # stop endpoints (keys preserved)
./down.sh --purge   # stop + delete all generated keys
```

`up.sh` prints the exact device `ssh` commands, the test password, and the
client private key path to import on device.

## Endpoint matrix

| Endpoint | Port | Host key | Auth mode             | Exercises                 |
|----------|------|----------|-----------------------|---------------------------|
| A        | 2201 | ed25519  | password              | ed25519 hostkey, password |
| B        | 2202 | rsa      | publickey only        | rsa hostkey, publickey, key mgmt |
| C        | 2203 | ecdsa    | keyboard-interactive  | ecdsa hostkey, kbd-interactive |
| D        | 2204 | ed25519  | all methods           | auth-method negotiation   |
| E        | 2205 | rsa      | none (no auth)        | minimal/none auth path    |

Credentials: user `tester`, password `testpass`.
Client key (import on device): `keys/client_id_ed25519`.

## Test procedures

### Host key compatibility
Connect A, B, C. Each must complete the handshake. Device log:
`Server host key: type=<algo> SHA256:...`. No `rc=-5/-8` KEX failure.

### Auth methods
- A: password prompt -> `testpass` -> success.
- B: requires a loaded key (see Key management); without one, expect
  `server requires publickey auth; load key with sshkey load/import`.
- C: keyboard-interactive prompt -> `testpass`.
- D: watch which method the client picks (order: none -> publickey ->
  password/kbdint).
- E: empty-password account. Stock OpenSSH cannot grant the RFC 4252 `none`
  method, so the client succeeds via password with an empty string rather than
  a true protocol-level `none`. For the real `none` success path
  (`Auth OK via 'none' method`), test against a server that permits it
  (e.g. a Go/wish SSH server or patched sshd).

### Key management
On device:
```
sshkey import      # paste contents of keys/client_id_ed25519, set vault pass
sshkey status      # vault key present
sshkey load        # load into runtime
```
Then connect B -> publickey success. Reboot, `sshkey load`, reconnect to
confirm persistence. `sshkey erase` removes it.

### Host trust (TOFU + SHA256 + mismatch)
1. First connect any endpoint -> key pinned. On device: `sshknown list`
   shows `host:port  <type>  SHA256:...`. Compare with `./status.sh`.
2. Reconnect -> log `Host trust verified`.
3. Mismatch: `./rotate-hostkey.sh a-ed25519-password`, reconnect endpoint A
   -> blocked with `Host key mismatch`. Recover:
   `sshknown trust <host-lan-ip>:2201` then reconnect (re-pins new key).
4. `sshknown clear` empties the store; next connect re-pins (TOFU).
5. Legacy upgrade: connect with an old firmware build first (writes a SHA1
   record), then connect with the current build -> log
   `Host trust upgraded to SHA256`.

### Transport robustness
- Idle a session > 30s -> application keepalive keeps it alive (no `rc=-4`).
- Large output (`yes | head -100000`, `dmesg`) -> transport stays stable.
- On a forced failure, device logs name the rc, e.g.
  `rc=-43 (socket-disconnect (transport read/EOF))`.
- Handshake fallback ladder: attempt 1 conservative, attempt 2 broadened
  modern, attempt 3 libssh2 defaults (visible in device log lines
  `Using ... SSH method profile (attempt N)`).

## Notes

- Host keys persist under `keys/<endpoint>/` so identities are stable across
  restarts (required for the trust tests). `rotate-hostkey.sh` intentionally
  regenerates one to simulate a changed key.
- `keys/` is gitignored; nothing secret is committed.
- Alpine `sshd` keyboard-interactive maps onto the local password DB, which is
  sufficient to exercise the client's kbd-interactive code path.
