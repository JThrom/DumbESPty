#!/usr/bin/env bash
# Stop and remove all DumbESPty SSH test endpoints.
# Host keys in ./keys are preserved unless --purge is given.
set -euo pipefail
cd "$(dirname "$0")"

docker compose down

if [[ "${1:-}" == "--purge" ]]; then
    echo "[down] purging ./keys (host keys + client key + authorized_keys)"
    rm -rf ./keys
fi
