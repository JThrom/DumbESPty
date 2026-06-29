#!/usr/bin/env bash
# Rotate (regenerate) an endpoint's host key to simulate a changed/rogue host
# key for the host-trust mismatch test.
#
# Usage: ./rotate-hostkey.sh <endpoint-name>
#   endpoint-name is the ENDPOINT_NAME / ./keys subdir, e.g. a-ed25519-password
set -euo pipefail
cd "$(dirname "$0")"

if [[ $# -lt 1 ]]; then
    echo "usage: $0 <endpoint-name>" >&2
    echo "available:" >&2
    ls -1 ./keys 2>/dev/null | grep -v authorized_keys >&2 || true
    exit 1
fi

NAME="$1"
KEYDIR="./keys/${NAME}"
if [[ ! -d "${KEYDIR}" ]]; then
    echo "no such endpoint key dir: ${KEYDIR}" >&2
    exit 1
fi

# Map endpoint dir -> compose service name.
case "${NAME}" in
    a-*) SVC="ssh-ed25519-password" ;;
    b-*) SVC="ssh-rsa-publickey" ;;
    c-*) SVC="ssh-ecdsa-kbdint" ;;
    d-*) SVC="ssh-all-methods" ;;
    e-*) SVC="ssh-none" ;;
    *)   echo "cannot map ${NAME} to a service" >&2; exit 1 ;;
esac

echo "[rotate] deleting host keys in ${KEYDIR}"
rm -f "${KEYDIR}"/ssh_host_*

echo "[rotate] restarting ${SVC} (will regenerate a fresh host key)"
docker compose restart "${SVC}"

sleep 1
echo "[rotate] new fingerprint(s):"
for kf in "${KEYDIR}"/ssh_host_*_key; do
    [[ -f "$kf" ]] && ssh-keygen -lf "$kf"
done
echo "[rotate] reconnect this endpoint on device to trigger 'Host key mismatch'."
