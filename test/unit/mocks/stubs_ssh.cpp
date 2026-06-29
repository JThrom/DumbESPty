/* Host-side stubs for libssh2 / mbedtls / coexistence / cross-module symbols
 * referenced by ssh_client.cpp.
 *
 * The SSH transport is never driven in unit tests; these return benign values
 * so the file links. A handful are controllable from tests (see
 * mock_libssh2_* setters) so the pure helpers that read them
 * (build_hostkey_method_pref, get_server_hostkey_sha256, ...) can be
 * exercised. mbedtls_base64_encode is a real implementation. */
#include "libssh2.h"
#include "mbedtls/base64.h"
#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "coex_manager.hpp"

#include <cstring>
#include <cstdint>
#include <vector>
#include <string>

/* ============================ libssh2 ============================== */
namespace {
struct AlgList { std::vector<const char *> algs; };
AlgList g_algs[8];
int g_hostkey_type = LIBSSH2_HOSTKEY_TYPE_ED25519;
std::vector<unsigned char> g_hostkey_bytes;
unsigned char g_sha1[20];
unsigned char g_sha256[32];
bool g_have_sha1 = false, g_have_sha256 = false;
}

extern "C" void mock_libssh2_set_supported_algs(int method_type, const char **algs, int count) {
    if (method_type < 0 || method_type >= 8) return;
    g_algs[method_type].algs.assign(algs, algs + count);
}
extern "C" void mock_libssh2_set_hostkey(int type_id, const unsigned char *key, size_t len) {
    g_hostkey_type = type_id;
    g_hostkey_bytes.assign(key, key + len);
}
extern "C" void mock_libssh2_set_hostkey_hash(int hash_type, const unsigned char *digest) {
    if (hash_type == LIBSSH2_HOSTKEY_HASH_SHA1) { memcpy(g_sha1, digest, 20); g_have_sha1 = true; }
    else if (hash_type == LIBSSH2_HOSTKEY_HASH_SHA256) { memcpy(g_sha256, digest, 32); g_have_sha256 = true; }
}
extern "C" void mock_libssh2_reset(void) {
    for (auto &a : g_algs) a.algs.clear();
    g_hostkey_type = LIBSSH2_HOSTKEY_TYPE_ED25519;
    g_hostkey_bytes.clear();
    g_have_sha1 = g_have_sha256 = false;
}

extern "C" int libssh2_init(int) { return 0; }
extern "C" void libssh2_exit(void) {}
extern "C" void libssh2_free(LIBSSH2_SESSION *, void *) {}

extern "C" LIBSSH2_SESSION *libssh2_session_init_ex(void *, void *, void *, void *) {
    static int s; return (LIBSSH2_SESSION *)&s;
}
extern "C" void libssh2_session_set_blocking(LIBSSH2_SESSION *, int) {}
extern "C" void libssh2_session_set_timeout(LIBSSH2_SESSION *, long) {}
extern "C" int libssh2_session_handshake(LIBSSH2_SESSION *, libssh2_socket_t) { return 0; }
extern "C" int libssh2_session_disconnect_ex(LIBSSH2_SESSION *, int, const char *, const char *) { return 0; }
extern "C" int libssh2_session_free(LIBSSH2_SESSION *) { return 0; }
extern "C" int libssh2_session_last_errno(LIBSSH2_SESSION *) { return 0; }
extern "C" int libssh2_session_last_error(LIBSSH2_SESSION *, char **errmsg, int *errmsg_len, int) {
    if (errmsg) *errmsg = nullptr;
    if (errmsg_len) *errmsg_len = 0;
    return 0;
}
extern "C" int libssh2_session_block_directions(LIBSSH2_SESSION *) { return 0; }
extern "C" const char *libssh2_session_methods(LIBSSH2_SESSION *, int) { return "none"; }
extern "C" int libssh2_session_method_pref(LIBSSH2_SESSION *, int, const char *) { return 0; }
extern "C" int libssh2_session_supported_algs(LIBSSH2_SESSION *, int method_type, const char ***algs) {
    if (method_type < 0 || method_type >= 8) return 0;
    auto &v = g_algs[method_type].algs;
    if (v.empty()) { *algs = nullptr; return 0; }
    *algs = v.data();
    return (int)v.size();
}
extern "C" const char *libssh2_session_hostkey(LIBSSH2_SESSION *, size_t *len, int *type) {
    if (len) *len = g_hostkey_bytes.size();
    if (type) *type = g_hostkey_type;
    return g_hostkey_bytes.empty() ? nullptr : (const char *)g_hostkey_bytes.data();
}
extern "C" const char *libssh2_hostkey_hash(LIBSSH2_SESSION *, int hash_type) {
    if (hash_type == LIBSSH2_HOSTKEY_HASH_SHA1) return g_have_sha1 ? (const char *)g_sha1 : nullptr;
    if (hash_type == LIBSSH2_HOSTKEY_HASH_SHA256) return g_have_sha256 ? (const char *)g_sha256 : nullptr;
    return nullptr;
}

extern "C" char *libssh2_userauth_list(LIBSSH2_SESSION *, const char *, unsigned int) {
    static char m[] = "publickey,password";
    return m;
}
extern "C" int libssh2_userauth_authenticated(LIBSSH2_SESSION *) { return 1; }
extern "C" int libssh2_userauth_password_ex(LIBSSH2_SESSION *, const char *, unsigned int,
                                            const char *, unsigned int, void *) { return 0; }
extern "C" int libssh2_userauth_keyboard_interactive_ex(LIBSSH2_SESSION *, const char *, unsigned int,
                                                        LIBSSH2_USERAUTH_KBDINT_RESPONSE_FUNC_PTR) { return 0; }
extern "C" int libssh2_userauth_publickey_frommemory(LIBSSH2_SESSION *, const char *, size_t,
                                                     const char *, size_t, const char *, size_t,
                                                     const char *) { return 0; }

extern "C" LIBSSH2_CHANNEL *libssh2_channel_open_ex(LIBSSH2_SESSION *, const char *, unsigned int,
                                                    unsigned int, unsigned int, const char *, unsigned int) {
    static int c; return (LIBSSH2_CHANNEL *)&c;
}
extern "C" int libssh2_channel_request_pty_ex(LIBSSH2_CHANNEL *, const char *, unsigned int,
                                              const char *, unsigned int, int, int, int, int) { return 0; }
extern "C" int libssh2_channel_process_startup(LIBSSH2_CHANNEL *, const char *, unsigned int,
                                               const char *, unsigned int) { return 0; }
extern "C" int libssh2_channel_process_startup_no_reply(LIBSSH2_CHANNEL *, const char *, unsigned int,
                                                        const char *, unsigned int) { return 0; }
extern "C" int libssh2_channel_setenv_ex(LIBSSH2_CHANNEL *, const char *, unsigned int,
                                         const char *, unsigned int) { return 0; }
extern "C" long libssh2_channel_read_ex(LIBSSH2_CHANNEL *, int, char *, size_t) { return 0; }
extern "C" long libssh2_channel_write_ex(LIBSSH2_CHANNEL *, int, const char *, size_t buflen) { return (long)buflen; }
extern "C" int libssh2_channel_eof(LIBSSH2_CHANNEL *) { return 1; }
extern "C" int libssh2_channel_send_eof(LIBSSH2_CHANNEL *) { return 0; }
extern "C" int libssh2_channel_wait_eof(LIBSSH2_CHANNEL *) { return 0; }
extern "C" int libssh2_channel_close(LIBSSH2_CHANNEL *) { return 0; }
extern "C" int libssh2_channel_free(LIBSSH2_CHANNEL *) { return 0; }
extern "C" int libssh2_channel_get_exit_status(LIBSSH2_CHANNEL *) { return 0; }
extern "C" void libssh2_channel_set_blocking(LIBSSH2_CHANNEL *, int) {}
extern "C" int libssh2_channel_handle_extended_data2(LIBSSH2_CHANNEL *, int) { return 0; }
extern "C" int libssh2_poll_channel_read(LIBSSH2_CHANNEL *, int) { return 0; }

extern "C" void libssh2_keepalive_config(LIBSSH2_SESSION *, int, unsigned int) {}
extern "C" int libssh2_keepalive_send(LIBSSH2_SESSION *, int *seconds_to_next) {
    if (seconds_to_next) *seconds_to_next = 30;
    return 0;
}
extern "C" void libssh2_trace(LIBSSH2_SESSION *, int) {}
extern "C" int libssh2_trace_sethandler(LIBSSH2_SESSION *, void *, void *) { return 0; }

/* ============================ mbedtls ============================== */
/* Real base64 encoder (RFC 4648) so SHA256 fingerprint formatting is correct. */
extern "C" int mbedtls_base64_encode(unsigned char *dst, size_t dlen, size_t *olen,
                                     const unsigned char *src, size_t slen) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out_needed = ((slen + 2) / 3) * 4;
    if (olen) *olen = out_needed;
    if (!dst || dlen < out_needed + 1) { if (olen) *olen = out_needed + 1; return -0x002A; }
    size_t i = 0, o = 0;
    while (i + 3 <= slen) {
        uint32_t n = (src[i] << 16) | (src[i + 1] << 8) | src[i + 2];
        dst[o++] = tbl[(n >> 18) & 63];
        dst[o++] = tbl[(n >> 12) & 63];
        dst[o++] = tbl[(n >> 6) & 63];
        dst[o++] = tbl[n & 63];
        i += 3;
    }
    if (slen - i == 1) {
        uint32_t n = src[i] << 16;
        dst[o++] = tbl[(n >> 18) & 63];
        dst[o++] = tbl[(n >> 12) & 63];
        dst[o++] = '=';
        dst[o++] = '=';
    } else if (slen - i == 2) {
        uint32_t n = (src[i] << 16) | (src[i + 1] << 8);
        dst[o++] = tbl[(n >> 18) & 63];
        dst[o++] = tbl[(n >> 12) & 63];
        dst[o++] = tbl[(n >> 6) & 63];
        dst[o++] = '=';
    }
    dst[o] = '\0';
    if (olen) *olen = o;
    return 0;
}
extern "C" int mbedtls_base64_decode(unsigned char *, size_t, size_t *olen,
                                     const unsigned char *, size_t) {
    if (olen) *olen = 0;
    return 0;
}

extern "C" void mbedtls_pk_init(mbedtls_pk_context *) {}
extern "C" void mbedtls_pk_free(mbedtls_pk_context *) {}
extern "C" const mbedtls_pk_info_t *mbedtls_pk_info_from_type(mbedtls_pk_type_t) { return nullptr; }
extern "C" int mbedtls_pk_setup(mbedtls_pk_context *, const mbedtls_pk_info_t *) { return 0; }
extern "C" mbedtls_rsa_context *mbedtls_pk_rsa(const mbedtls_pk_context) { return nullptr; }
extern "C" int mbedtls_pk_write_key_pem(mbedtls_pk_context *, unsigned char *buf, size_t size) {
    if (buf && size) buf[0] = '\0';
    return 0;
}
extern "C" int mbedtls_rsa_gen_key(mbedtls_rsa_context *,
                                   int (*)(void *, unsigned char *, size_t),
                                   void *, unsigned int, int) { return 0; }
extern "C" void mbedtls_ctr_drbg_init(mbedtls_ctr_drbg_context *) {}
extern "C" void mbedtls_ctr_drbg_free(mbedtls_ctr_drbg_context *) {}
extern "C" int mbedtls_ctr_drbg_seed(mbedtls_ctr_drbg_context *,
                                     int (*)(void *, unsigned char *, size_t),
                                     void *, const unsigned char *, size_t) { return 0; }
extern "C" int mbedtls_ctr_drbg_random(void *, unsigned char *output, size_t output_len) {
    memset(output, 0, output_len);
    return 0;
}
extern "C" void mbedtls_entropy_init(mbedtls_entropy_context *) {}
extern "C" void mbedtls_entropy_free(mbedtls_entropy_context *) {}
extern "C" int mbedtls_entropy_func(void *, unsigned char *output, size_t len) {
    memset(output, 0, len);
    return 0;
}

/* ============================ select ============================== */
#include "lwip/sockets.h"
extern "C" int lwip_select(int, struct fd_set *, struct fd_set *, struct fd_set *, struct timeval *) {
    return 0;  /* not exercised by unit tests */
}

/* ========================= coexistence ============================= */
extern "C" void coex_acquire(void) {}
extern "C" void coex_release(void) {}

/* ===================== cross-module firmware ======================= */
extern "C" bool wifi_mgr_is_connected(void) { return true; }
extern "C" bool tailscale_mgr_is_connected(void) { return false; }
extern "C" const char *tailscale_mgr_get_vpn_ip(void) { return ""; }
extern "C" bool shell_is_ssh_active(void) { return false; }
extern "C" void shell_set_ssh_active(bool) {}
