# Minimal OpenSSH test server for DumbESPty SSH auth/trust testing.
# A single image is reused by every endpoint; per-endpoint behavior is driven
# entirely by mounted config + environment variables (see docker-compose.yml).
FROM alpine:3.20

RUN apk add --no-cache openssh openssh-server-pam shadow \
    && mkdir -p /var/empty /run/sshd /keys

# Test login account. Password auth endpoints accept TESTPASS; key/none
# endpoints ignore it. UID kept stable for predictable logs.
ARG TEST_USER=tester
ARG TEST_PASS=testpass
RUN adduser -D -u 2000 "${TEST_USER}" \
    && echo "${TEST_USER}:${TEST_PASS}" | chpasswd

COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
