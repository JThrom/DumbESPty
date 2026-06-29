/* Host test mock of libssh2.h.
 *
 * Models the constant/type/function surface that ssh_client.cpp references so
 * it compiles and links on host. The SSH *transport* is never driven by the
 * unit tests; only pure helper logic (transport-rc decoding, FNV trust-key
 * hashing, hostkey-method preference building, SHA256 fingerprint formatting)
 * is exercised. Stub function bodies live in stubs_ssh.cpp and return benign
 * values; a few (supported_algs, hostkey, hostkey_hash) are made controllable
 * from tests via mock_libssh2_* setters. */
#ifndef MOCK_LIBSSH2_H
#define MOCK_LIBSSH2_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------- error codes ------------------------- */
#define LIBSSH2_ERROR_NONE                    0
#define LIBSSH2_ERROR_SOCKET_NONE            -1
#define LIBSSH2_ERROR_BANNER_RECV            -2
#define LIBSSH2_ERROR_BANNER_SEND            -3
#define LIBSSH2_ERROR_INVALID_MAC            -4
#define LIBSSH2_ERROR_KEX_FAILURE            -5
#define LIBSSH2_ERROR_ALLOC                  -6
#define LIBSSH2_ERROR_SOCKET_SEND            -7
#define LIBSSH2_ERROR_KEY_EXCHANGE_FAILURE   -8
#define LIBSSH2_ERROR_TIMEOUT                -9
#define LIBSSH2_ERROR_HOSTKEY_INIT           -10
#define LIBSSH2_ERROR_HOSTKEY_SIGN           -11
#define LIBSSH2_ERROR_DECRYPT                -12
#define LIBSSH2_ERROR_SOCKET_DISCONNECT      -13
#define LIBSSH2_ERROR_PROTO                  -14
#define LIBSSH2_ERROR_PASSWORD_EXPIRED       -15
#define LIBSSH2_ERROR_METHOD_NONE            -17
#define LIBSSH2_ERROR_AUTHENTICATION_FAILED  -18
#define LIBSSH2_ERROR_PUBLICKEY_UNVERIFIED   -19
#define LIBSSH2_ERROR_CHANNEL_FAILURE        -21
#define LIBSSH2_ERROR_CHANNEL_CLOSED         -26
#define LIBSSH2_ERROR_CHANNEL_EOF_SENT       -27
#define LIBSSH2_ERROR_BAD_USE                -34
#define LIBSSH2_ERROR_EAGAIN                 -37
#define LIBSSH2_ERROR_BUFFER_TOO_SMALL       -38
#define LIBSSH2_ERROR_INVAL                  -34

/* ----------------------------- method types ------------------------ */
#define LIBSSH2_METHOD_KEX        0
#define LIBSSH2_METHOD_HOSTKEY    1
#define LIBSSH2_METHOD_CRYPT_CS   2
#define LIBSSH2_METHOD_CRYPT_SC   3
#define LIBSSH2_METHOD_MAC_CS     4
#define LIBSSH2_METHOD_MAC_SC     5

/* ----------------------------- hostkey ----------------------------- */
#define LIBSSH2_HOSTKEY_HASH_SHA1    1
#define LIBSSH2_HOSTKEY_HASH_SHA256  3

#define LIBSSH2_HOSTKEY_TYPE_UNKNOWN     0
#define LIBSSH2_HOSTKEY_TYPE_RSA         1
#define LIBSSH2_HOSTKEY_TYPE_DSS         2
#define LIBSSH2_HOSTKEY_TYPE_ECDSA_256   3
#define LIBSSH2_HOSTKEY_TYPE_ECDSA_384   4
#define LIBSSH2_HOSTKEY_TYPE_ECDSA_521   5
#define LIBSSH2_HOSTKEY_TYPE_ED25519     6

/* ----------------------------- session flags ----------------------- */
#define LIBSSH2_SESSION_BLOCK_INBOUND   1
#define LIBSSH2_SESSION_BLOCK_OUTBOUND  2

#define LIBSSH2_CHANNEL_EXTENDED_DATA_NORMAL   0
#define LIBSSH2_CHANNEL_EXTENDED_DATA_IGNORE   1
#define LIBSSH2_CHANNEL_EXTENDED_DATA_MERGE    2

#define LIBSSH2_TRACE_TRANS   (1 << 1)
#define LIBSSH2_TRACE_KEX     (1 << 2)
#define LIBSSH2_TRACE_AUTH    (1 << 3)
#define LIBSSH2_TRACE_SOCKET  (1 << 5)
#define LIBSSH2_TRACE_ERROR   (1 << 8)

/* ----------------------------- types ------------------------------- */
typedef struct LIBSSH2_SESSION LIBSSH2_SESSION;
typedef struct LIBSSH2_CHANNEL LIBSSH2_CHANNEL;
typedef int libssh2_socket_t;

/* keyboard-interactive prompt/response structures + callback macro,
 * matching the real libssh2 public API shape used by ssh_client.cpp. */
typedef struct {
    char *text;
    unsigned int length;
    unsigned char echo;
} LIBSSH2_USERAUTH_KBDINT_PROMPT;

typedef struct {
    char *text;
    unsigned int length;
} LIBSSH2_USERAUTH_KBDINT_RESPONSE;

#define LIBSSH2_USERAUTH_KBDINT_RESPONSE_FUNC(name_) \
    void name_(const char *name, int name_len, \
               const char *instruction, int instruction_len, \
               int num_prompts, \
               const LIBSSH2_USERAUTH_KBDINT_PROMPT *prompts, \
               LIBSSH2_USERAUTH_KBDINT_RESPONSE *responses, \
               void **abstract)

typedef void (*LIBSSH2_USERAUTH_KBDINT_RESPONSE_FUNC_PTR)(
    const char *, int, const char *, int, int,
    const LIBSSH2_USERAUTH_KBDINT_PROMPT *,
    LIBSSH2_USERAUTH_KBDINT_RESPONSE *, void **);

/* ----------------------------- core lib ---------------------------- */
int libssh2_init(int flags);
void libssh2_exit(void);
void libssh2_free(LIBSSH2_SESSION *session, void *ptr);

/* ----------------------------- session ----------------------------- */
LIBSSH2_SESSION *libssh2_session_init_ex(void *a, void *b, void *c, void *d);
#define libssh2_session_init() libssh2_session_init_ex(NULL, NULL, NULL, NULL)
void libssh2_session_set_blocking(LIBSSH2_SESSION *session, int blocking);
void libssh2_session_set_timeout(LIBSSH2_SESSION *session, long timeout);
int libssh2_session_handshake(LIBSSH2_SESSION *session, libssh2_socket_t sock);
int libssh2_session_disconnect_ex(LIBSSH2_SESSION *session, int reason, const char *desc, const char *lang);
#define libssh2_session_disconnect(s, d) libssh2_session_disconnect_ex((s), 11, (d), "")
int libssh2_session_free(LIBSSH2_SESSION *session);
int libssh2_session_last_errno(LIBSSH2_SESSION *session);
int libssh2_session_last_error(LIBSSH2_SESSION *session, char **errmsg, int *errmsg_len, int want_buf);
int libssh2_session_block_directions(LIBSSH2_SESSION *session);
const char *libssh2_session_methods(LIBSSH2_SESSION *session, int method_type);
int libssh2_session_method_pref(LIBSSH2_SESSION *session, int method_type, const char *prefs);
int libssh2_session_supported_algs(LIBSSH2_SESSION *session, int method_type, const char ***algs);
const char *libssh2_session_hostkey(LIBSSH2_SESSION *session, size_t *len, int *type);

/* ----------------------------- hostkey hash ------------------------ */
const char *libssh2_hostkey_hash(LIBSSH2_SESSION *session, int hash_type);

/* ----------------------------- userauth ---------------------------- */
char *libssh2_userauth_list(LIBSSH2_SESSION *session, const char *username, unsigned int len);
int libssh2_userauth_authenticated(LIBSSH2_SESSION *session);
int libssh2_userauth_password_ex(LIBSSH2_SESSION *session, const char *username, unsigned int ulen,
                                 const char *password, unsigned int plen, void *cb);
#define libssh2_userauth_password(s, u, p) \
    libssh2_userauth_password_ex((s), (u), (unsigned int)strlen(u), (p), (unsigned int)strlen(p), NULL)
int libssh2_userauth_keyboard_interactive_ex(LIBSSH2_SESSION *session, const char *username,
                                             unsigned int ulen,
                                             LIBSSH2_USERAUTH_KBDINT_RESPONSE_FUNC_PTR cb);
#define libssh2_userauth_keyboard_interactive(s, u, cb) \
    libssh2_userauth_keyboard_interactive_ex((s), (u), (unsigned int)strlen(u), (cb))
int libssh2_userauth_publickey_frommemory(LIBSSH2_SESSION *session, const char *username, size_t ulen,
                                          const char *pubkey, size_t pubkey_len,
                                          const char *privkey, size_t privkey_len,
                                          const char *passphrase);

/* ----------------------------- channel ----------------------------- */
LIBSSH2_CHANNEL *libssh2_channel_open_ex(LIBSSH2_SESSION *session, const char *type, unsigned int type_len,
                                         unsigned int win, unsigned int packet, const char *msg, unsigned int msg_len);
#define libssh2_channel_open_session(s) \
    libssh2_channel_open_ex((s), "session", 7, 2 * 1024 * 1024, 32768, NULL, 0)
int libssh2_channel_request_pty_ex(LIBSSH2_CHANNEL *channel, const char *term, unsigned int term_len,
                                   const char *modes, unsigned int modes_len,
                                   int width, int height, int width_px, int height_px);
int libssh2_channel_process_startup(LIBSSH2_CHANNEL *channel, const char *req, unsigned int req_len,
                                    const char *msg, unsigned int msg_len);
#define libssh2_channel_shell(c) libssh2_channel_process_startup((c), "shell", 5, NULL, 0)
#define libssh2_channel_exec(c, cmd) libssh2_channel_process_startup((c), "exec", 4, (cmd), (unsigned int)strlen(cmd))
int libssh2_channel_process_startup_no_reply(LIBSSH2_CHANNEL *channel, const char *req, unsigned int req_len,
                                             const char *msg, unsigned int msg_len);
int libssh2_channel_setenv_ex(LIBSSH2_CHANNEL *channel, const char *var, unsigned int var_len,
                              const char *val, unsigned int val_len);
long libssh2_channel_read_ex(LIBSSH2_CHANNEL *channel, int stream_id, char *buf, size_t buflen);
#define libssh2_channel_read(c, b, l) libssh2_channel_read_ex((c), 0, (b), (l))
long libssh2_channel_write_ex(LIBSSH2_CHANNEL *channel, int stream_id, const char *buf, size_t buflen);
#define libssh2_channel_write(c, b, l) libssh2_channel_write_ex((c), 0, (b), (l))
int libssh2_channel_eof(LIBSSH2_CHANNEL *channel);
int libssh2_channel_send_eof(LIBSSH2_CHANNEL *channel);
int libssh2_channel_wait_eof(LIBSSH2_CHANNEL *channel);
int libssh2_channel_close(LIBSSH2_CHANNEL *channel);
int libssh2_channel_free(LIBSSH2_CHANNEL *channel);
int libssh2_channel_get_exit_status(LIBSSH2_CHANNEL *channel);
void libssh2_channel_set_blocking(LIBSSH2_CHANNEL *channel, int blocking);
int libssh2_channel_handle_extended_data2(LIBSSH2_CHANNEL *channel, int mode);
int libssh2_poll_channel_read(LIBSSH2_CHANNEL *channel, int extended);

/* ----------------------------- keepalive --------------------------- */
void libssh2_keepalive_config(LIBSSH2_SESSION *session, int want_reply, unsigned int interval);
int libssh2_keepalive_send(LIBSSH2_SESSION *session, int *seconds_to_next);

/* ----------------------------- trace ------------------------------- */
void libssh2_trace(LIBSSH2_SESSION *session, int bitmask);
int libssh2_trace_sethandler(LIBSSH2_SESSION *session, void *context, void *handler);

/* --------------------- test control hooks (mock-only) -------------- */
/* Set the algorithm list returned by libssh2_session_supported_algs. */
void mock_libssh2_set_supported_algs(int method_type, const char **algs, int count);
/* Set the host key type id + raw bytes returned by libssh2_session_hostkey. */
void mock_libssh2_set_hostkey(int type_id, const unsigned char *key, size_t len);
/* Set the digest bytes returned by libssh2_hostkey_hash for a given hash type. */
void mock_libssh2_set_hostkey_hash(int hash_type, const unsigned char *digest);
void mock_libssh2_reset(void);

#ifdef __cplusplus
}
#endif

#endif
