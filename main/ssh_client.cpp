#include "ssh_client.hpp"
#include "shell.hpp"
#include "power_mgr.hpp"
#include "wifi_mgr.hpp"
#include "tailscale_mgr.hpp"
#include "terminal.hpp"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include "netinet/tcp.h"
#include "nvs.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include "libssh2.h"
#include "coex_manager.hpp"
#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/base64.h"

static const char *TAG = "SSH_CLIENT";

#define SSH_RX_QUEUE_SIZE 64
#define SSH_RX_BUF_SIZE 256
#define SSH_HANDSHAKE_TIMEOUT_MS 45000
#define SSH_CONNECT_TIMEOUT_MS 5000
#define SSH_CONNECT_RETRIES 3
#define SSH_HANDSHAKE_RETRIES 3
#define SSH_VERBOSE_LOGS 0
#define SSH_USE_RX_TASK 0
#define SSH_VERBOSE_RX_LOGS 0
#define SSH_PRIVATE_KEY_MAX_LEN 4096
#define SSH_KEY_PASSPHRASE_MAX_LEN 128

typedef struct {
    char data[SSH_RX_BUF_SIZE];
    uint16_t len;
} ssh_rx_msg_t;

static bool ssh_connected = false;
static bool s_ssh_connecting = false;
static int ssh_sock = -1;
static LIBSSH2_SESSION *session = NULL;
static LIBSSH2_CHANNEL *channel = NULL;
static QueueHandle_t ssh_rx_queue = NULL;
static StaticQueue_t *ssh_rx_queue_static_obj = NULL;
static uint8_t *ssh_rx_queue_static_storage = NULL;
static TaskHandle_t ssh_recv_task_handle = NULL;
static terminal_t *ssh_term = NULL;
static SemaphoreHandle_t ssh_write_mutex = NULL;
static constexpr int kRxTraceBudgetDefault = SSH_VERBOSE_RX_LOGS ? 160 : 0;
static constexpr int kRxPreviewBudgetDefault = SSH_VERBOSE_RX_LOGS ? 40 : 0;
static constexpr int kRxDiagBudgetDefault = SSH_VERBOSE_RX_LOGS ? 120 : 0;
static int s_rx_trace_budget = kRxTraceBudgetDefault;
static int s_rx_preview_budget = kRxPreviewBudgetDefault;
static uint8_t s_rx_prev_tail = 0;
static int s_rx_loop_budget = 120;
static int s_queue_drain_budget = 80;
static uint32_t s_rx_raw_total = 0;
static uint32_t s_rx_forwarded_total = 0;
static uint32_t s_rx_filtered_total = 0;
static uint32_t s_rx_queue_drop_total = 0;
static uint32_t s_rx_raw_prev = 0;
static uint32_t s_rx_forwarded_prev = 0;
static uint32_t s_rx_filtered_prev = 0;
static uint32_t s_rx_queue_drop_prev = 0;
static TickType_t s_rx_diag_last_log = 0;
static int s_rx_diag_budget = kRxDiagBudgetDefault;
static uint32_t s_tx_total = 0;
static uint32_t s_tx_prev = 0;
// Phase 5: application-level SSH keepalive. libssh2 tracks the configured
// interval; we drive libssh2_keepalive_send() from the main-loop pump so idle
// sessions exchange traffic and dead links are detected instead of silently
// stalling into rc=-4 transport reads.
static constexpr int kSshKeepaliveIntervalSec = 30;
static TickType_t s_keepalive_last_send = 0;
static bool s_last_connect_requires_password = false;
static char s_last_connect_error[192] = "";
static const char *s_kbdint_password = NULL;
static char s_ssh_private_key_pem[SSH_PRIVATE_KEY_MAX_LEN] = "";
static char s_ssh_private_key_passphrase[SSH_KEY_PASSPHRASE_MAX_LEN] = "";
static constexpr char SSH_TRUST_NS[] = "sshtrust";
static constexpr char SSH_IDENT_NS[] = "sshident";
static constexpr char SSH_IDENT_KEY_DEFAULT[] = "default_priv";

static inline void secure_zero_local(void *p, size_t len);

bool ssh_last_connect_requires_password(void) {
    return s_last_connect_requires_password;
}

const char *ssh_last_connect_error(void) {
    return s_last_connect_error;
}

bool ssh_set_private_key_pem(const char *private_key_pem, const char *passphrase) {
    if (!private_key_pem) return false;
    size_t n = strnlen(private_key_pem, SSH_PRIVATE_KEY_MAX_LEN);
    if (n == 0 || n >= SSH_PRIVATE_KEY_MAX_LEN) {
        return false;
    }

    memset(s_ssh_private_key_pem, 0, sizeof(s_ssh_private_key_pem));
    memcpy(s_ssh_private_key_pem, private_key_pem, n);
    s_ssh_private_key_pem[n] = '\0';

    memset(s_ssh_private_key_passphrase, 0, sizeof(s_ssh_private_key_passphrase));
    if (passphrase && passphrase[0]) {
        size_t pn = strnlen(passphrase, SSH_KEY_PASSPHRASE_MAX_LEN);
        if (pn >= SSH_KEY_PASSPHRASE_MAX_LEN) {
            secure_zero_local(s_ssh_private_key_pem, sizeof(s_ssh_private_key_pem));
            s_ssh_private_key_pem[0] = '\0';
            return false;
        }
        memcpy(s_ssh_private_key_passphrase, passphrase, pn);
        s_ssh_private_key_passphrase[pn] = '\0';
    }

    return true;
}

void ssh_clear_private_key(void) {
    secure_zero_local(s_ssh_private_key_pem, sizeof(s_ssh_private_key_pem));
    secure_zero_local(s_ssh_private_key_passphrase, sizeof(s_ssh_private_key_passphrase));
    s_ssh_private_key_pem[0] = '\0';
    s_ssh_private_key_passphrase[0] = '\0';
}

bool ssh_has_private_key(void) {
    return s_ssh_private_key_pem[0] != '\0';
}

static void set_last_connect_error(const char *fmt, ...) {
    if (!fmt || !fmt[0]) {
        s_last_connect_error[0] = '\0';
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_last_connect_error, sizeof(s_last_connect_error), fmt, ap);
    va_end(ap);
}

// Phase 5: human-readable names for the libssh2 transport/runtime return codes
// that show up in rc<0 disconnect logs, so failures are attributable instead
// of being bare integers.
static const char *ssh_transport_rc_str(int rc) {
    switch (rc) {
        case LIBSSH2_ERROR_NONE:                   return "none";
        case LIBSSH2_ERROR_SOCKET_NONE:            return "socket-none";
        case LIBSSH2_ERROR_BANNER_RECV:            return "banner-recv";
        case LIBSSH2_ERROR_BANNER_SEND:            return "banner-send";
        case LIBSSH2_ERROR_INVALID_MAC:            return "invalid-mac";
        case LIBSSH2_ERROR_KEX_FAILURE:            return "kex-failure";
        case LIBSSH2_ERROR_ALLOC:                  return "alloc";
        case LIBSSH2_ERROR_SOCKET_SEND:            return "socket-send";
        case LIBSSH2_ERROR_KEY_EXCHANGE_FAILURE:   return "key-exchange-failure";
        case LIBSSH2_ERROR_TIMEOUT:                return "timeout";
        case LIBSSH2_ERROR_HOSTKEY_INIT:           return "hostkey-init";
        case LIBSSH2_ERROR_HOSTKEY_SIGN:           return "hostkey-sign";
        case LIBSSH2_ERROR_DECRYPT:                return "decrypt";
        case LIBSSH2_ERROR_SOCKET_DISCONNECT:      return "socket-disconnect (transport read/EOF)";
        case LIBSSH2_ERROR_PROTO:                  return "protocol-error";
        case LIBSSH2_ERROR_PASSWORD_EXPIRED:       return "password-expired";
        case LIBSSH2_ERROR_METHOD_NONE:            return "method-none";
        case LIBSSH2_ERROR_AUTHENTICATION_FAILED:  return "auth-failed";
        case LIBSSH2_ERROR_PUBLICKEY_UNVERIFIED:   return "publickey-unverified";
        case LIBSSH2_ERROR_CHANNEL_FAILURE:        return "channel-failure";
        case LIBSSH2_ERROR_CHANNEL_CLOSED:         return "channel-closed";
        case LIBSSH2_ERROR_CHANNEL_EOF_SENT:       return "channel-eof-sent";
        case LIBSSH2_ERROR_BAD_USE:                return "bad-use";
        case LIBSSH2_ERROR_EAGAIN:                 return "eagain (would-block)";
        case LIBSSH2_ERROR_BUFFER_TOO_SMALL:       return "buffer-too-small";
        default: break;
    }
    return "unknown";
}

static size_t filter_and_reply_fast_queries(const char *src, size_t len, char *dst);
static void log_rx_trace(const char *data, size_t len);
static void log_ssh_runtime_error_context(const char *where, int rc);
static bool session_supports_alg(LIBSSH2_SESSION *sess, int method_type, const char *alg);
static void log_supported_hostkey_algs(LIBSSH2_SESSION *sess);
static bool build_hostkey_method_pref(LIBSSH2_SESSION *sess,
                                      char *out,
                                      size_t out_len,
                                      bool *has_ed25519);
static void log_server_hostkey_fingerprint(LIBSSH2_SESSION *sess);
static bool get_server_hostkey_sha1(LIBSSH2_SESSION *sess,
                                    char *fp,
                                    size_t fp_len,
                                    char *hostkey_type,
                                    size_t hostkey_type_len);
static bool get_server_hostkey_sha256(LIBSSH2_SESSION *sess,
                                      char *fp,
                                      size_t fp_len,
                                      char *hostkey_type,
                                      size_t hostkey_type_len);
static bool verify_or_store_host_fingerprint(const char *host, uint16_t port, LIBSSH2_SESSION *sess);
static uint32_t fnv1a_32(const char *s);
static bool make_trust_key(const char *host, uint16_t port, char *out, size_t out_len);
static bool load_trust_record(const char *trust_key,
                              char *host_port_out,
                              size_t host_port_out_len,
                              char *type_out,
                              size_t type_out_len,
                              char *fingerprint_out,
                              size_t fingerprint_out_len);
static bool save_trust_record(const char *trust_key,
                              const char *host_port,
                              const char *type,
                              const char *fingerprint);
static bool ensure_default_identity_key_loaded(void);

static bool load_default_identity_key(char *out, size_t out_len) {
    if (!out || out_len < 2) return false;
    out[0] = '\0';

    nvs_handle_t nvs = 0;
    if (nvs_open(SSH_IDENT_NS, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }

    size_t need = out_len;
    esp_err_t err = nvs_get_str(nvs, SSH_IDENT_KEY_DEFAULT, out, &need);
    nvs_close(nvs);
    if (err != ESP_OK || need == 0 || out[0] == '\0') {
        out[0] = '\0';
        return false;
    }

    return true;
}

static bool save_default_identity_key(const char *pem) {
    if (!pem || !pem[0]) return false;

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(SSH_IDENT_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return false;

    err = nvs_set_str(nvs, SSH_IDENT_KEY_DEFAULT, pem);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    return err == ESP_OK;
}

static bool generate_rsa_identity_pem(char *out, size_t out_len) {
    if (!out || out_len < 64) return false;
    out[0] = '\0';

    mbedtls_pk_context pk;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    const char *pers = "dumbespty_ssh_identity";
    int rc = -1;

    mbedtls_pk_init(&pk);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    rc = mbedtls_ctr_drbg_seed(&ctr_drbg,
                               mbedtls_entropy_func,
                               &entropy,
                               (const unsigned char *)pers,
                               strlen(pers));
    if (rc != 0) goto clean_exit;

    rc = mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    if (rc != 0) goto clean_exit;

    rc = mbedtls_rsa_gen_key(mbedtls_pk_rsa(pk),
                             mbedtls_ctr_drbg_random,
                             &ctr_drbg,
                             2048,
                             65537);
    if (rc != 0) goto clean_exit;

    rc = mbedtls_pk_write_key_pem(&pk, (unsigned char *)out, out_len);
    if (rc != 0 || strstr(out, "-----BEGIN") == NULL) {
        out[0] = '\0';
        goto clean_exit;
    }

clean_exit:
    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return rc == 0;
}

static bool ensure_default_identity_key_loaded(void) {
    if (ssh_has_private_key()) {
        return true;
    }

    char pem[SSH_PRIVATE_KEY_MAX_LEN];
    memset(pem, 0, sizeof(pem));

    if (load_default_identity_key(pem, sizeof(pem))) {
        bool ok = ssh_set_private_key_pem(pem, "");
        secure_zero_local(pem, sizeof(pem));
        if (ok) {
            ESP_LOGI(TAG, "Loaded default SSH identity key from NVS");
            return true;
        }
        ESP_LOGW(TAG, "Stored default SSH identity key is invalid");
        return false;
    }

    ESP_LOGI(TAG, "Generating default RSA identity key...");
    if (!generate_rsa_identity_pem(pem, sizeof(pem))) {
        secure_zero_local(pem, sizeof(pem));
        ESP_LOGE(TAG, "Failed to generate default SSH identity key");
        return false;
    }

    bool set_ok = ssh_set_private_key_pem(pem, "");
    bool save_ok = set_ok && save_default_identity_key(pem);
    secure_zero_local(pem, sizeof(pem));

    if (!set_ok || !save_ok) {
        ESP_LOGE(TAG, "Failed to persist default SSH identity key");
        return false;
    }

    ESP_LOGI(TAG, "Generated and saved default SSH identity key");
    return true;
}

static inline void secure_zero_local(void *p, size_t len) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (len--) *v++ = 0;
}

static void ssh_handle_rx_bytes(const char *src, size_t len) {
    if (len > 0 && s_rx_preview_budget > 0) {
        char preview[80];
        size_t sample = len < 32 ? len : 32;
        size_t p = 0;
        for (size_t i = 0; i < sample && p < sizeof(preview) - 1; i++) {
            uint8_t b = (uint8_t)src[i];
            preview[p++] = (b >= 0x20 && b <= 0x7E) ? (char)b : '.';
        }
        preview[p] = '\0';
        ESP_LOGI(TAG, "RX %uB preview: %s", (unsigned)len, preview);
        s_rx_preview_budget--;
    }

    size_t offset = 0;
    while (offset < len) {
        ssh_rx_msg_t msg;
        size_t chunk = len - offset;
        if (chunk > SSH_RX_BUF_SIZE - 1) chunk = SSH_RX_BUF_SIZE - 1;
        msg.len = filter_and_reply_fast_queries(src + offset, chunk, msg.data);
        if (chunk >= msg.len) {
            s_rx_filtered_total += (uint32_t)(chunk - msg.len);
        }
        log_rx_trace(msg.data, msg.len);
        if (msg.len > 0) {
            if (xQueueSend(ssh_rx_queue, &msg, pdMS_TO_TICKS(20)) == pdTRUE) {
                s_rx_forwarded_total += (uint32_t)msg.len;
            } else {
                s_rx_queue_drop_total += (uint32_t)msg.len;
                if (s_rx_loop_budget > 0) {
                    ESP_LOGW(TAG, "SSH rx queue full, dropping %u bytes", (unsigned)msg.len);
                    s_rx_loop_budget--;
                }
            }
        }
        offset += chunk;
    }
}

static bool ssh_pump_channel_stream_once(int stream_id) {
    if (!channel || !ssh_rx_queue) return false;

    char buf[1024];
    int rc = libssh2_channel_read_ex(channel, stream_id, buf, sizeof(buf) - 1);
    if (rc > 0) {
        s_rx_raw_total += (uint32_t)rc;
        if (s_rx_loop_budget > 0) s_rx_loop_budget--;
        buf[rc] = '\0';
        ssh_handle_rx_bytes(buf, (size_t)rc);
        return true;
    }

    if (rc == LIBSSH2_ERROR_EAGAIN || rc == 0) {
        return false;
    }

    log_ssh_runtime_error_context(stream_id == 1 ? "SSH stderr read error" : "SSH rx read error", rc);
    ESP_LOGW(TAG, "Marking SSH disconnected after read rc=%d stream=%d", rc, stream_id);
    ssh_connected = false;
    return false;
}

static void ssh_pump_rx_once(int max_reads) {
    if (!ssh_connected || !channel || !ssh_rx_queue) return;

    for (int i = 0; i < max_reads; i++) {
        bool got_stdout = ssh_pump_channel_stream_once(0);
        bool got_stderr = ssh_pump_channel_stream_once(1);
        if (!got_stdout && !got_stderr) {
            break;
        }
    }

    if (libssh2_channel_eof(channel)) {
        int exit_status = libssh2_channel_get_exit_status(channel);
        ESP_LOGW(TAG, "Remote channel EOF (exit_status=%d)", exit_status);
        ssh_connected = false;
    }
}

#if SSH_VERBOSE_LOGS
static int s_ssh_trace_budget = 1200;

static void ssh_trace_handler(LIBSSH2_SESSION *session, void *context,
                              const char *data, size_t len) {
    (void)session;
    (void)context;
    if (!data || len == 0 || s_ssh_trace_budget <= 0) return;

    size_t take = len;
    if (take > 180) take = 180;
    char line[192];
    memcpy(line, data, take);
    line[take] = '\0';
    ESP_LOGI(TAG, "libssh2: %s", line);
    s_ssh_trace_budget--;
}

static void log_supported_algs(LIBSSH2_SESSION *sess, int method_type, const char *label) {
    const char **algs = NULL;
    int n = libssh2_session_supported_algs(sess, method_type, &algs);
    if (n < 0 || !algs) {
        ESP_LOGW(TAG, "supported_algs(%s) unavailable: %d", label, n);
        return;
    }

    char buf[320];
    int p = snprintf(buf, sizeof(buf), "%s:", label);
    for (int i = 0; i < n; i++) {
        p += snprintf(buf + p, sizeof(buf) - p, "%s%s", (i == 0 ? " " : ","), algs[i]);
        if (p >= (int)sizeof(buf) - 32) break;
    }
    ESP_LOGI(TAG, "%s", buf);
    libssh2_free(sess, (void *)algs);
}
#endif

static bool session_supports_alg(LIBSSH2_SESSION *sess, int method_type, const char *alg) {
    if (!sess || !alg || !alg[0]) return false;
    const char **algs = NULL;
    int n = libssh2_session_supported_algs(sess, method_type, &algs);
    bool found = false;
    if (n > 0 && algs) {
        for (int i = 0; i < n; i++) {
            if (algs[i] && strcmp(algs[i], alg) == 0) {
                found = true;
                break;
            }
        }
    }
    if (algs) {
        libssh2_free(sess, (void *)algs);
    }
    return found;
}

static void log_supported_hostkey_algs(LIBSSH2_SESSION *sess) {
    if (!sess) return;
    const char **algs = NULL;
    int n = libssh2_session_supported_algs(sess, LIBSSH2_METHOD_HOSTKEY, &algs);
    if (n < 0 || !algs) {
        ESP_LOGW(TAG, "Unable to enumerate client hostkey algorithms");
        return;
    }

    char buf[256];
    int p = snprintf(buf, sizeof(buf), "Client hostkey algorithms:");
    for (int i = 0; i < n; i++) {
        p += snprintf(buf + p, sizeof(buf) - p, "%s%s", (i == 0 ? " " : ","), algs[i]);
        if (p >= (int)sizeof(buf) - 24) break;
    }
    ESP_LOGI(TAG, "%s", buf);
    libssh2_free(sess, (void *)algs);
}

static bool build_hostkey_method_pref(LIBSSH2_SESSION *sess,
                                      char *out,
                                      size_t out_len,
                                      bool *has_ed25519) {
    if (!sess || !out || out_len == 0) return false;
    out[0] = '\0';
    if (has_ed25519) *has_ed25519 = false;

    static const char *kHostkeyPreferenceOrder[] = {
        "ssh-ed25519",
        "ecdsa-sha2-nistp256",
        "ecdsa-sha2-nistp384",
        "ecdsa-sha2-nistp521",
        "rsa-sha2-512",
        "rsa-sha2-256",
        "ssh-rsa",
    };

    size_t p = 0;
    bool added_any = false;
    for (size_t i = 0; i < sizeof(kHostkeyPreferenceOrder) / sizeof(kHostkeyPreferenceOrder[0]); i++) {
        const char *alg = kHostkeyPreferenceOrder[i];
        if (!session_supports_alg(sess, LIBSSH2_METHOD_HOSTKEY, alg)) {
            continue;
        }

        int nw = snprintf(out + p, out_len - p, "%s%s", added_any ? "," : "", alg);
        if (nw <= 0 || (size_t)nw >= (out_len - p)) {
            break;
        }
        p += (size_t)nw;
        added_any = true;
        if (has_ed25519 && strcmp(alg, "ssh-ed25519") == 0) {
            *has_ed25519 = true;
        }
    }
    return added_any;
}

static bool get_server_hostkey_sha1(LIBSSH2_SESSION *sess,
                                    char *fp,
                                    size_t fp_len,
                                    char *hostkey_type,
                                    size_t hostkey_type_len) {
    if (!sess || !fp || fp_len == 0) return false;

    size_t hostkey_len = 0;
    int hostkey_type_id = 0;
    const char *hostkey = libssh2_session_hostkey(sess, &hostkey_len, &hostkey_type_id);
    if (!hostkey || hostkey_len == 0) {
        return false;
    }

    const char *hostkey_type_name = "unknown";
    switch (hostkey_type_id) {
        case LIBSSH2_HOSTKEY_TYPE_RSA:
            hostkey_type_name = "ssh-rsa";
            break;
        case LIBSSH2_HOSTKEY_TYPE_DSS:
            hostkey_type_name = "ssh-dss";
            break;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:
            hostkey_type_name = "ecdsa-sha2-nistp256";
            break;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:
            hostkey_type_name = "ecdsa-sha2-nistp384";
            break;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:
            hostkey_type_name = "ecdsa-sha2-nistp521";
            break;
#ifdef LIBSSH2_HOSTKEY_TYPE_ED25519
        case LIBSSH2_HOSTKEY_TYPE_ED25519:
            hostkey_type_name = "ssh-ed25519";
            break;
#endif
        default:
            break;
    }

    if (hostkey_type && hostkey_type_len > 0) {
        snprintf(hostkey_type, hostkey_type_len, "%s", hostkey_type_name);
    }

    const unsigned char *sha1 = (const unsigned char *)libssh2_hostkey_hash(sess,
                                                                             LIBSSH2_HOSTKEY_HASH_SHA1);
    if (!sha1) {
        return false;
    }

    int p = 0;
    for (int i = 0; i < 20 && p < (int)fp_len - 3; i++) {
        p += snprintf(fp + p, fp_len - p, "%s%02x", (i == 0) ? "" : ":", sha1[i]);
    }
    fp[p] = '\0';
    return true;
}

// Produces an OpenSSH-style fingerprint string: "SHA256:<base64-no-padding>".
static bool get_server_hostkey_sha256(LIBSSH2_SESSION *sess,
                                      char *fp,
                                      size_t fp_len,
                                      char *hostkey_type,
                                      size_t hostkey_type_len) {
    if (!sess || !fp || fp_len == 0) return false;

    size_t hostkey_len = 0;
    int hostkey_type_id = 0;
    const char *hostkey = libssh2_session_hostkey(sess, &hostkey_len, &hostkey_type_id);
    if (!hostkey || hostkey_len == 0) {
        return false;
    }

    if (hostkey_type && hostkey_type_len > 0) {
        const char *name = "unknown";
        switch (hostkey_type_id) {
            case LIBSSH2_HOSTKEY_TYPE_RSA:        name = "ssh-rsa"; break;
            case LIBSSH2_HOSTKEY_TYPE_DSS:        name = "ssh-dss"; break;
            case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:  name = "ecdsa-sha2-nistp256"; break;
            case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:  name = "ecdsa-sha2-nistp384"; break;
            case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:  name = "ecdsa-sha2-nistp521"; break;
#ifdef LIBSSH2_HOSTKEY_TYPE_ED25519
            case LIBSSH2_HOSTKEY_TYPE_ED25519:    name = "ssh-ed25519"; break;
#endif
            default: break;
        }
        snprintf(hostkey_type, hostkey_type_len, "%s", name);
    }

    const unsigned char *sha256 = (const unsigned char *)libssh2_hostkey_hash(
        sess, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (!sha256) {
        return false;
    }

    // base64 of 32 bytes is 44 chars (with padding); strip trailing '='.
    unsigned char b64[64] = {0};
    size_t b64_len = 0;
    if (mbedtls_base64_encode(b64, sizeof(b64), &b64_len, sha256, 32) != 0) {
        return false;
    }
    while (b64_len > 0 && b64[b64_len - 1] == '=') {
        b64[--b64_len] = '\0';
    }

    int n = snprintf(fp, fp_len, "SHA256:%s", (const char *)b64);
    return n > 0 && n < (int)fp_len;
}

static void log_server_hostkey_fingerprint(LIBSSH2_SESSION *sess) {
    if (!sess) return;
    char sha1_fp[80] = {0};
    char sha256_fp[80] = {0};
    char hostkey_type[32] = {0};
    bool have_sha1 = get_server_hostkey_sha1(sess, sha1_fp, sizeof(sha1_fp),
                                             hostkey_type, sizeof(hostkey_type));
    bool have_sha256 = get_server_hostkey_sha256(sess, sha256_fp, sizeof(sha256_fp),
                                                 hostkey_type, sizeof(hostkey_type));
    if (!have_sha1 && !have_sha256) {
        ESP_LOGW(TAG, "Unable to derive server host key fingerprint");
        return;
    }
    ESP_LOGI(TAG, "Server host key: type=%s %s sha1=%s",
             hostkey_type,
             have_sha256 ? sha256_fp : "SHA256:?",
             have_sha1 ? sha1_fp : "?");
}

static uint32_t fnv1a_32(const char *s) {
    uint32_t h = 2166136261u;
    if (!s) return h;
    while (*s) {
        h ^= (uint8_t)(*s++);
        h *= 16777619u;
    }
    return h;
}

static bool make_trust_key(const char *host, uint16_t port, char *out, size_t out_len) {
    if (!host || !host[0] || !out || out_len == 0) return false;
    char host_port[96];
    int n = snprintf(host_port, sizeof(host_port), "%s:%u", host, (unsigned)port);
    if (n <= 0 || n >= (int)sizeof(host_port)) return false;

    uint32_t id = fnv1a_32(host_port);
    n = snprintf(out, out_len, "h%08x", (unsigned)id);
    return n > 0 && n < (int)out_len;
}

// Record format (current): "host:port|type|SHA256:base64"
// Legacy format (pre-Phase4): "host:port|aa:bb:..:.." (colon-hex SHA1, no type).
// Legacy records load with type_out="" so the caller forces a SHA256 upgrade.
static bool load_trust_record(const char *trust_key,
                              char *host_port_out,
                              size_t host_port_out_len,
                              char *type_out,
                              size_t type_out_len,
                              char *fingerprint_out,
                              size_t fingerprint_out_len) {
    if (!trust_key || !host_port_out || !fingerprint_out) return false;
    host_port_out[0] = '\0';
    fingerprint_out[0] = '\0';
    if (type_out && type_out_len) type_out[0] = '\0';

    nvs_handle_t nvs = 0;
    if (nvs_open(SSH_TRUST_NS, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }

    char record[200] = {0};
    size_t record_len = sizeof(record);
    esp_err_t err = nvs_get_str(nvs, trust_key, record, &record_len);
    nvs_close(nvs);
    if (err != ESP_OK) {
        return false;
    }

    const char *sep1 = strchr(record, '|');
    if (!sep1) {
        return false;
    }
    size_t host_len = (size_t)(sep1 - record);
    if (host_len == 0 || host_len >= host_port_out_len) {
        return false;
    }
    memcpy(host_port_out, record, host_len);
    host_port_out[host_len] = '\0';

    const char *sep2 = strchr(sep1 + 1, '|');
    if (sep2) {
        // Current 3-field format: type | fingerprint
        size_t type_len = (size_t)(sep2 - (sep1 + 1));
        size_t fp_len = strnlen(sep2 + 1, sizeof(record) - (size_t)((sep2 + 1) - record));
        if (fp_len == 0 || fp_len >= fingerprint_out_len) {
            return false;
        }
        if (type_out && type_out_len) {
            if (type_len >= type_out_len) type_len = type_out_len - 1;
            memcpy(type_out, sep1 + 1, type_len);
            type_out[type_len] = '\0';
        }
        memcpy(fingerprint_out, sep2 + 1, fp_len);
        fingerprint_out[fp_len] = '\0';
        return true;
    }

    // Legacy 2-field format: host:port | sha1-hex. type_out stays "".
    size_t fp_len = strnlen(sep1 + 1, sizeof(record) - (size_t)((sep1 + 1) - record));
    if (fp_len == 0 || fp_len >= fingerprint_out_len) {
        return false;
    }
    memcpy(fingerprint_out, sep1 + 1, fp_len);
    fingerprint_out[fp_len] = '\0';
    return true;
}

static bool save_trust_record(const char *trust_key,
                              const char *host_port,
                              const char *type,
                              const char *fingerprint) {
    if (!trust_key || !host_port || !fingerprint) return false;

    char record[200] = {0};
    int n = snprintf(record, sizeof(record), "%s|%s|%s",
                     host_port, (type && type[0]) ? type : "unknown", fingerprint);
    if (n <= 0 || n >= (int)sizeof(record)) {
        return false;
    }

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(SSH_TRUST_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return false;
    }
    err = nvs_set_str(nvs, trust_key, record);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err == ESP_OK;
}

static bool verify_or_store_host_fingerprint(const char *host, uint16_t port, LIBSSH2_SESSION *sess) {
    if (!host || !host[0] || !sess) return true;

    char fp[80] = {0};
    char hostkey_type[32] = {0};
    if (!get_server_hostkey_sha256(sess, fp, sizeof(fp), hostkey_type, sizeof(hostkey_type))) {
        ESP_LOGW(TAG, "Host trust check skipped: SHA256 fingerprint unavailable");
        return true;
    }

    char trust_key[16] = {0};
    char host_port[96] = {0};
    if (!make_trust_key(host, port, trust_key, sizeof(trust_key))) {
        ESP_LOGW(TAG, "Host trust key generation failed for %s:%u", host, (unsigned)port);
        return true;
    }
    snprintf(host_port, sizeof(host_port), "%s:%u", host, (unsigned)port);

    char stored_host_port[96] = {0};
    char stored_type[32] = {0};
    char stored_fp[80] = {0};
    if (!load_trust_record(trust_key,
                           stored_host_port,
                           sizeof(stored_host_port),
                           stored_type,
                           sizeof(stored_type),
                           stored_fp,
                           sizeof(stored_fp))) {
        if (!save_trust_record(trust_key, host_port, hostkey_type, fp)) {
            ESP_LOGW(TAG, "Failed to persist TOFU host fingerprint for %s", host_port);
            return true;
        }
        ESP_LOGI(TAG, "Host trust TOFU: %s type=%s %s", host_port, hostkey_type, fp);
        return true;
    }

    if (strcmp(stored_host_port, host_port) != 0) {
        ESP_LOGW(TAG,
                 "Host trust key collision (%s): stored=%s current=%s",
                 trust_key,
                 stored_host_port,
                 host_port);
        set_last_connect_error("host trust key collision for %s", host_port);
        return false;
    }

    // Legacy record (SHA1, no type): cannot compare against SHA256. Upgrade
    // in place by re-pinning the current key with a SHA256 record (TOFU on
    // the same connection). This only happens once per previously-seen host.
    if (stored_type[0] == '\0') {
        if (save_trust_record(trust_key, host_port, hostkey_type, fp)) {
            ESP_LOGI(TAG, "Host trust upgraded to SHA256: %s type=%s %s",
                     host_port, hostkey_type, fp);
        } else {
            ESP_LOGW(TAG, "Failed to upgrade legacy host trust record for %s", host_port);
        }
        return true;
    }

    if (strcmp(stored_fp, fp) != 0) {
        ESP_LOGE(TAG,
                 "Host key mismatch for %s: expected=%s got=%s",
                 host_port,
                 stored_fp,
                 fp);
        set_last_connect_error(
            "host key mismatch for %s (run: sshknown trust %s to accept)",
            host_port, host_port);
        return false;
    }

    ESP_LOGI(TAG, "Host trust verified: %s %s", host_port, fp);
    return true;
}

// ---- Phase 4: known-host trust management API ----

int ssh_known_hosts_foreach(ssh_known_host_cb cb, void *ctx) {
    if (!cb) return 0;
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, SSH_TRUST_NS, NVS_TYPE_STR, &it);
    int count = 0;
    while (err == ESP_OK && it != NULL) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);

        char host_port[96] = {0};
        char type[32] = {0};
        char fp[80] = {0};
        if (load_trust_record(info.key,
                              host_port, sizeof(host_port),
                              type, sizeof(type),
                              fp, sizeof(fp))) {
            cb(host_port, type[0] ? type : "legacy-sha1", fp, ctx);
            count++;
        }
        err = nvs_entry_next(&it);
    }
    if (it) nvs_release_iterator(it);
    return count;
}

bool ssh_known_host_remove(const char *host, uint16_t port) {
    if (!host || !host[0]) return false;
    char trust_key[16] = {0};
    if (!make_trust_key(host, port, trust_key, sizeof(trust_key))) {
        return false;
    }
    nvs_handle_t nvs = 0;
    if (nvs_open(SSH_TRUST_NS, NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_erase_key(nvs, trust_key);
    if (err == ESP_OK) {
        nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err == ESP_OK;
}

int ssh_known_hosts_clear(void) {
    nvs_handle_t nvs = 0;
    if (nvs_open(SSH_TRUST_NS, NVS_READWRITE, &nvs) != ESP_OK) {
        return 0;
    }
    // Count first (best-effort), then erase the whole namespace.
    int count = 0;
    nvs_iterator_t it = NULL;
    if (nvs_entry_find(NVS_DEFAULT_PART_NAME, SSH_TRUST_NS, NVS_TYPE_STR, &it) == ESP_OK) {
        while (it != NULL) {
            count++;
            if (nvs_entry_next(&it) != ESP_OK) break;
        }
        if (it) nvs_release_iterator(it);
    }
    esp_err_t err = nvs_erase_all(nvs);
    if (err == ESP_OK) {
        nvs_commit(nvs);
    }
    nvs_close(nvs);
    return (err == ESP_OK) ? count : 0;
}

static QueueHandle_t create_rx_queue_with_fallback(void) {
    size_t static_bytes = SSH_RX_QUEUE_SIZE * sizeof(ssh_rx_msg_t);
    ssh_rx_queue_static_storage = (uint8_t *)heap_caps_malloc(static_bytes,
                                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ssh_rx_queue_static_storage) {
        ssh_rx_queue_static_obj = (StaticQueue_t *)heap_caps_malloc(sizeof(StaticQueue_t),
                                                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (ssh_rx_queue_static_obj) {
            QueueHandle_t static_q = xQueueCreateStatic(SSH_RX_QUEUE_SIZE,
                                                        sizeof(ssh_rx_msg_t),
                                                        ssh_rx_queue_static_storage,
                                                        ssh_rx_queue_static_obj);
            if (static_q) {
                ESP_LOGI(TAG,
                         "RX queue created (static+SPIRAM): depth=%u item=%u (~%u bytes payload)",
                         (unsigned)SSH_RX_QUEUE_SIZE,
                         (unsigned)sizeof(ssh_rx_msg_t),
                         (unsigned)static_bytes);
                return static_q;
            }
            free(ssh_rx_queue_static_obj);
            ssh_rx_queue_static_obj = NULL;
        }
        free(ssh_rx_queue_static_storage);
        ssh_rx_queue_static_storage = NULL;
    }

    ESP_LOGW(TAG,
             "Static RX queue unavailable; trying dynamic fallback (free_int=%u largest_int=%u free_spiram=%u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    static const uint16_t depth_candidates[] = {48, 32, 24, 16};
    for (size_t i = 0; i < sizeof(depth_candidates) / sizeof(depth_candidates[0]); i++) {
        uint16_t depth = depth_candidates[i];
        QueueHandle_t q = xQueueCreate(depth, sizeof(ssh_rx_msg_t));
        if (q) {
            ESP_LOGI(TAG,
                     "RX queue created: depth=%u item=%u (~%u bytes payload)",
                     (unsigned)depth,
                     (unsigned)sizeof(ssh_rx_msg_t),
                     (unsigned)(depth * sizeof(ssh_rx_msg_t)));
            return q;
        }
    }
    return NULL;
}

static size_t filter_and_reply_fast_queries(const char *src, size_t len, char *dst) {
    size_t o = 0;
    for (size_t i = 0; i < len; ) {
        if (i + 4 <= len &&
            (uint8_t)src[i] == 0x1B && src[i + 1] == '[' && src[i + 2] == '5' && src[i + 3] == 'n') {
            if (ssh_connected && channel) {
                int wr = ssh_write("\033[0n", 4);
                if (wr == 4) {
                    ESP_LOGI(TAG, "Fast DSR reply: CSI 5n -> 0n");
                    i += 4;
                    continue;
                }
                ESP_LOGW(TAG, "Fast DSR reply failed for CSI 5n (wr=%d), falling back", wr);
            }
        }
        if (i + 5 <= len &&
            (uint8_t)src[i] == 0x1B && src[i + 1] == '[' && src[i + 2] == '?' && src[i + 3] == '5' && src[i + 4] == 'n') {
            if (ssh_connected && channel) {
                int wr = ssh_write("\033[?0n", 5);
                if (wr == 5) {
                    ESP_LOGI(TAG, "Fast DSR reply: CSI ?5n -> ?0n");
                    i += 5;
                    continue;
                }
                ESP_LOGW(TAG, "Fast DSR reply failed for CSI ?5n (wr=%d), falling back", wr);
            }
        }
        if (o < SSH_RX_BUF_SIZE - 1) dst[o++] = src[i];
        i++;
    }
    dst[o] = '\0';
    return o;
}

static void log_rx_trace(const char *data, size_t len) {
    if (!data || len == 0 || s_rx_trace_budget <= 0) {
        if (len) s_rx_prev_tail = (uint8_t)data[len - 1];
        return;
    }

    bool interesting = false;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = (uint8_t)data[i];
        if (b == 0x1B || (b >= 0x9B && b <= 0x9F)) {
            interesting = true;
            break;
        }
    }

    if (!interesting) {
        for (size_t i = 0; i + 1 < len; i++) {
            if (data[i] == '[' && data[i + 1] == '?') {
                interesting = true;
                break;
            }
        }
    }

    if (!interesting && data[0] == '[' && s_rx_prev_tail != 0x1B && s_rx_prev_tail != 0x9B) {
        interesting = true;
    }

    if (!interesting) {
        s_rx_prev_tail = (uint8_t)data[len - 1];
        return;
    }

    char hex[160];
    char txt[80];
    int hp = 0;
    int tp = 0;
    size_t sample = len > 32 ? 32 : len;
    for (size_t i = 0; i < sample && hp < (int)sizeof(hex) - 4; i++) {
        hp += snprintf(hex + hp, sizeof(hex) - hp, "%02X ", (uint8_t)data[i]);
    }
    for (size_t i = 0; i < sample && tp < (int)sizeof(txt) - 2; i++) {
        uint8_t b = (uint8_t)data[i];
        txt[tp++] = (b >= 0x20 && b <= 0x7E) ? (char)b : '.';
    }
    hex[hp] = '\0';
    txt[tp] = '\0';

    ESP_LOGI(TAG,
             "RX trace %uB prev=%02X: %s| %s",
             (unsigned)len,
             (unsigned)s_rx_prev_tail,
             hex,
             txt);
    s_rx_prev_tail = (uint8_t)data[len - 1];
    s_rx_trace_budget--;
}

static void delete_rx_queue(void) {
    if (ssh_rx_queue) {
        vQueueDelete(ssh_rx_queue);
        ssh_rx_queue = NULL;
    }
    if (ssh_rx_queue_static_obj) {
        free(ssh_rx_queue_static_obj);
        ssh_rx_queue_static_obj = NULL;
    }
    if (ssh_rx_queue_static_storage) {
        free(ssh_rx_queue_static_storage);
        ssh_rx_queue_static_storage = NULL;
    }
}

static int waitsocket(int sock, LIBSSH2_SESSION *sess);

static void log_channel_timeout_context(const char *op, LIBSSH2_CHANNEL *ch) {
    int dir = session ? libssh2_session_block_directions(session) : 0;
    int eof = ch ? libssh2_channel_eof(ch) : -1;
    int exit_status = ch ? libssh2_channel_get_exit_status(ch) : -1;

    ESP_LOGW(TAG,
             "%s timeout context: block_dir=%d eof=%d exit_status=%d",
             op ? op : "channel",
             dir,
             eof,
             exit_status);

    if (session) {
        char *errmsg = NULL;
        int errmsg_len = 0;
        int errcode = libssh2_session_last_error(session, &errmsg, &errmsg_len, 0);
        if (errmsg && errmsg_len > 0) {
            ESP_LOGW(TAG, "%s timeout: libssh2 err=%d msg=%.*s", op ? op : "channel", errcode, errmsg_len, errmsg);
        } else {
            ESP_LOGW(TAG, "%s timeout: libssh2 err=%d", op ? op : "channel", errcode);
        }
    }
}

static const char *safe_method_name(int method_type) {
    if (!session) return "n/a";
    const char *m = libssh2_session_methods(session, method_type);
    return (m && m[0]) ? m : "n/a";
}

static void log_ssh_runtime_error_context(const char *where, int rc) {
    int sock_errno = errno;
    int last_errno = session ? libssh2_session_last_errno(session) : 0;

    ESP_LOGW(TAG,
             "%s: rc=%d (%s) sock_errno=%d (%s) libssh2_last_errno=%d (%s)",
             where,
             rc,
             ssh_transport_rc_str(rc),
             sock_errno,
             strerror(sock_errno),
             last_errno,
             ssh_transport_rc_str(last_errno));

    if (session) {
        char *errmsg = NULL;
        int errmsg_len = 0;
        int errcode = libssh2_session_last_error(session, &errmsg, &errmsg_len, 0);
        if (errmsg && errmsg_len > 0) {
            ESP_LOGW(TAG, "%s: libssh2 err=%d msg=%.*s", where, errcode, errmsg_len, errmsg);
        }

        ESP_LOGW(TAG,
                 "%s: negotiated kex=%s hostkey=%s c2s=%s s2c=%s mac_c2s=%s mac_s2c=%s",
                 where,
                 safe_method_name(LIBSSH2_METHOD_KEX),
                 safe_method_name(LIBSSH2_METHOD_HOSTKEY),
                 safe_method_name(LIBSSH2_METHOD_CRYPT_CS),
                 safe_method_name(LIBSSH2_METHOD_CRYPT_SC),
                 safe_method_name(LIBSSH2_METHOD_MAC_CS),
                 safe_method_name(LIBSSH2_METHOD_MAC_SC));
    }
}

static void log_session_error(const char *stage) {
    if (!session) return;
    char *errmsg = NULL;
    int errmsg_len = 0;
    int errcode = libssh2_session_last_error(session, &errmsg, &errmsg_len, 0);
    if (errmsg && errmsg_len > 0) {
        ESP_LOGE(TAG, "%s failed: libssh2 err=%d msg=%.*s", stage, errcode, errmsg_len, errmsg);
    } else {
        ESP_LOGE(TAG, "%s failed: libssh2 err=%d", stage, errcode);
    }
}

static int ssh_handshake_with_timeout(int timeout_ms) {
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        int rc = libssh2_session_handshake(session, ssh_sock);
        if (rc == 0) return 0;
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            waitsocket(ssh_sock, session);
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        ESP_LOGE(TAG, "session_handshake rc=%d last_errno=%d", rc, libssh2_session_last_errno(session));
        return rc;
    }
    return LIBSSH2_ERROR_TIMEOUT;
}

static int ssh_userauth_with_timeout(const char *user, const char *pass, int timeout_ms) {
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        int rc = libssh2_userauth_password(session, user, pass);
        if (rc == 0) return 0;
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            waitsocket(ssh_sock, session);
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        return rc;
    }
    return LIBSSH2_ERROR_TIMEOUT;
}

static int ssh_userauth_publickey_with_timeout(const char *user,
                                               const char *private_key_pem,
                                               const char *passphrase,
                                               int timeout_ms) {
    if (!user || !user[0] || !private_key_pem || !private_key_pem[0]) {
        return LIBSSH2_ERROR_AUTHENTICATION_FAILED;
    }

    size_t user_len = strlen(user);
    size_t key_len = strlen(private_key_pem);
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        int rc = libssh2_userauth_publickey_frommemory(session,
                                                       user,
                                                       user_len,
                                                       NULL,
                                                       0,
                                                       private_key_pem,
                                                       key_len,
                                                       (passphrase && passphrase[0]) ? passphrase : NULL);
        if (rc == 0) return 0;
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            waitsocket(ssh_sock, session);
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        return rc;
    }
    return LIBSSH2_ERROR_TIMEOUT;
}

LIBSSH2_USERAUTH_KBDINT_RESPONSE_FUNC(ssh_kbdint_cb) {
    (void)name;
    (void)name_len;
    (void)instruction;
    (void)instruction_len;
    (void)abstract;

    for (int i = 0; i < num_prompts; i++) {
        responses[i].text = NULL;
        responses[i].length = 0;
        if (!s_kbdint_password) continue;

        const char *prompt = prompts[i].text ? (const char *)prompts[i].text : "";
        bool is_password_prompt = strstr(prompt, "Password") || strstr(prompt, "password");
        if (!is_password_prompt && prompts[i].echo != 0) continue;

        size_t n = strlen(s_kbdint_password);
        char *buf = (char *)malloc(n + 1);
        if (!buf) continue;
        memcpy(buf, s_kbdint_password, n + 1);
        responses[i].text = buf;
        responses[i].length = (unsigned int)n;
    }
}

static int ssh_userauth_kbdint_with_timeout(const char *user, const char *pass, int timeout_ms) {
    s_kbdint_password = pass;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        int rc = libssh2_userauth_keyboard_interactive(session, user, &ssh_kbdint_cb);
        if (rc == 0) {
            s_kbdint_password = NULL;
            return 0;
        }
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            waitsocket(ssh_sock, session);
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        s_kbdint_password = NULL;
        return rc;
    }
    s_kbdint_password = NULL;
    return LIBSSH2_ERROR_TIMEOUT;
}

static bool ssh_get_auth_methods_with_timeout(const char *user,
                                              char *out,
                                              size_t out_len,
                                              bool *none_succeeded,
                                              int timeout_ms) {
    if (!out || out_len == 0 || !none_succeeded) return false;
    out[0] = '\0';
    *none_succeeded = false;

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        char *methods = libssh2_userauth_list(session, user, (unsigned int)strlen(user));
        if (methods) {
            snprintf(out, out_len, "%s", methods);
            return true;
        }

        if (libssh2_userauth_authenticated(session)) {
            *none_succeeded = true;
            return true;
        }

        int err = libssh2_session_last_errno(session);
        if (err == LIBSSH2_ERROR_EAGAIN) {
            waitsocket(ssh_sock, session);
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        return false;
    }

    return false;
}

static LIBSSH2_CHANNEL *ssh_channel_open_with_timeout(int timeout_ms) {
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        LIBSSH2_CHANNEL *ch = libssh2_channel_open_session(session);
        if (ch) return ch;
        int err = libssh2_session_last_errno(session);
        if (err == LIBSSH2_ERROR_EAGAIN) {
            waitsocket(ssh_sock, session);
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        return NULL;
    }
    return NULL;
}

/* Request PTY with terminal dimensions included in the pty-req itself.
 *
 * Important compatibility note: do NOT send a separate window-change
 * ("pty size") request between pty-req and shell. Go-based SSH servers
 * (e.g. terminal.shop, charm/wish stacks) can stop servicing channel
 * requests in that ordering, so the shell request never gets a reply
 * and the session stays silent. Passing dimensions inside pty-req
 * avoids the extra request entirely.
 */
static int ssh_channel_request_pty_with_timeout(LIBSSH2_CHANNEL *ch, const char *termtype, int timeout_ms) {
    int cols = ssh_term ? ssh_term->cols : 80;
    int rows = ssh_term ? ssh_term->rows : 24;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        int rc = libssh2_channel_request_pty_ex(ch,
                                                termtype,
                                                (unsigned int)strlen(termtype),
                                                NULL,
                                                0,
                                                cols,
                                                rows,
                                                0,
                                                0);
        if (rc == 0) return 0;
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            waitsocket(ssh_sock, session);
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        return rc;
    }
    log_channel_timeout_context("channel_request_pty", ch);
    return LIBSSH2_ERROR_TIMEOUT;
}

/* Best-effort channel env var. Many servers restrict accepted vars via
 * sshd AcceptEnv; a refusal returns a non-EAGAIN error and is non-fatal. */
static int ssh_channel_setenv_with_timeout(LIBSSH2_CHANNEL *ch,
                                           const char *name,
                                           const char *value,
                                           int timeout_ms) {
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        int rc = libssh2_channel_setenv_ex(ch,
                                           name, (unsigned int)strlen(name),
                                           value, (unsigned int)strlen(value));
        if (rc == 0) return 0;
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            waitsocket(ssh_sock, session);
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        return rc;
    }
    return LIBSSH2_ERROR_TIMEOUT;
}

static int ssh_channel_shell_with_timeout(LIBSSH2_CHANNEL *ch, int timeout_ms) {
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        int rc = libssh2_channel_shell(ch);
        if (rc == 0) return 0;
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            waitsocket(ssh_sock, session);
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        return rc;
    }
    int avail_out = ch ? libssh2_poll_channel_read(ch, 0) : 0;
    int avail_err = ch ? libssh2_poll_channel_read(ch, 1) : 0;
    if (avail_out > 0 || avail_err > 0) {
        ESP_LOGW(TAG,
                 "channel_shell timed out waiting for reply but data is available (stdout=%d stderr=%d); treating as success",
                 avail_out,
                 avail_err);
        return 0;
    }

    log_channel_timeout_context("channel_shell", ch);
    return LIBSSH2_ERROR_TIMEOUT;
}

static int ssh_channel_shell_no_reply_with_timeout(LIBSSH2_CHANNEL *ch, int timeout_ms) {
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        int rc = libssh2_channel_process_startup_no_reply(ch, "shell", 5, NULL, 0);
        if (rc == 0) return 0;
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            waitsocket(ssh_sock, session);
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        return rc;
    }
    log_channel_timeout_context("channel_shell_no_reply", ch);
    return LIBSSH2_ERROR_TIMEOUT;
}

static int ssh_channel_exec_no_reply_with_timeout(LIBSSH2_CHANNEL *ch,
                                                  const char *cmd,
                                                  int timeout_ms) {
    if (!cmd) return LIBSSH2_ERROR_INVAL;

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        int rc = libssh2_channel_process_startup_no_reply(ch,
                                                          "exec",
                                                          4,
                                                          cmd,
                                                          (unsigned int)strlen(cmd));
        if (rc == 0) return 0;
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            waitsocket(ssh_sock, session);
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        return rc;
    }
    log_channel_timeout_context(cmd, ch);
    return LIBSSH2_ERROR_TIMEOUT;
}

static int ssh_channel_exec_with_timeout(LIBSSH2_CHANNEL *ch,
                                         const char *cmd,
                                         int timeout_ms) {
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        int rc = libssh2_channel_exec(ch, cmd);
        if (rc == 0) return 0;
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            waitsocket(ssh_sock, session);
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        return rc;
    }
    log_channel_timeout_context(cmd ? cmd : "channel_exec", ch);
    return LIBSSH2_ERROR_TIMEOUT;
}

static bool ssh_channel_stays_open_briefly(LIBSSH2_CHANNEL *ch, int observe_ms) {
    if (!ch) return false;

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(observe_ms);
    char scratch[64];
    while (xTaskGetTickCount() < deadline) {
        if (libssh2_channel_eof(ch)) {
            return false;
        }

        int rc = libssh2_channel_read(ch, scratch, sizeof(scratch));
        if (rc > 0) {
            return true;
        }
        if (rc == LIBSSH2_ERROR_EAGAIN || rc == 0) {
            waitsocket(ssh_sock, session);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        return false;
    }

    return !libssh2_channel_eof(ch);
}

static bool ssh_open_pty_shell_fallback(const char *termtype,
                                        int pty_timeout_ms,
                                        int shell_timeout_ms,
                                        int observe_ms) {
    channel = ssh_channel_open_with_timeout(15000);
    if (!channel) {
        ESP_LOGW(TAG, "Channel reopen failed for PTY retry (term=%s)", termtype);
        return false;
    }

    libssh2_channel_set_blocking(channel, 0);
    int pty_rc = ssh_channel_request_pty_with_timeout(channel, termtype, pty_timeout_ms);
    if (pty_rc != 0) {
        ESP_LOGW(TAG, "PTY retry failed (term=%s): %d", termtype, pty_rc);
        libssh2_channel_close(channel);
        libssh2_channel_free(channel);
        channel = NULL;
        return false;
    }

    /* Terminal dimensions are sent inside pty-req; no separate
     * window-change request (Go SSH server compatibility). */

    int sh_rc = ssh_channel_shell_with_timeout(channel, shell_timeout_ms);
    if (sh_rc == 0 && ssh_channel_stays_open_briefly(channel, observe_ms)) {
        ESP_LOGI(TAG, "Shell retry succeeded with PTY term=%s", termtype);
        return true;
    }

    if (sh_rc == 0) {
        ESP_LOGW(TAG, "Shell retry opened then closed with PTY term=%s", termtype);
    } else {
        ESP_LOGW(TAG, "Shell retry failed with PTY term=%s rc=%d", termtype, sh_rc);
    }

    libssh2_channel_close(channel);
    libssh2_channel_free(channel);
    channel = NULL;
    return false;
}

static bool ssh_channel_wait_for_output(LIBSSH2_CHANNEL *ch,
                                        int observe_ms,
                                        bool send_probe) {
    if (!ch) return false;

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(observe_ms);
    bool probe_sent = false;
    char scratch[160];
    while (xTaskGetTickCount() < deadline) {
        int rd_out = libssh2_channel_read_ex(ch, 0, scratch, sizeof(scratch));
        if (rd_out > 0) return true;
        if (rd_out < 0 && rd_out != LIBSSH2_ERROR_EAGAIN) return false;

        int rd_err = libssh2_channel_read_ex(ch, 1, scratch, sizeof(scratch));
        if (rd_err > 0) return true;
        if (rd_err < 0 && rd_err != LIBSSH2_ERROR_EAGAIN) return false;

        int avail_out = libssh2_poll_channel_read(ch, 0);
        int avail_err = libssh2_poll_channel_read(ch, 1);
        if (avail_out > 0 || avail_err > 0) {
            return true;
        }

        if (libssh2_channel_eof(ch)) {
            return false;
        }

        if (send_probe && !probe_sent) {
            int wr = libssh2_channel_write(ch, "\r\n", 2);
            if (wr == 2) {
                probe_sent = true;
            } else if (wr < 0 && wr != LIBSSH2_ERROR_EAGAIN) {
                return false;
            }
        }

        waitsocket(ssh_sock, session);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return false;
}

static bool ssh_channel_bootstrap_expect_output(LIBSSH2_CHANNEL *ch,
                                                int per_probe_wait_ms,
                                                int tail_wait_ms) {
    if (!ch) return false;

    static const char *probe_seq[] = {
        "\r",
        "\n",
        "\r\n",
        "echo __DUMBESPTY_SHELL_READY__\r",
        "printf __DUMBESPTY_SHELL_READY__\\n\r"
    };

    for (size_t i = 0; i < sizeof(probe_seq) / sizeof(probe_seq[0]); i++) {
        const char *probe = probe_seq[i];
        int probe_len = (int)strlen(probe);
        int wr = libssh2_channel_write(ch, probe, probe_len);
        if (wr < 0 && wr != LIBSSH2_ERROR_EAGAIN) {
            return false;
        }

        if (ssh_channel_wait_for_output(ch, per_probe_wait_ms, false)) {
            return true;
        }

        if (libssh2_channel_eof(ch)) {
            return false;
        }
    }

    return ssh_channel_wait_for_output(ch, tail_wait_ms, false);
}

static bool ssh_open_pty_shell_no_reply_fallback(const char *termtype,
                                                  int pty_timeout_ms,
                                                  int startup_timeout_ms,
                                                  int observe_ms) {
    channel = ssh_channel_open_with_timeout(15000);
    if (!channel) {
        ESP_LOGW(TAG, "Channel reopen failed for PTY no-reply shell (term=%s)", termtype);
        return false;
    }

    libssh2_channel_set_blocking(channel, 0);
    int pty_rc = ssh_channel_request_pty_with_timeout(channel, termtype, pty_timeout_ms);
    if (pty_rc != 0) {
        ESP_LOGW(TAG, "PTY no-reply shell failed PTY request (term=%s): %d", termtype, pty_rc);
        libssh2_channel_close(channel);
        libssh2_channel_free(channel);
        channel = NULL;
        return false;
    }

    /* Terminal dimensions are sent inside pty-req; no separate
     * window-change request (Go SSH server compatibility). */

    int sh_rc = ssh_channel_shell_no_reply_with_timeout(channel, startup_timeout_ms);
    if (sh_rc == 0 && ssh_channel_stays_open_briefly(channel, observe_ms)) {
        bool had_output = ssh_channel_wait_for_output(channel, observe_ms, true);
        if (had_output) {
            ESP_LOGI(TAG, "Shell no-reply fallback succeeded with PTY term=%s (output seen)", termtype);
            return true;
        } else {
            ESP_LOGW(TAG, "Shell no-reply fallback stayed silent with PTY term=%s", termtype);
        }
    }

    if (sh_rc == 0) {
        ESP_LOGW(TAG, "Shell no-reply fallback opened then closed (term=%s)", termtype);
    } else {
        ESP_LOGW(TAG, "Shell no-reply fallback failed (term=%s rc=%d)", termtype, sh_rc);
    }

    libssh2_channel_close(channel);
    libssh2_channel_free(channel);
    channel = NULL;
    return false;
}

static bool ssh_open_pty_exec_no_reply_fallback(const char *termtype,
                                                const char *cmd,
                                                int pty_timeout_ms,
                                                int startup_timeout_ms,
                                                int observe_ms) {
    channel = ssh_channel_open_with_timeout(15000);
    if (!channel) {
        ESP_LOGW(TAG, "Channel reopen failed for PTY no-reply exec (term=%s)", termtype);
        return false;
    }

    libssh2_channel_set_blocking(channel, 0);
    int pty_rc = ssh_channel_request_pty_with_timeout(channel, termtype, pty_timeout_ms);
    if (pty_rc != 0) {
        ESP_LOGW(TAG, "PTY no-reply exec failed PTY request (term=%s): %d", termtype, pty_rc);
        libssh2_channel_close(channel);
        libssh2_channel_free(channel);
        channel = NULL;
        return false;
    }

    /* Terminal dimensions are sent inside pty-req; no separate
     * window-change request (Go SSH server compatibility). */

    int ex_rc = ssh_channel_exec_no_reply_with_timeout(channel, cmd, startup_timeout_ms);
    if (ex_rc == 0 && ssh_channel_stays_open_briefly(channel, observe_ms)) {
        bool had_output = ssh_channel_wait_for_output(channel, observe_ms, true);
        if (had_output) {
            ESP_LOGI(TAG, "PTY no-reply exec fallback succeeded: term=%s cmd=%s", termtype, cmd);
            return true;
        }
        ESP_LOGW(TAG, "PTY no-reply exec fallback stayed silent: term=%s cmd=%s", termtype, cmd);
    } else if (ex_rc == 0) {
        ESP_LOGW(TAG, "PTY no-reply exec fallback opened then closed: term=%s cmd=%s", termtype, cmd);
    } else {
        ESP_LOGW(TAG, "PTY no-reply exec fallback failed: term=%s cmd=%s rc=%d", termtype, cmd, ex_rc);
    }

    libssh2_channel_close(channel);
    libssh2_channel_free(channel);
    channel = NULL;
    return false;
}

static bool ssh_open_pty_exec_fallback(const char *termtype,
                                       const char *cmd,
                                       int pty_timeout_ms,
                                       int exec_timeout_ms,
                                       int observe_ms) {
    channel = ssh_channel_open_with_timeout(15000);
    if (!channel) {
        ESP_LOGW(TAG, "Channel reopen failed for PTY exec fallback (term=%s)", termtype);
        return false;
    }

    libssh2_channel_set_blocking(channel, 0);
    int pty_rc = ssh_channel_request_pty_with_timeout(channel, termtype, pty_timeout_ms);
    if (pty_rc != 0) {
        ESP_LOGW(TAG, "PTY exec fallback failed PTY request (term=%s): %d", termtype, pty_rc);
        libssh2_channel_close(channel);
        libssh2_channel_free(channel);
        channel = NULL;
        return false;
    }

    /* Terminal dimensions are sent inside pty-req; no separate
     * window-change request (Go SSH server compatibility). */

    int ex_rc = ssh_channel_exec_with_timeout(channel, cmd, exec_timeout_ms);
    if (ex_rc == 0 && ssh_channel_stays_open_briefly(channel, observe_ms)) {
        ESP_LOGI(TAG, "PTY exec fallback succeeded: term=%s cmd=%s", termtype, cmd);
        return true;
    }

    if (ex_rc == 0) {
        ESP_LOGW(TAG, "PTY exec fallback opened then closed: term=%s cmd=%s", termtype, cmd);
    } else {
        ESP_LOGW(TAG, "PTY exec fallback failed: term=%s cmd=%s rc=%d", termtype, cmd, ex_rc);
    }

    libssh2_channel_close(channel);
    libssh2_channel_free(channel);
    channel = NULL;
    return false;
}

static bool ssh_open_raw_channel_fallback(int probe_ms) {
    channel = ssh_channel_open_with_timeout(15000);
    if (!channel) {
        ESP_LOGW(TAG, "Channel reopen failed for raw fallback");
        return false;
    }

    libssh2_channel_set_blocking(channel, 0);

    if (ssh_channel_wait_for_output(channel, probe_ms, true)) {
        ESP_LOGI(TAG, "Raw channel fallback has data");
        return true;
    }

    if (!libssh2_channel_eof(channel)) {
        ESP_LOGW(TAG, "Raw channel fallback produced no output during probe");
    } else {
        ESP_LOGW(TAG, "Raw channel fallback hit EOF during probe");
    }

    libssh2_channel_close(channel);
    libssh2_channel_free(channel);
    channel = NULL;
    return false;
}

static bool ssh_open_shell_no_pty_fallback(int shell_timeout_ms, int observe_ms) {
    channel = ssh_channel_open_with_timeout(15000);
    if (!channel) {
        ESP_LOGW(TAG, "Channel reopen failed for no-PTY shell fallback");
        return false;
    }

    libssh2_channel_set_blocking(channel, 0);
    int sh_rc = ssh_channel_shell_with_timeout(channel, shell_timeout_ms);
    if (sh_rc == 0 && ssh_channel_stays_open_briefly(channel, observe_ms)) {
        ESP_LOGI(TAG, "Shell fallback succeeded without PTY");
        return true;
    }

    if (sh_rc == 0) {
        ESP_LOGW(TAG, "No-PTY shell opened then closed");
    } else {
        ESP_LOGW(TAG, "No-PTY shell fallback failed rc=%d", sh_rc);
    }

    libssh2_channel_close(channel);
    libssh2_channel_free(channel);
    channel = NULL;
    return false;
}

static bool ssh_probe_banner_with_timeout(char *dst, size_t dst_len, int timeout_ms) {
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        int pr = recv(ssh_sock, dst, dst_len - 1, MSG_PEEK);
        if (pr > 0) {
            dst[pr] = '\0';
            return true;
        }
        if (pr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT)) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(ssh_sock, &rfds);
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 200000;
            select(ssh_sock + 1, &rfds, NULL, NULL, &tv);
            continue;
        }
        return false;
    }
    errno = ETIMEDOUT;
    return false;
}

static bool set_socket_blocking_mode(int fd, bool blocking) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    if (blocking) {
        flags &= ~O_NONBLOCK;
    } else {
        flags |= O_NONBLOCK;
    }
    return fcntl(fd, F_SETFL, flags) == 0;
}

static bool is_tailnet_ipv4(uint32_t ip_host_order) {
    return (ip_host_order & 0xFFC00000u) == 0x64400000u;  // 100.64.0.0/10
}

static bool tcp_connect_with_retry(const struct sockaddr_in *saddr, uint32_t bind_ip_host_order) {
    for (int attempt = 1; attempt <= SSH_CONNECT_RETRIES; attempt++) {
        ssh_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (ssh_sock < 0) {
            ESP_LOGE(TAG, "socket() failed on attempt %d", attempt);
            continue;
        }

        int flags = fcntl(ssh_sock, F_GETFL, 0);
        if (flags < 0 || fcntl(ssh_sock, F_SETFL, flags | O_NONBLOCK) < 0) {
            ESP_LOGE(TAG, "fcntl(O_NONBLOCK) failed on attempt %d: errno=%d %s", attempt, errno, strerror(errno));
            close(ssh_sock);
            ssh_sock = -1;
            continue;
        }

        if (bind_ip_host_order != 0) {
            struct sockaddr_in src;
            memset(&src, 0, sizeof(src));
            src.sin_family = AF_INET;
            src.sin_port = 0;
            src.sin_addr.s_addr = htonl(bind_ip_host_order);
            if (bind(ssh_sock, (const struct sockaddr *)&src, sizeof(src)) != 0) {
                ESP_LOGW(TAG,
                         "bind() to VPN IP failed on attempt %d: errno=%d %s",
                         attempt,
                         errno,
                         strerror(errno));
            }
        }

        TickType_t t0 = xTaskGetTickCount();
        int rc = ::connect(ssh_sock, (const struct sockaddr *)saddr, sizeof(*saddr));
        if (rc == 0) {
            TickType_t t1 = xTaskGetTickCount();
            ESP_LOGI(TAG, "connect() immediate success in %d ms (attempt %d)", (t1 - t0) * portTICK_PERIOD_MS, attempt);
            return true;
        }

        if (errno == EINPROGRESS || errno == EALREADY) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(ssh_sock, &wfds);
            struct timeval tv;
            tv.tv_sec = SSH_CONNECT_TIMEOUT_MS / 1000;
            tv.tv_usec = (SSH_CONNECT_TIMEOUT_MS % 1000) * 1000;

            int sel = select(ssh_sock + 1, NULL, &wfds, NULL, &tv);
            TickType_t t1 = xTaskGetTickCount();

            if (sel > 0 && FD_ISSET(ssh_sock, &wfds)) {
                int so_error = 0;
                socklen_t optlen = sizeof(so_error);
                getsockopt(ssh_sock, SOL_SOCKET, SO_ERROR, &so_error, &optlen);
                ESP_LOGI(TAG, "connect() select done in %d ms (attempt %d), SO_ERROR=%d",
                         (t1 - t0) * portTICK_PERIOD_MS, attempt, so_error);
                if (so_error == 0) return true;
                errno = so_error;
            } else if (sel == 0) {
                ESP_LOGW(TAG, "connect() timeout after %d ms (attempt %d)", (t1 - t0) * portTICK_PERIOD_MS, attempt);
                errno = ETIMEDOUT;
            } else {
                ESP_LOGW(TAG, "connect() select error (attempt %d): errno=%d %s", attempt, errno, strerror(errno));
            }
        }

        ESP_LOGW(TAG, "TCP connect attempt %d failed: errno=%d %s", attempt, errno, strerror(errno));
        close(ssh_sock);
        ssh_sock = -1;
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    return false;
}

static int waitsocket(int sock, LIBSSH2_SESSION *sess) {
    struct fd_set rfds;
    struct fd_set wfds;
    struct timeval timeout;
    int dir = sess ? libssh2_session_block_directions(sess) : 0;

    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    if (dir == 0 || (dir & LIBSSH2_SESSION_BLOCK_INBOUND)) {
        FD_SET(sock, &rfds);
    }
    if (dir == 0 || (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND)) {
        FD_SET(sock, &wfds);
    }
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    return select(sock + 1, &rfds, &wfds, NULL, &timeout);
}

#if SSH_USE_RX_TASK
static void ssh_recv_task(void *param) {
    ESP_LOGD(TAG, "SSH recv task started");

    while (ssh_connected && channel) {
        bool got_stdout = ssh_pump_channel_stream_once(0);
        bool got_stderr = ssh_pump_channel_stream_once(1);

        if (got_stdout || got_stderr) {
            /* keep pumping */
        } else {
            waitsocket(ssh_sock, session);
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        if (libssh2_channel_eof(channel)) break;
    }

    ESP_LOGI(TAG, "SSH recv task ended");
    ssh_connected = false;
    ssh_recv_task_handle = NULL;
    vTaskDelete(NULL);
}
#endif

bool ssh_connect(const char *host, uint16_t port, const char *user, const char *pass) {
    struct connect_flag_guard_t {
        bool *flag;
        explicit connect_flag_guard_t(bool *f) : flag(f) {}
        ~connect_flag_guard_t() {
            if (flag) *flag = false;
        }
    };

    s_ssh_connecting = true;
    connect_flag_guard_t connect_guard(&s_ssh_connecting);

    s_last_connect_requires_password = false;
    set_last_connect_error("");

    if (ssh_connected) {
        ESP_LOGW(TAG, "Already connected");
        set_last_connect_error("already connected");
        return false;
    }

    coex_acquire();

    ESP_LOGI(TAG, "Coex acquired, settling radio for 1s...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "Radio settled");

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(port);

    if (!wifi_mgr_is_connected()) {
        ESP_LOGE(TAG, "WiFi not connected");
        set_last_connect_error("wifi not connected");
        coex_release();
        return false;
    }

    ESP_LOGI(TAG, "Resolving %s...", host);
    struct hostent *he = gethostbyname(host);
    if (!he) {
        ESP_LOGE(TAG, "DNS lookup failed for %s", host);
        set_last_connect_error("dns lookup failed for '%s'", host);
        close(ssh_sock); ssh_sock = -1;
        delete_rx_queue();
        coex_release();
        return false;
    }
    memcpy(&saddr.sin_addr, he->h_addr, he->h_length);
    ESP_LOGI(TAG, "Resolved %s to %s", host, inet_ntoa(saddr.sin_addr));
    uint32_t bind_ip = 0;
    uint32_t dest_ip = ntohl(saddr.sin_addr.s_addr);
    if (tailscale_mgr_is_connected() && is_tailnet_ipv4(dest_ip)) {
        bind_ip = tailscale_mgr_get_vpn_ip();
        if (bind_ip != 0) {
            struct in_addr bind_addr = {.s_addr = htonl(bind_ip)};
            ESP_LOGI(TAG, "Tailnet destination detected; binding source to VPN IP %s", inet_ntoa(bind_addr));
        } else {
            ESP_LOGW(TAG, "Tailnet destination detected but VPN IP unavailable");
        }
    }

    ESP_LOGI(TAG, "Connecting TCP to %s:%d...", host, port);

    if (!tcp_connect_with_retry(&saddr, bind_ip)) {
        ESP_LOGE(TAG, "TCP connect failed: errno=%d %s", errno, strerror(errno));
        set_last_connect_error("tcp connect failed: %s", strerror(errno));
        int so_error = 0;
        socklen_t optlen = sizeof(so_error);
        if (ssh_sock >= 0) getsockopt(ssh_sock, SOL_SOCKET, SO_ERROR, &so_error, &optlen);
        ESP_LOGE(TAG, "SO_ERROR: %d", so_error);
        if (ssh_sock >= 0) {
            close(ssh_sock);
            ssh_sock = -1;
        }
        delete_rx_queue();
        coex_release();
        return false;
    }

    int keepalive = 1;
    setsockopt(ssh_sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    /* Phase 5: disable Nagle so small interactive writes (keystrokes, query
     * replies) are not coalesced/delayed; matches typical Linux SSH client
     * latency behavior. */
    int nodelay = 1;
    if (setsockopt(ssh_sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) != 0) {
        ESP_LOGW(TAG, "TCP_NODELAY set failed: errno=%d %s", errno, strerror(errno));
    }

    char banner_probe[96];
    if (!ssh_probe_banner_with_timeout(banner_probe, sizeof(banner_probe), 3000)) {
        ESP_LOGW(TAG, "No SSH banner in probe window: errno=%d %s (continuing)", errno, strerror(errno));
    } else {
        ESP_LOGI(TAG, "SSH banner probe (%d bytes): %.80s", (int)strlen(banner_probe), banner_probe);
    }

    /* Use blocking socket during key exchange/auth for stability on ESP32. */
    if (!set_socket_blocking_mode(ssh_sock, true)) {
        ESP_LOGW(TAG, "Failed to switch SSH socket to blocking mode");
    }

    { // libssh2 scope
    int rc = 0;
    for (int hs_try = 1; hs_try <= SSH_HANDSHAKE_RETRIES; hs_try++) {
        rc = libssh2_init(0);
        if (rc != 0) {
            ESP_LOGE(TAG, "libssh2 init failed: %d", rc);
            close(ssh_sock); ssh_sock = -1;
            delete_rx_queue();
            coex_release();
            return false;
        }

        session = libssh2_session_init();
        if (!session) {
            ESP_LOGE(TAG, "Session init failed");
            libssh2_exit();
            close(ssh_sock); ssh_sock = -1;
            delete_rx_queue();
            coex_release();
            return false;
        }

        if (hs_try == 1) {
            log_supported_hostkey_algs(session);
        }

#if SSH_VERBOSE_LOGS
        s_ssh_trace_budget = 1200;
        libssh2_trace_sethandler(session, NULL, ssh_trace_handler);
        libssh2_trace(session, LIBSSH2_TRACE_KEX |
                               LIBSSH2_TRACE_AUTH |
                               LIBSSH2_TRACE_TRANS |
                               LIBSSH2_TRACE_SOCKET |
                               LIBSSH2_TRACE_ERROR);
#else
        libssh2_trace(session, 0);
#endif

        /* Phase 5: graded fallback ladder rather than a hard jump to libssh2
         * defaults.
         *   attempt 1 -> conservative profile (CBC-first, sha1-first) known
         *               stable on ESP/mbedTLS.
         *   attempt 2 -> broadened modern profile (CTR-first, sha2-first) for
         *               servers that reject the legacy-leaning set, while
         *               still pinning hostkey preference.
         *   attempt 3 -> libssh2 built-in defaults (last resort).
         * The hostkey preference list is applied on every non-default attempt
         * so ed25519-only servers keep negotiating across the ladder. */
        bool hostkey_has_ed25519 = false;
        char hostkey_pref[160];
        bool have_hostkey_pref = build_hostkey_method_pref(session,
                                                           hostkey_pref,
                                                           sizeof(hostkey_pref),
                                                           &hostkey_has_ed25519);
        if (hs_try == 1) {
            libssh2_session_method_pref(session, LIBSSH2_METHOD_CRYPT_CS,
                                        "aes128-cbc,aes128-ctr,aes256-ctr,aes192-ctr");
            libssh2_session_method_pref(session, LIBSSH2_METHOD_CRYPT_SC,
                                        "aes128-cbc,aes128-ctr,aes256-ctr,aes192-ctr");
            libssh2_session_method_pref(session, LIBSSH2_METHOD_MAC_CS,
                                        "hmac-sha1,hmac-sha2-256,hmac-sha2-512");
            libssh2_session_method_pref(session, LIBSSH2_METHOD_MAC_SC,
                                        "hmac-sha1,hmac-sha2-256,hmac-sha2-512");
            libssh2_session_method_pref(session, LIBSSH2_METHOD_KEX,
                                        "ecdh-sha2-nistp256,curve25519-sha256,curve25519-sha256@libssh.org,diffie-hellman-group14-sha256,diffie-hellman-group14-sha1");
            if (have_hostkey_pref) {
                libssh2_session_method_pref(session, LIBSSH2_METHOD_HOSTKEY, hostkey_pref);
            }
            ESP_LOGI(TAG, "Using conservative SSH method profile (attempt %d)", hs_try);
        } else if (hs_try == 2) {
            libssh2_session_method_pref(session, LIBSSH2_METHOD_CRYPT_CS,
                                        "aes256-ctr,aes192-ctr,aes128-ctr,aes128-cbc");
            libssh2_session_method_pref(session, LIBSSH2_METHOD_CRYPT_SC,
                                        "aes256-ctr,aes192-ctr,aes128-ctr,aes128-cbc");
            libssh2_session_method_pref(session, LIBSSH2_METHOD_MAC_CS,
                                        "hmac-sha2-256,hmac-sha2-512,hmac-sha1");
            libssh2_session_method_pref(session, LIBSSH2_METHOD_MAC_SC,
                                        "hmac-sha2-256,hmac-sha2-512,hmac-sha1");
            libssh2_session_method_pref(session, LIBSSH2_METHOD_KEX,
                                        "ecdh-sha2-nistp256,ecdh-sha2-nistp384,ecdh-sha2-nistp521,diffie-hellman-group14-sha256,diffie-hellman-group16-sha512");
            if (have_hostkey_pref) {
                libssh2_session_method_pref(session, LIBSSH2_METHOD_HOSTKEY, hostkey_pref);
            }
            ESP_LOGI(TAG, "Using broadened modern SSH method profile (attempt %d)", hs_try);
        } else {
            if (have_hostkey_pref) {
                libssh2_session_method_pref(session, LIBSSH2_METHOD_HOSTKEY, hostkey_pref);
            }
            ESP_LOGI(TAG, "Using libssh2 default SSH method profile (attempt %d)", hs_try);
        }
        if (have_hostkey_pref) {
            ESP_LOGI(TAG, "Hostkey preference list: %s", hostkey_pref);
        }
        if (!hostkey_has_ed25519) {
            ESP_LOGW(TAG,
                     "ssh-ed25519 hostkey algorithm is not available in this client build");
        }

#if SSH_VERBOSE_LOGS
        log_supported_algs(session, LIBSSH2_METHOD_KEX, "local KEX");
        log_supported_algs(session, LIBSSH2_METHOD_HOSTKEY, "local hostkey");
        log_supported_algs(session, LIBSSH2_METHOD_CRYPT_CS, "local c2s ciphers");
        log_supported_algs(session, LIBSSH2_METHOD_MAC_CS, "local c2s mac");
#endif

        libssh2_session_set_blocking(session, 1);
        libssh2_session_set_timeout(session, SSH_HANDSHAKE_TIMEOUT_MS);

        ESP_LOGI(TAG, "Starting SSH handshake (attempt %d/%d)...", hs_try, SSH_HANDSHAKE_RETRIES);
        rc = ssh_handshake_with_timeout(SSH_HANDSHAKE_TIMEOUT_MS);
        if (rc == 0) {
            log_server_hostkey_fingerprint(session);
            if (!verify_or_store_host_fingerprint(host, port, session)) {
                rc = -1000;
            } else {
                break;
            }
        }

        log_session_error("Handshake");
        ESP_LOGE(TAG, "Handshake failed: rc=%d errno=%d %s", rc, errno, strerror(errno));
        if (rc == -1000) {
            ESP_LOGE(TAG, "Handshake accepted but host trust verification failed");
        } else if (rc == LIBSSH2_ERROR_KEX_FAILURE &&
            !session_supports_alg(session, LIBSSH2_METHOD_HOSTKEY, "ssh-ed25519")) {
            ESP_LOGW(TAG,
                     "This SSH client build does not support ssh-ed25519 host keys; servers offering only ssh-ed25519 cannot be used");
            set_last_connect_error("handshake failed: ssh-ed25519 hostkeys unsupported by this build");
        } else if (s_last_connect_error[0] == '\0') {
            set_last_connect_error("handshake failed (rc=%d)", rc);
        }

        libssh2_session_free(session);
        session = NULL;
        libssh2_exit();
        close(ssh_sock);
        ssh_sock = -1;

        if (hs_try >= SSH_HANDSHAKE_RETRIES) {
            delete_rx_queue();
            coex_release();
            return false;
        }

        ESP_LOGW(TAG, "Retrying after handshake failure rc=%d", rc);
        vTaskDelay(pdMS_TO_TICKS(250));
        if (!tcp_connect_with_retry(&saddr, bind_ip)) {
            ESP_LOGE(TAG, "TCP reconnect failed after handshake error");
            set_last_connect_error("tcp reconnect failed after handshake");
            delete_rx_queue();
            coex_release();
            return false;
        }
        int keepalive_retry = 1;
        setsockopt(ssh_sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive_retry, sizeof(keepalive_retry));
    }

    if (rc != 0 || !session) {
        delete_rx_queue();
        coex_release();
        return false;
    }

    ESP_LOGI(TAG, "Handshake OK, authenticating...");

    char auth_methods[96];
    bool none_auth_succeeded = false;
    bool have_auth_methods = ssh_get_auth_methods_with_timeout(user,
                                                               auth_methods,
                                                               sizeof(auth_methods),
                                                               &none_auth_succeeded,
                                                               SSH_HANDSHAKE_TIMEOUT_MS);
    bool publickey_supported = have_auth_methods && strstr(auth_methods, "publickey");
    bool password_supported = have_auth_methods && strstr(auth_methods, "password");
    bool kbdint_supported = have_auth_methods && strstr(auth_methods, "keyboard-interactive");
    bool key_loaded = ssh_has_private_key();

    if (!key_loaded && publickey_supported) {
        key_loaded = ensure_default_identity_key_loaded();
        if (!key_loaded) {
            ESP_LOGW(TAG, "Publickey is supported but default identity key is unavailable");
        }
    }

    if (none_auth_succeeded) {
        ESP_LOGI(TAG, "Auth OK via 'none' method");
        rc = 0;
    } else if (have_auth_methods && publickey_supported && key_loaded) {
        rc = ssh_userauth_publickey_with_timeout(user,
                                                 s_ssh_private_key_pem,
                                                 s_ssh_private_key_passphrase,
                                                 SSH_HANDSHAKE_TIMEOUT_MS);
        if (rc == 0) {
            ESP_LOGI(TAG, "Auth OK via 'publickey' method");
        } else {
            ESP_LOGW(TAG, "Publickey auth failed rc=%d (methods: %s)", rc, auth_methods);
            if (!(pass && pass[0]) && (password_supported || kbdint_supported)) {
                s_last_connect_requires_password = true;
                set_last_connect_error("server requires password authentication");
                ESP_LOGI(TAG, "Prompting for password after publickey failure");
            }
        }
    } else if (!have_auth_methods) {
        ESP_LOGW(TAG, "Could not query auth methods; continuing with configured auth path");
        rc = ssh_userauth_with_timeout(user, pass, SSH_HANDSHAKE_TIMEOUT_MS);
    } else if (pass && pass[0] && (password_supported || kbdint_supported)) {
        if (kbdint_supported) {
            rc = ssh_userauth_kbdint_with_timeout(user, pass, SSH_HANDSHAKE_TIMEOUT_MS);
            if (rc != 0 && password_supported) {
                rc = ssh_userauth_with_timeout(user, pass, SSH_HANDSHAKE_TIMEOUT_MS);
            }
        } else {
            rc = ssh_userauth_with_timeout(user, pass, SSH_HANDSHAKE_TIMEOUT_MS);
        }
    } else {
        if (publickey_supported && !key_loaded && !(pass && pass[0])) {
            set_last_connect_error("server requires publickey auth; load key with sshkey load/import");
            ESP_LOGW(TAG, "Publickey auth required but no key is loaded (methods: %s)", auth_methods);
        } else if (password_supported || kbdint_supported) {
            s_last_connect_requires_password = true;
            set_last_connect_error("server requires password authentication");
            ESP_LOGI(TAG, "Password-style auth required by server (methods: %s)", auth_methods);
        } else if (publickey_supported) {
            set_last_connect_error("server requires publickey auth; key authentication failed");
            ESP_LOGW(TAG, "Publickey auth available but failed (methods: %s)", auth_methods);
        } else {
            set_last_connect_error("no supported auth methods offered by server (%s)", auth_methods);
            ESP_LOGW(TAG, "No passwordless auth available (methods: %s)", auth_methods);
        }
        rc = LIBSSH2_ERROR_AUTHENTICATION_FAILED;
    }

    if (rc != 0 &&
        have_auth_methods &&
        (pass && pass[0]) &&
        (password_supported || kbdint_supported) &&
        !(none_auth_succeeded)) {
        ESP_LOGI(TAG, "Falling back to password-style auth after prior auth failure");
        if (kbdint_supported) {
            rc = ssh_userauth_kbdint_with_timeout(user, pass, SSH_HANDSHAKE_TIMEOUT_MS);
            if (rc != 0 && password_supported) {
                rc = ssh_userauth_with_timeout(user, pass, SSH_HANDSHAKE_TIMEOUT_MS);
            }
        } else {
            rc = ssh_userauth_with_timeout(user, pass, SSH_HANDSHAKE_TIMEOUT_MS);
        }
    }

    if (rc != 0) {
        log_session_error("Auth");
        ESP_LOGE(TAG, "Auth failed: rc=%d", rc);
        if (s_last_connect_error[0] == '\0') {
            set_last_connect_error("authentication failed (rc=%d)", rc);
        }
        libssh2_session_free(session); session = NULL;
        libssh2_exit();
        close(ssh_sock); ssh_sock = -1;
        delete_rx_queue();
        coex_release();
        return false;
    }
    ESP_LOGI(TAG, "Auth OK, opening channel...");

    /* Use non-blocking mode for channel request flow and drive waits explicitly.
     * This avoids long blocking stalls during PTY/shell negotiation on some hosts. */
    if (!set_socket_blocking_mode(ssh_sock, false)) {
        ESP_LOGW(TAG, "Failed to switch SSH socket to non-blocking mode before channel setup");
    }
    libssh2_session_set_blocking(session, 0);

    channel = ssh_channel_open_with_timeout(SSH_HANDSHAKE_TIMEOUT_MS);
    if (!channel) {
        char *errmsg; int errmsg_len;
        int errcode = libssh2_session_last_error(session, &errmsg, &errmsg_len, 0);
        ESP_LOGE(TAG, "Channel open failed: %d %s", errcode, errmsg);
        set_last_connect_error("channel open failed");
        libssh2_session_free(session); session = NULL;
        libssh2_exit();
        close(ssh_sock); ssh_sock = -1;
        delete_rx_queue();
        coex_release();
        return false;
    }

    libssh2_channel_set_blocking(channel, 0);

    ESP_LOGI(TAG, "Channel open OK, requesting PTY...");

    /* Advertise truecolor support to the remote environment. tmux configs
     * (e.g. oh-my-tmux _apply_24b) gate 24-bit color on $COLORTERM; without
     * it tmux degrades truecolor SGR to the 256 palette, leaking the wrong
     * (e.g. default-green) colors and breaking LazyVim bg/fg detection.
     * Sent before pty/shell; non-fatal if the server's AcceptEnv rejects it. */
    {
        int env_rc = ssh_channel_setenv_with_timeout(channel, "COLORTERM", "truecolor", 3000);
        if (env_rc != 0) {
            ESP_LOGW(TAG, "setenv COLORTERM failed (non-fatal): %d", env_rc);
        } else {
            ESP_LOGI(TAG, "Sent COLORTERM=truecolor to remote");
        }
    }

    rc = ssh_channel_request_pty_with_timeout(channel, "xterm-256color", SSH_HANDSHAKE_TIMEOUT_MS);
    /* Terminal dimensions are sent inside pty-req; no separate
     * window-change request (Go SSH server compatibility). */
    if (rc != 0) {
        ESP_LOGW(TAG, "PTY request failed (non-fatal): %d", rc);
    }
    ESP_LOGI(TAG, "Requesting shell...");

#if SSH_VERBOSE_LOGS
    s_ssh_trace_budget = 2500;
#endif

    bool used_no_reply_shell = false;
    rc = ssh_channel_shell_with_timeout(channel, SSH_HANDSHAKE_TIMEOUT_MS);
    if (rc != 0 && rc == LIBSSH2_ERROR_TIMEOUT) {
        ESP_LOGW(TAG, "Shell request timed out after PTY; retrying alternate startup paths");

        if (rc != 0) {
            bool timed_out_shell_adopted = false;
            if (channel && !libssh2_channel_eof(channel)) {
                ESP_LOGW(TAG, "Trying to adopt timed-out shell channel via active probes");
                if (ssh_channel_bootstrap_expect_output(channel, 2500, 8000)) {
                    ESP_LOGI(TAG, "Timed-out shell channel produced output; adopting channel");
                    rc = 0;
                    timed_out_shell_adopted = true;
                } else {
                    ESP_LOGW(TAG, "Timed-out shell channel remained silent after probes");
                    if (!libssh2_channel_eof(channel)) {
                        int nr_rc = ssh_channel_shell_no_reply_with_timeout(channel, 5000);
                        if (nr_rc == 0 && !libssh2_channel_eof(channel)) {
                            ESP_LOGW(TAG,
                                     "Timed-out shell channel accepted explicit no-reply shell request");
                            if (ssh_channel_bootstrap_expect_output(channel, 2000, 6000)) {
                                ESP_LOGI(TAG,
                                         "Timed-out shell channel produced output after explicit no-reply shell");
                            } else {
                                ESP_LOGW(TAG,
                                         "Timed-out shell channel still silent after explicit no-reply shell");
                            }
                            rc = 0;
                            used_no_reply_shell = true;
                            timed_out_shell_adopted = true;
                        } else if (nr_rc != 0 && nr_rc != LIBSSH2_ERROR_TIMEOUT) {
                            ESP_LOGW(TAG,
                                     "Explicit no-reply shell request on timed-out channel failed rc=%d",
                                     nr_rc);
                        }
                    }

                    if (!timed_out_shell_adopted && !libssh2_channel_eof(channel)) {
                        ESP_LOGW(TAG,
                                 "Accepting timed-out shell channel without reply; server may omit shell success/failure");
                        rc = 0;
                        used_no_reply_shell = true;
                        timed_out_shell_adopted = true;
                    }
                }
            }

            if (!timed_out_shell_adopted && channel) {
                libssh2_channel_close(channel);
                libssh2_channel_free(channel);
                channel = NULL;
            }
        }

        if (rc != 0) {
            bool pty_no_reply_ok = ssh_open_pty_shell_no_reply_fallback("xterm", 10000, 8000, 2500) ||
                                   ssh_open_pty_shell_no_reply_fallback("vt100", 10000, 8000, 2500);
            if (pty_no_reply_ok) {
                rc = 0;
                used_no_reply_shell = true;
            }
        }

        if (rc != 0) {
            const char *no_reply_exec_cmds[] = {
                "printf __DUMBESPTY_EXEC_READY__\\n; exec bash -il",
                "printf __DUMBESPTY_EXEC_READY__\\n; exec sh -il",
                "printf __DUMBESPTY_EXEC_READY__\\n; exec /bin/bash -il",
                "printf __DUMBESPTY_EXEC_READY__\\n; exec /bin/sh -il"
            };

            bool pty_no_reply_exec_ok = false;
            for (size_t i = 0; i < sizeof(no_reply_exec_cmds) / sizeof(no_reply_exec_cmds[0]) && !pty_no_reply_exec_ok; i++) {
                pty_no_reply_exec_ok = ssh_open_pty_exec_no_reply_fallback("xterm", no_reply_exec_cmds[i], 10000, 10000, 3000) ||
                                       ssh_open_pty_exec_no_reply_fallback("vt100", no_reply_exec_cmds[i], 10000, 10000, 3000);
            }

            if (pty_no_reply_exec_ok) {
                rc = 0;
                used_no_reply_shell = true;
            }
        }

        if (rc != 0) {
            bool pty_retry_ok = ssh_open_pty_shell_fallback("xterm", 10000, 25000, 2500) ||
                                ssh_open_pty_shell_fallback("vt100", 10000, 25000, 2500);
            rc = pty_retry_ok ? 0 : LIBSSH2_ERROR_TIMEOUT;
        }

        if (rc != 0 && ssh_open_shell_no_pty_fallback(30000, 2500)) {
            rc = 0;
        }

        if (rc != 0 && ssh_open_raw_channel_fallback(2500)) {
            ESP_LOGI(TAG, "Using raw channel fallback (no shell/exec request)");
            rc = 0;
        }

        if (rc != 0) {
            const char *pty_exec_cmds[] = {
                "exec bash -il",
                "exec sh -il",
                "bash -il",
                "sh -il",
                "/bin/bash -il",
                "/bin/sh -il"
            };

            bool pty_exec_ok = false;
            for (size_t i = 0; i < sizeof(pty_exec_cmds) / sizeof(pty_exec_cmds[0]) && !pty_exec_ok; i++) {
                pty_exec_ok = ssh_open_pty_exec_fallback("xterm", pty_exec_cmds[i], 10000, 15000, 2500) ||
                              ssh_open_pty_exec_fallback("vt100", pty_exec_cmds[i], 10000, 15000, 2500);
            }

            rc = pty_exec_ok ? 0 : rc;
        }

        if (rc != 0) {
            const char *fallback_cmds[] = {
                "exec bash -i",
                "exec sh -i",
                "exec /bin/sh -i",
                "bash -il",
                "sh -il",
                "/bin/sh -il",
                "/bin/sh"
            };

            for (size_t i = 0; i < sizeof(fallback_cmds) / sizeof(fallback_cmds[0]); i++) {
                channel = ssh_channel_open_with_timeout(10000);
                if (!channel) {
                    rc = LIBSSH2_ERROR_CHANNEL_FAILURE;
                    continue;
                }

                libssh2_channel_set_blocking(channel, 0);
                rc = ssh_channel_exec_with_timeout(channel, fallback_cmds[i], 10000);
                if (rc == 0 && ssh_channel_stays_open_briefly(channel, 1800)) {
                    ESP_LOGI(TAG, "Shell fallback exec succeeded: %s", fallback_cmds[i]);
                    break;
                }

                if (rc == 0) {
                    ESP_LOGW(TAG, "Fallback exec closed immediately: %s", fallback_cmds[i]);
                    rc = LIBSSH2_ERROR_CHANNEL_CLOSED;
                }

                libssh2_channel_close(channel);
                libssh2_channel_free(channel);
                channel = NULL;
            }

            if (rc != 0) {
                channel = ssh_channel_open_with_timeout(10000);
                if (channel) {
                    libssh2_channel_set_blocking(channel, 0);
                    rc = ssh_channel_shell_with_timeout(channel, 15000);
                    if (rc == 0 && ssh_channel_stays_open_briefly(channel, 1800)) {
                        ESP_LOGI(TAG, "Shell request succeeded without PTY fallback");
                    } else {
                        if (rc == 0) {
                            ESP_LOGW(TAG, "No-PTY shell opened then closed immediately");
                            rc = LIBSSH2_ERROR_CHANNEL_CLOSED;
                        }
                        libssh2_channel_close(channel);
                        libssh2_channel_free(channel);
                        channel = NULL;
                    }
                }
            }
        }
    }

    if (channel && libssh2_channel_eof(channel)) {
        ESP_LOGW(TAG, "Shell channel closed before session became interactive");

        libssh2_channel_close(channel);
        libssh2_channel_free(channel);
        channel = NULL;

        if (ssh_open_raw_channel_fallback(2500)) {
            ESP_LOGI(TAG, "Recovered with raw channel fallback after EOF");
            rc = 0;
        } else {
            rc = LIBSSH2_ERROR_CHANNEL_CLOSED;
        }
    }

    if (rc != 0) {
        log_session_error("Shell request");
        ESP_LOGE(TAG, "Shell request failed: %d", rc);
        set_last_connect_error("shell request failed");
        if (channel) {
            libssh2_channel_close(channel);
            libssh2_channel_free(channel);
            channel = NULL;
        }
        libssh2_session_free(session); session = NULL;
        libssh2_exit();
        close(ssh_sock); ssh_sock = -1;
        delete_rx_queue();
        coex_release();
        return false;
    }

    libssh2_channel_handle_extended_data2(channel, LIBSSH2_CHANNEL_EXTENDED_DATA_MERGE);

    ssh_rx_queue = create_rx_queue_with_fallback();
    if (!ssh_rx_queue) {
        ESP_LOGE(TAG, "Failed to create rx queue");
        set_last_connect_error("rx queue allocation failed");
        libssh2_channel_close(channel);
        libssh2_channel_free(channel);
        channel = NULL;
        libssh2_session_free(session); session = NULL;
        libssh2_exit();
        close(ssh_sock); ssh_sock = -1;
        coex_release();
        return false;
    }

    ssh_connected = true;
    /* Phase 5: enable application keepalive (no forced server reply so it
     * never blocks; we just need periodic traffic to keep NAT/idle links
     * alive and to surface dead peers promptly). */
    if (session) {
        libssh2_keepalive_config(session, 0, kSshKeepaliveIntervalSec);
    }
    s_keepalive_last_send = xTaskGetTickCount();
    s_rx_trace_budget = kRxTraceBudgetDefault;
    s_rx_preview_budget = kRxPreviewBudgetDefault;
    s_rx_prev_tail = 0;
    s_rx_loop_budget = 120;
    s_queue_drain_budget = 80;
    s_rx_raw_total = 0;
    s_rx_forwarded_total = 0;
    s_rx_filtered_total = 0;
    s_rx_queue_drop_total = 0;
    s_rx_raw_prev = 0;
    s_rx_forwarded_prev = 0;
    s_rx_filtered_prev = 0;
    s_rx_queue_drop_prev = 0;
    s_rx_diag_last_log = xTaskGetTickCount();
    s_rx_diag_budget = kRxDiagBudgetDefault;
    s_tx_total = 0;
    s_tx_prev = 0;

    ESP_LOGI(TAG,
             "Negotiated: kex=%s hostkey=%s c2s=%s s2c=%s mac_c2s=%s mac_s2c=%s",
             safe_method_name(LIBSSH2_METHOD_KEX),
             safe_method_name(LIBSSH2_METHOD_HOSTKEY),
             safe_method_name(LIBSSH2_METHOD_CRYPT_CS),
             safe_method_name(LIBSSH2_METHOD_CRYPT_SC),
             safe_method_name(LIBSSH2_METHOD_MAC_CS),
             safe_method_name(LIBSSH2_METHOD_MAC_SC));

    /* Keep non-blocking mode for interactive operation loops. */
    libssh2_channel_set_blocking(channel, 0);
    if (!set_socket_blocking_mode(ssh_sock, false)) {
        ESP_LOGW(TAG, "Failed to restore SSH socket non-blocking mode");
    }
    if (session) {
        libssh2_session_set_blocking(session, 0);
    } else {
        ESP_LOGW(TAG, "Session disappeared before non-blocking restore");
    }

    /* Clear the local shell clutter now that auth+pty+shell have all
     * succeeded, BEFORE the recv task starts enqueuing remote output. Doing
     * the clear here (instead of from the shell worker task after connect)
     * guarantees it precedes the server's login banner/MOTD (e.g. Armbian
     * ASCII art) in terminal-write order, so the banner is preserved, and it
     * only runs on a fully successful connect so the password prompt on a
     * failed first auth attempt is never wiped. */
    if (ssh_term) {
        terminal_write(ssh_term, "\033[2J\033[H", -1);
    }

#if SSH_USE_RX_TASK
    ESP_LOGD(TAG, "Starting SSH recv task...");
    BaseType_t rx_task_ok = xTaskCreatePinnedToCore(ssh_recv_task,
                                                    "ssh_recv",
                                                    8192,
                                                    NULL,
                                                    5,
                                                    &ssh_recv_task_handle,
                                                    0);
    if (rx_task_ok != pdPASS) {
        rx_task_ok = xTaskCreate(ssh_recv_task,
                                 "ssh_recv",
                                 8192,
                                 NULL,
                                 5,
                                 &ssh_recv_task_handle);
    }
    if (rx_task_ok != pdPASS) {
        ESP_LOGW(TAG, "Failed to create ssh_recv task, using foreground RX pump");
        ssh_recv_task_handle = NULL;
    }
#else
    ssh_recv_task_handle = NULL;
#endif
    coex_release();

    ESP_LOGI(TAG, "SSH connected to %s:%d as %s", host, port, user);

    if (used_no_reply_shell) {
        bool saw_data = false;
        static const char *probe_seq[] = {
            "\r",
            "\n",
            "\r\n",
            " "
        };
        static const int probe_len[] = {1, 1, 2, 1};

        for (size_t i = 0; i < sizeof(probe_seq) / sizeof(probe_seq[0]); i++) {
            int wr = ssh_write(probe_seq[i], probe_len[i]);
            if (wr < 0) {
                ESP_LOGW(TAG, "No-reply shell bootstrap write failed at try %u: %d", (unsigned)(i + 1), wr);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(150));
            ssh_pump_rx_once(8);
            if (ssh_rx_queue && uxQueueMessagesWaiting(ssh_rx_queue) > 0) {
                ESP_LOGI(TAG, "No-reply shell bootstrap received remote data on try %u", (unsigned)(i + 1));
                saw_data = true;
                break;
            }
        }
        if (!saw_data) {
            static const char *force_cmd = "echo __DUMBESPTY_READY__\r";
            int wr = ssh_write(force_cmd, (int)strlen(force_cmd));
            if (wr >= 0) {
                for (int i = 0; i < 12; i++) {
                    vTaskDelay(pdMS_TO_TICKS(150));
                    ssh_pump_rx_once(8);
                    if (ssh_rx_queue && uxQueueMessagesWaiting(ssh_rx_queue) > 0) {
                        ESP_LOGI(TAG, "No-reply shell produced output after forced echo probe");
                        saw_data = true;
                        break;
                    }
                }
            } else {
                ESP_LOGW(TAG, "No-reply shell forced echo probe write failed: %d", wr);
            }
        }

        if (!saw_data) {
            TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(12000);
            while (xTaskGetTickCount() < deadline) {
                vTaskDelay(pdMS_TO_TICKS(200));
                ssh_pump_rx_once(8);
                if (ssh_rx_queue && uxQueueMessagesWaiting(ssh_rx_queue) > 0) {
                    ESP_LOGI(TAG, "No-reply shell produced delayed output during wait window");
                    saw_data = true;
                    break;
                }
            }
        }

        if (!saw_data) {
            ESP_LOGW(TAG, "No-reply shell bootstrap completed with no remote output");
        }
    }

    return true;
    }
}

void ssh_disconnect(void) {
    ssh_connected = false;

    if (ssh_recv_task_handle) {
        vTaskDelete(ssh_recv_task_handle);
        ssh_recv_task_handle = NULL;
    }

    if (channel) {
        int exit_status = libssh2_channel_get_exit_status(channel);
        ESP_LOGI(TAG, "Closing channel (exit_status=%d)", exit_status);
        libssh2_channel_send_eof(channel);
        libssh2_channel_wait_eof(channel);
        libssh2_channel_close(channel);
        libssh2_channel_free(channel);
        channel = NULL;
    }
    if (session) {
        libssh2_session_disconnect(session, "Normal shutdown");
        libssh2_session_free(session);
        session = NULL;
    }
    if (ssh_sock >= 0) {
        close(ssh_sock);
        ssh_sock = -1;
    }
    if (ssh_write_mutex) {
        vSemaphoreDelete(ssh_write_mutex);
        ssh_write_mutex = NULL;
    }
    delete_rx_queue();
    libssh2_exit();

    coex_release();

    ESP_LOGI(TAG, "SSH disconnected");
}

bool ssh_is_connected(void) {
    return ssh_connected;
}

int ssh_write(const char *data, int len) {
    if (!ssh_connected || !channel) return -1;

    if (!ssh_write_mutex) {
        ssh_write_mutex = xSemaphoreCreateMutex();
        if (!ssh_write_mutex) return -1;
    }
    if (xSemaphoreTake(ssh_write_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) return -1;

    int total = 0;
    while (total < len) {
        int rc = libssh2_channel_write(channel, data + total, len - total);
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            waitsocket(ssh_sock, session);
            continue;
        }
        if (rc < 0) {
            log_ssh_runtime_error_context("SSH write error", rc);
            ssh_connected = false;
            xSemaphoreGive(ssh_write_mutex);
            return rc;
        }
        s_tx_total += (uint32_t)rc;
        total += rc;
    }
    xSemaphoreGive(ssh_write_mutex);
    return total;
}

void ssh_process_queue(void) {
    if (!ssh_rx_queue) return;

    if (ssh_connected && channel && !ssh_recv_task_handle) {
        ssh_pump_rx_once(4);
    }

    /* Phase 5: drive periodic keepalive. libssh2_keepalive_send reports the
     * seconds until the next one is due; we just gate on our own interval and
     * surface send failures as a diagnosable disconnect cause. */
    if (ssh_connected && session) {
        TickType_t now = xTaskGetTickCount();
        if (now - s_keepalive_last_send >= pdMS_TO_TICKS(kSshKeepaliveIntervalSec * 1000)) {
            int seconds_to_next = 0;
            int ka_rc = libssh2_keepalive_send(session, &seconds_to_next);
            s_keepalive_last_send = now;
            if (ka_rc != 0 && ka_rc != LIBSSH2_ERROR_EAGAIN) {
                ESP_LOGW(TAG, "Keepalive send failed rc=%d (%s)",
                         ka_rc, ssh_transport_rc_str(ka_rc));
            }
        }
    }

    ssh_rx_msg_t msg;
    int drained = 0;
    while (xQueueReceive(ssh_rx_queue, &msg, 0) == pdTRUE) {
        drained++;
        if (ssh_term) {
            terminal_write(ssh_term, msg.data, msg.len);
        }
    }

    if (drained > 0) {
        // SSH terminal output counts as activity: keep the screen awake while a
        // remote session is producing output (e.g. logs, TUI redraws).
        power_mark_activity();
        if (s_queue_drain_budget > 0) s_queue_drain_budget--;
    }

    if (ssh_connected && s_rx_diag_budget > 0) {
        TickType_t now = xTaskGetTickCount();
        if (now - s_rx_diag_last_log >= pdMS_TO_TICKS(1000)) {
            uint32_t raw_delta = s_rx_raw_total - s_rx_raw_prev;
            uint32_t fwd_delta = s_rx_forwarded_total - s_rx_forwarded_prev;
            uint32_t filt_delta = s_rx_filtered_total - s_rx_filtered_prev;
            uint32_t drop_delta = s_rx_queue_drop_total - s_rx_queue_drop_prev;
            uint32_t tx_delta = s_tx_total - s_tx_prev;
            uint32_t q_depth = ssh_rx_queue ? (uint32_t)uxQueueMessagesWaiting(ssh_rx_queue) : 0;

            ESP_LOGI(TAG,
                     "IO diag d(rx=%u fwd=%u filt=%u drop=%u tx=%u q=%u) t(rx=%u fwd=%u filt=%u drop=%u tx=%u)",
                     (unsigned)raw_delta,
                     (unsigned)fwd_delta,
                     (unsigned)filt_delta,
                     (unsigned)drop_delta,
                     (unsigned)tx_delta,
                     (unsigned)q_depth,
                     (unsigned)s_rx_raw_total,
                     (unsigned)s_rx_forwarded_total,
                     (unsigned)s_rx_filtered_total,
                     (unsigned)s_rx_queue_drop_total,
                     (unsigned)s_tx_total);

            s_rx_raw_prev = s_rx_raw_total;
            s_rx_forwarded_prev = s_rx_forwarded_total;
            s_rx_filtered_prev = s_rx_filtered_total;
            s_rx_queue_drop_prev = s_rx_queue_drop_total;
            s_tx_prev = s_tx_total;
            s_rx_diag_last_log = now;
            s_rx_diag_budget--;
        }
    }

    if (!ssh_connected && !s_ssh_connecting) {
        if (channel || session || ssh_sock >= 0) {
            ssh_disconnect();
        }
        delete_rx_queue();
        if (shell_is_ssh_active()) {
            shell_set_ssh_active(false);
        }
    }
}

void ssh_set_terminal(terminal_t *term) {
    ssh_term = term;
}

void ssh_terminal_output_cb(const char *data, size_t len) {
    if (!data || len == 0) return;
    if (!ssh_connected || !channel) return;
    if (len <= 16) {
        char hex[64];
        int p = 0;
        for (size_t i = 0; i < len && p < (int)sizeof(hex) - 4; i++) {
            p += snprintf(hex + p, sizeof(hex) - p, "%02X ", (uint8_t)data[i]);
        }
        ESP_LOGI(TAG, "TX %u bytes: %s", (unsigned)len, hex);
    }
    ssh_write(data, (int)len);
}
