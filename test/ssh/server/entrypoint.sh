#!/bin/sh
# Per-endpoint sshd launcher for the DumbESPty auth/trust test matrix.
#
# Behavior is driven by environment variables set in docker-compose.yml:
#   HOSTKEY_TYPES   space-separated key types this server offers (ed25519/rsa/ecdsa)
#   AUTH_MODE       one of: password | publickey | kbdint | all | none
#   TEST_USER       login account (default: tester)
#
# Host keys are persisted under /keys/<endpoint> (mounted volume) so a server
# keeps a stable identity across restarts. To simulate a changed host key
# (host-trust mismatch test), delete that endpoint's key dir and restart, OR set
# ROTATE_HOSTKEY=1 to force regeneration on boot.
set -eu

TEST_USER="${TEST_USER:-tester}"
HOSTKEY_TYPES="${HOSTKEY_TYPES:-ed25519}"
AUTH_MODE="${AUTH_MODE:-password}"
KEYDIR="/keys/${ENDPOINT_NAME:-default}"

mkdir -p "${KEYDIR}"

if [ "${ROTATE_HOSTKEY:-0}" = "1" ]; then
    echo "[entrypoint] ROTATE_HOSTKEY=1 -> wiping host keys in ${KEYDIR}"
    rm -f "${KEYDIR}"/ssh_host_*
fi

# Generate any missing host keys for the requested types.
for t in ${HOSTKEY_TYPES}; do
    keyfile="${KEYDIR}/ssh_host_${t}_key"
    if [ ! -f "${keyfile}" ]; then
        echo "[entrypoint] generating ${t} host key"
        ssh-keygen -q -t "${t}" -f "${keyfile}" -N ""
    fi
done

# Build sshd_config from the selected auth policy.
CONF="/etc/ssh/sshd_config"
{
    echo "Port 22"
    echo "ListenAddress 0.0.0.0"
    echo "LogLevel VERBOSE"
    echo "PidFile /run/sshd.pid"
    echo "UsePAM no"
    echo "PermitRootLogin no"
    echo "AllowUsers ${TEST_USER}"
    echo "Subsystem sftp internal-sftp"

    # Offer ONLY the requested host key types (so the client must negotiate
    # the matching algorithm). This exercises host key algorithm negotiation.
    for t in ${HOSTKEY_TYPES}; do
        echo "HostKey ${KEYDIR}/ssh_host_${t}_key"
    done

    # Default everything off, then enable per AUTH_MODE.
    echo "PasswordAuthentication no"
    echo "PubkeyAuthentication no"
    echo "KbdInteractiveAuthentication no"
    echo "PermitEmptyPasswords no"

    case "${AUTH_MODE}" in
        password)
            echo "PasswordAuthentication yes"
            ;;
        publickey)
            echo "PubkeyAuthentication yes"
            echo "AuthorizedKeysFile /keys/authorized_keys"
            ;;
        kbdint)
            echo "KbdInteractiveAuthentication yes"
            # Alpine sshd uses its own keyboard-interactive over password DB.
            echo "AuthenticationMethods keyboard-interactive"
            echo "PasswordAuthentication no"
            ;;
        all)
            echo "PasswordAuthentication yes"
            echo "PubkeyAuthentication yes"
            echo "KbdInteractiveAuthentication yes"
            echo "AuthorizedKeysFile /keys/authorized_keys"
            ;;
        none)
            # NOTE: stock OpenSSH cannot grant the SSH "none" auth method
            # (RFC 4252) on its own; it always requires at least one real
            # method. This endpoint approximates it with an empty-password
            # account so the client succeeds with minimal credentials. The
            # client will still negotiate "password" with an empty string,
            # which is the closest stock sshd can offer. To test the true
            # protocol-level "none" success path, point the device at a server
            # that explicitly permits it (e.g. terminal.shop-style Go servers
            # or a patched sshd).
            echo "PasswordAuthentication yes"
            echo "PermitEmptyPasswords yes"
            passwd -d "${TEST_USER}" || true
            ;;
        *)
            echo "[entrypoint] unknown AUTH_MODE=${AUTH_MODE}" >&2
            exit 1
            ;;
    esac
} > "${CONF}"

echo "[entrypoint] endpoint=${ENDPOINT_NAME:-default} auth=${AUTH_MODE} hostkeys=${HOSTKEY_TYPES}"
echo "[entrypoint] --- sshd_config ---"
cat "${CONF}"
echo "[entrypoint] fingerprints:"
for t in ${HOSTKEY_TYPES}; do
    ssh-keygen -lf "${KEYDIR}/ssh_host_${t}_key" || true
done

exec /usr/sbin/sshd -D -e -f "${CONF}"
