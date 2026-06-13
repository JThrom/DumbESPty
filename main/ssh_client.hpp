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
