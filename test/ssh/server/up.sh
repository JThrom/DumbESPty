#!/usr/bin/env bash
# Bring up all DumbESPty SSH test endpoints and print a per-endpoint cheat
# sheet (device command + expected behavior + host key fingerprint).
set -euo pipefail

cd "$(dirname "$0")"

KEYS_DIR="./keys"
CLIENT_KEY="${KEYS_DIR}/client_id_ed25519"
AUTH_KEYS="${KEYS_DIR}/authorized_keys"
TEST_USER="tester"
TEST_PASS="testpass"

mkdir -p "${KEYS_DIR}"

# Generate the device's test client key (the private key you import on device
# via `sshkey import`). ed25519 is small and pasteable over the shell.
if [[ ! -f "${CLIENT_KEY}" ]]; then
    echo "[up] generating client test key: ${CLIENT_KEY}"
    ssh-keygen -q -t ed25519 -f "${CLIENT_KEY}" -N "" -C "dumbespty-test"
fi
cp "${CLIENT_KEY}.pub" "${AUTH_KEYS}"
chmod 600 "${AUTH_KEYS}"

echo "[up] building + starting endpoints..."
docker compose up -d --build

# Detect the host LAN IP the device should target.
LAN_IP="$(ip -4 -o addr show scope global 2>/dev/null \
    | awk '!/docker|br-/ {print $4}' | cut -d/ -f1 | head -n1)"
LAN_IP="${LAN_IP:-<host-lan-ip>}"

cat <<EOF

=========================================================================
 DumbESPty SSH test endpoints are UP. Target host: ${LAN_IP}
 Test user: ${TEST_USER}   Password: ${TEST_PASS}
 Client private key to import on device: ${CLIENT_KEY}
=========================================================================

 A) ed25519 hostkey, PASSWORD auth      -> ssh ${TEST_USER}@${LAN_IP}:2201
    ed25519 hostkey + password auth. Enter: ${TEST_PASS}

 B) rsa hostkey, PUBLICKEY-only auth     -> ssh ${TEST_USER}@${LAN_IP}:2202
    publickey auth + key management. On device first:
      sshkey import   (paste ${CLIENT_KEY}, set a vault password)
      sshkey load
    Then connect. Without a key loaded, expect:
      "server requires publickey auth; load key with sshkey load/import"

 C) ecdsa hostkey, KEYBOARD-INTERACTIVE  -> ssh ${TEST_USER}@${LAN_IP}:2203
    ecdsa hostkey + keyboard-interactive auth. Enter: ${TEST_PASS}

 D) ed25519 hostkey, ALL methods         -> ssh ${TEST_USER}@${LAN_IP}:2204
    auth-method negotiation order (none -> publickey -> password/kbdint).
    Watch device log for which method wins.

 E) rsa hostkey, empty-password (~none)  -> ssh ${TEST_USER}@${LAN_IP}:2205
    minimal-auth path. Stock sshd can't grant true SSH "none";
    client succeeds via empty password. For real "none" use a Go/wish server.

 Host trust on any endpoint:
   1. First connect pins the key. On device: sshknown list
   2. Reconnect -> log "Host trust verified".
   3. Mismatch test: ./rotate-hostkey.sh a-ed25519-password
      Reconnect endpoint A -> blocked with "Host key mismatch".
      Recover: sshknown trust ${LAN_IP}:2201  (then reconnect re-pins)

 Transport: leave a session idle > 30s to exercise keepalive; run long output
   (e.g. 'dmesg', 'yes | head -100000') to stress transport stability.
=========================================================================
EOF
