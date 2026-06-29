#ifndef SSH_CLIENT_HPP
#define SSH_CLIENT_HPP

#include <cstdint>
#include <cstddef>

struct terminal_t;

#ifdef __cplusplus
extern "C" {
#endif

bool ssh_connect(const char *host, uint16_t port, const char *user, const char *pass);
bool ssh_last_connect_requires_password(void);
const char *ssh_last_connect_error(void);
bool ssh_set_private_key_pem(const char *private_key_pem, const char *passphrase);
void ssh_clear_private_key(void);
bool ssh_has_private_key(void);

// Known-host trust management (Phase 4).
// host_port is "host:port"; type is the host key algorithm; fingerprint is the
// OpenSSH-style "SHA256:..." form (or legacy SHA1 colon-hex for old records).
typedef void (*ssh_known_host_cb)(const char *host_port,
                                  const char *type,
                                  const char *fingerprint,
                                  void *ctx);
// Enumerates all stored known-host trust records. Returns the count visited.
int ssh_known_hosts_foreach(ssh_known_host_cb cb, void *ctx);
// Removes the trust record for "host:port" (port defaults handled by caller).
// Returns true if a record was removed.
bool ssh_known_host_remove(const char *host, uint16_t port);
// Removes ALL stored known-host trust records. Returns count removed.
int ssh_known_hosts_clear(void);
void ssh_disconnect(void);
bool ssh_is_connected(void);
int ssh_write(const char *data, int len);
void ssh_process_queue(void);
void ssh_set_terminal(struct terminal_t *term);
void ssh_terminal_output_cb(const char *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif
