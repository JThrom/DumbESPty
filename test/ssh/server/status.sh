#!/usr/bin/env bash
# Show running test endpoints and each server's offered host key fingerprints.
set -euo pipefail
cd "$(dirname "$0")"

docker compose ps

echo
echo "Host key fingerprints (compare against device 'sshknown list'):"
for d in ./keys/*/; do
    [[ -d "$d" ]] || continue
    name="$(basename "$d")"
    [[ "$name" == "authorized_keys" ]] && continue
    for kf in "$d"ssh_host_*_key; do
        [[ -f "$kf" ]] || continue
        printf '  %-22s ' "$name"
        ssh-keygen -lf "$kf" 2>/dev/null || echo "(unreadable)"
    done
done
