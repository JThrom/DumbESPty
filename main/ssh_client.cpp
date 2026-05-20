#include "ssh_client.hpp"
#include "shell.hpp"
#include "wifi_mgr.hpp"
#include "terminal.hpp"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include "libssh2.h"
#include "coex_manager.hpp"

static const char *TAG = "SSH_CLIENT";

#define SSH_RX_QUEUE_SIZE 128
#define SSH_RX_BUF_SIZE 256
#define SSH_HANDSHAKE_TIMEOUT_MS 45000
#define SSH_CONNECT_TIMEOUT_MS 5000
#define SSH_CONNECT_RETRIES 3

typedef struct {
    char data[SSH_RX_BUF_SIZE];
    uint16_t len;
} ssh_rx_msg_t;

static bool ssh_connected = false;
static int ssh_sock = -1;
static LIBSSH2_SESSION *session = NULL;
static LIBSSH2_CHANNEL *channel = NULL;
static QueueHandle_t ssh_rx_queue = NULL;
static TaskHandle_t ssh_recv_task_handle = NULL;
static terminal_t *ssh_term = NULL;
static SemaphoreHandle_t ssh_write_mutex = NULL;
static int s_rx_trace_budget = 160;
static uint8_t s_rx_prev_tail = 0;

static QueueHandle_t create_rx_queue_with_fallback(void) {
    static const uint16_t depth_candidates[] = {SSH_RX_QUEUE_SIZE, 96, 64, 48, 32, 24};
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
}

static int waitsocket(int sock, LIBSSH2_SESSION *sess);

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

static int ssh_channel_request_pty_with_timeout(LIBSSH2_CHANNEL *ch, const char *termtype, int timeout_ms) {
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        int rc = libssh2_channel_request_pty(ch, termtype);
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
    return LIBSSH2_ERROR_TIMEOUT;
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

static bool tcp_connect_with_retry(const struct sockaddr_in *saddr) {
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

    (void)sess;

    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_SET(sock, &rfds);
    FD_SET(sock, &wfds);
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    return select(sock + 1, &rfds, &wfds, NULL, &timeout);
}

static void ssh_recv_task(void *param) {
    char buf[1024];

    while (ssh_connected && channel) {
        int rc = libssh2_channel_read(channel, buf, sizeof(buf) - 1);

        if (rc > 0) {
            buf[rc] = '\0';
            size_t offset = 0;
            while (offset < (size_t)rc) {
                ssh_rx_msg_t msg;
                size_t chunk = rc - offset;
                if (chunk > SSH_RX_BUF_SIZE - 1) chunk = SSH_RX_BUF_SIZE - 1;
                msg.len = filter_and_reply_fast_queries(buf + offset, chunk, msg.data);
                log_rx_trace(msg.data, msg.len);
                if (msg.len > 0) {
                    xQueueSend(ssh_rx_queue, &msg, pdMS_TO_TICKS(20));
                }
                offset += chunk;
            }
        } else if (rc == LIBSSH2_ERROR_EAGAIN) {
            waitsocket(ssh_sock, session);
            vTaskDelay(pdMS_TO_TICKS(50));
        } else {
            break;
        }

        if (libssh2_channel_eof(channel)) break;
    }

    ESP_LOGI(TAG, "SSH recv task ended");
    ssh_connected = false;
    ssh_recv_task_handle = NULL;
    vTaskDelete(NULL);
}

bool ssh_connect(const char *host, uint16_t port, const char *user, const char *pass) {
    if (ssh_connected) {
        ESP_LOGW(TAG, "Already connected");
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
        coex_release();
        return false;
    }

    ESP_LOGI(TAG, "Resolving %s...", host);
    struct hostent *he = gethostbyname(host);
    if (!he) {
        ESP_LOGE(TAG, "DNS lookup failed for %s", host);
        close(ssh_sock); ssh_sock = -1;
        delete_rx_queue();
        coex_release();
        return false;
    }
    memcpy(&saddr.sin_addr, he->h_addr, he->h_length);
    ESP_LOGI(TAG, "Resolved %s to %s", host, inet_ntoa(saddr.sin_addr));
    ESP_LOGI(TAG, "Connecting TCP to %s:%d...", host, port);

    if (!tcp_connect_with_retry(&saddr)) {
        ESP_LOGE(TAG, "TCP connect failed: errno=%d %s", errno, strerror(errno));
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

    char banner_probe[96];
    if (!ssh_probe_banner_with_timeout(banner_probe, sizeof(banner_probe), 3000)) {
        ESP_LOGW(TAG, "No SSH banner in probe window: errno=%d %s (continuing)", errno, strerror(errno));
    } else {
        ESP_LOGI(TAG, "SSH banner probe (%d bytes): %.80s", (int)strlen(banner_probe), banner_probe);
    }

    { // libssh2 scope
    int rc = libssh2_init(0);
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

    libssh2_session_set_blocking(session, 0);
    libssh2_session_set_timeout(session, SSH_HANDSHAKE_TIMEOUT_MS);

    ESP_LOGI(TAG, "Starting SSH handshake...");
    rc = ssh_handshake_with_timeout(SSH_HANDSHAKE_TIMEOUT_MS);
    if (rc != 0) {
        log_session_error("Handshake");
        ESP_LOGE(TAG, "Handshake failed: rc=%d errno=%d %s", rc, errno, strerror(errno));
        libssh2_session_free(session); session = NULL;
        libssh2_exit();
        close(ssh_sock); ssh_sock = -1;
        delete_rx_queue();
        coex_release();
        return false;
    }
    ESP_LOGI(TAG, "Handshake OK, authenticating...");

    rc = ssh_userauth_with_timeout(user, pass, SSH_HANDSHAKE_TIMEOUT_MS);
    if (rc != 0) {
        log_session_error("Auth");
        ESP_LOGE(TAG, "Auth failed: rc=%d", rc);
        libssh2_session_free(session); session = NULL;
        libssh2_exit();
        close(ssh_sock); ssh_sock = -1;
        delete_rx_queue();
        coex_release();
        return false;
    }
    ESP_LOGI(TAG, "Auth OK, opening channel...");

    channel = ssh_channel_open_with_timeout(SSH_HANDSHAKE_TIMEOUT_MS);
    if (!channel) {
        char *errmsg; int errmsg_len;
        int errcode = libssh2_session_last_error(session, &errmsg, &errmsg_len, 0);
        ESP_LOGE(TAG, "Channel open failed: %d %s", errcode, errmsg);
        libssh2_session_free(session); session = NULL;
        libssh2_exit();
        close(ssh_sock); ssh_sock = -1;
        delete_rx_queue();
        coex_release();
        return false;
    }

    // Keep channel non-blocking during PTY/shell startup so timeout wrappers
    // can handle EAGAIN instead of blocking inside libssh2 internals.
    libssh2_channel_set_blocking(channel, 0);

    ESP_LOGI(TAG, "Channel open OK, requesting PTY...");

    rc = ssh_channel_request_pty_with_timeout(channel, "xterm-256color", SSH_HANDSHAKE_TIMEOUT_MS);
    if (rc == 0 && ssh_term) {
        libssh2_channel_request_pty_size(channel,
                                         ssh_term->cols,
                                         ssh_term->rows);
    }
    if (rc != 0) {
        ESP_LOGW(TAG, "PTY request failed (non-fatal): %d", rc);
    }
    ESP_LOGI(TAG, "Requesting shell...");

    rc = ssh_channel_shell_with_timeout(channel, SSH_HANDSHAKE_TIMEOUT_MS);
    if (rc != 0) {
        log_session_error("Shell request");
        ESP_LOGE(TAG, "Shell request failed: %d", rc);
        libssh2_channel_close(channel); channel = NULL;
        libssh2_session_free(session); session = NULL;
        libssh2_exit();
        close(ssh_sock); ssh_sock = -1;
        delete_rx_queue();
        coex_release();
        return false;
    }

    ssh_rx_queue = create_rx_queue_with_fallback();
    if (!ssh_rx_queue) {
        ESP_LOGE(TAG, "Failed to create rx queue");
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
    s_rx_trace_budget = 160;
    s_rx_prev_tail = 0;

    xTaskCreate(ssh_recv_task, "ssh_recv", 8192, NULL, 5, &ssh_recv_task_handle);
    coex_release();

    ESP_LOGI(TAG, "SSH connected to %s:%d as %s", host, port, user);
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
            ssh_connected = false;
            xSemaphoreGive(ssh_write_mutex);
            return rc;
        }
        total += rc;
    }
    xSemaphoreGive(ssh_write_mutex);
    return total;
}

void ssh_process_queue(void) {
    if (!ssh_rx_queue) return;

    ssh_rx_msg_t msg;
    while (xQueueReceive(ssh_rx_queue, &msg, 0) == pdTRUE) {
        if (ssh_term) {
            terminal_write(ssh_term, msg.data, msg.len);
        }
    }

    if (!ssh_connected) {
        if (channel || session || ssh_sock >= 0) {
            ssh_disconnect();
        }
        if (ssh_rx_queue) {
            vQueueDelete(ssh_rx_queue);
            ssh_rx_queue = NULL;
        }
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
