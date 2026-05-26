#include "tailscale_mgr.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "microlink.h"
#include "nvs.h"
#include "hostname_mgr.hpp"
#include "secret_vault.hpp"
#include "shell.hpp"
#include "wifi_mgr.hpp"

static constexpr char TS_NS[] = "tailscale";
static constexpr char TS_KEY_ENABLED[] = "enabled";
static constexpr char TS_KEY_BACKEND[] = "backend";
static constexpr char TS_KEY_TAILNET[] = "tailnet";
static constexpr char TS_KEY_CTRL_URL[] = "ctrl_url";

static constexpr char TS_SECRET_AUTHKEY[] = "ts_authkey";
static constexpr char TS_SECRET_SAAS_TOKEN[] = "ts_saas_tok";
static constexpr char TS_SECRET_HEADSCALE_TOKEN[] = "ts_hs_tok";

static constexpr int64_t TS_STATUS_LOG_INTERVAL_US = 15LL * 1000LL * 1000LL;
static constexpr int64_t TS_AUTORETRY_INTERVAL_US = 5LL * 1000LL * 1000LL;

typedef struct {
    bool enabled;
    tailscale_backend_t backend;
    char tailnet[64];
    char control_url[128];
} tailscale_cfg_t;

typedef enum {
    TS_PENDING_NONE = 0,
    TS_PENDING_SET_AUTHKEY,
    TS_PENDING_SET_TOKEN,
    TS_PENDING_SET_SECRET_SETUP_VAULT,
    TS_PENDING_SET_SECRET_SETUP_VAULT_CONFIRM,
    TS_PENDING_SET_SECRET_STORE,
    TS_PENDING_UP_VAULT,
} ts_pending_t;

static tailscale_cfg_t s_cfg = {
    .enabled = false,
    .backend = TS_BACKEND_SAAS,
    .tailnet = "",
    .control_url = "https://headscale.local",
};

static bool s_connected = false;
static bool s_connecting = false;
static bool s_last_wifi_connected = false;
static int64_t s_last_status_log_us = 0;
static int64_t s_last_autoretry_us = 0;
static uint32_t s_secret_store_ok = 0;
static uint32_t s_secret_store_fail = 0;
static uint32_t s_secret_load_ok = 0;
static uint32_t s_secret_load_fail = 0;
static uint32_t s_up_requests = 0;
static uint32_t s_start_ok = 0;
static uint32_t s_start_fail = 0;

static microlink_t *s_ml = NULL;
static microlink_state_t s_ml_state = ML_STATE_IDLE;
static bool s_session_armed = false;
static char s_runtime_authkey[256] = "";

static ts_pending_t s_pending = TS_PENDING_NONE;
static char s_pending_secret[256] = "";
static char s_pending_vault_pass[128] = "";
static const char *s_pending_secret_key = NULL;
static const char *TAG = "tailscale";

static void ts_println(const char *msg) {
    shell_print("\r\n  ");
    shell_print(msg);
    shell_print("\r\n");
}

static void ts_print_err(const char *prefix, esp_err_t err) {
    char line[128];
    snprintf(line, sizeof(line), "%s: %s", prefix, esp_err_to_name(err));
    ts_println(line);
}

static const char *token_key_for_backend(void) {
    return s_cfg.backend == TS_BACKEND_SAAS ? TS_SECRET_SAAS_TOKEN : TS_SECRET_HEADSCALE_TOKEN;
}

static const char *ml_state_name(microlink_state_t state) {
    switch (state) {
        case ML_STATE_IDLE: return "idle";
        case ML_STATE_WIFI_WAIT: return "wifi_wait";
        case ML_STATE_CONNECTING: return "connecting";
        case ML_STATE_REGISTERING: return "registering";
        case ML_STATE_CONNECTED: return "connected";
        case ML_STATE_RECONNECTING: return "reconnecting";
        case ML_STATE_ERROR: return "error";
        default: return "unknown";
    }
}

static void ts_log_status(const char *reason) {
    ESP_LOGI(TAG,
             "%s | enabled=%d wifi=%d connected=%d connecting=%d backend=%s host=%s ml=%d ml_state=%s armed=%d up=%u start_ok=%u start_fail=%u store_ok=%u store_fail=%u load_ok=%u load_fail=%u",
             reason,
             s_cfg.enabled ? 1 : 0,
             wifi_mgr_is_connected() ? 1 : 0,
             s_connected ? 1 : 0,
             s_connecting ? 1 : 0,
             tailscale_mgr_backend_name(),
             hostname_mgr_get(),
             s_ml ? 1 : 0,
             ml_state_name(s_ml_state),
             s_session_armed ? 1 : 0,
             (unsigned)s_up_requests,
             (unsigned)s_start_ok,
             (unsigned)s_start_fail,
             (unsigned)s_secret_store_ok,
             (unsigned)s_secret_store_fail,
             (unsigned)s_secret_load_ok,
             (unsigned)s_secret_load_fail);
}

static void ts_sync_state(void) {
    if (!s_ml) {
        s_ml_state = ML_STATE_IDLE;
        s_connected = false;
        s_connecting = false;
        return;
    }

    s_ml_state = microlink_get_state(s_ml);
    s_connected = microlink_is_connected(s_ml);
    s_connecting = !s_connected &&
                   (s_ml_state == ML_STATE_WIFI_WAIT ||
                    s_ml_state == ML_STATE_CONNECTING ||
                    s_ml_state == ML_STATE_REGISTERING ||
                    s_ml_state == ML_STATE_RECONNECTING);
}

static void ts_stop_session(const char *reason) {
    if (!s_ml) {
        ts_sync_state();
        return;
    }

    ESP_LOGI(TAG, "microlink stop (%s)", reason);
    microlink_stop(s_ml);
    microlink_destroy(s_ml);
    s_ml = NULL;
    ts_sync_state();
}

static void ts_on_state_change(microlink_t *ml, microlink_state_t state, void *user_data) {
    (void)ml;
    (void)user_data;
    s_ml_state = state;
    ts_sync_state();
    ESP_LOGI(TAG, "microlink state callback: %s", ml_state_name(state));
}

static void ts_save_cfg(void) {
    nvs_handle_t nvs = 0;
    if (nvs_open(TS_NS, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_u8(nvs, TS_KEY_ENABLED, s_cfg.enabled ? 1 : 0);
    nvs_set_u8(nvs, TS_KEY_BACKEND, (uint8_t)s_cfg.backend);
    nvs_set_str(nvs, TS_KEY_TAILNET, s_cfg.tailnet);
    nvs_set_str(nvs, TS_KEY_CTRL_URL, s_cfg.control_url);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void ts_load_cfg(void) {
    nvs_handle_t nvs = 0;
    if (nvs_open(TS_NS, NVS_READONLY, &nvs) != ESP_OK) return;

    uint8_t enabled = 0;
    uint8_t backend = 0;
    size_t len = 0;

    if (nvs_get_u8(nvs, TS_KEY_ENABLED, &enabled) == ESP_OK) s_cfg.enabled = enabled != 0;
    if (nvs_get_u8(nvs, TS_KEY_BACKEND, &backend) == ESP_OK && backend <= TS_BACKEND_HEADSCALE) {
        s_cfg.backend = (tailscale_backend_t)backend;
    }

    len = sizeof(s_cfg.tailnet);
    if (nvs_get_str(nvs, TS_KEY_TAILNET, s_cfg.tailnet, &len) != ESP_OK) s_cfg.tailnet[0] = '\0';

    len = sizeof(s_cfg.control_url);
    if (nvs_get_str(nvs, TS_KEY_CTRL_URL, s_cfg.control_url, &len) != ESP_OK) {
        snprintf(s_cfg.control_url, sizeof(s_cfg.control_url), "%s", "https://headscale.local");
    }

    nvs_close(nvs);
}

static esp_err_t ts_start_with_authkey(const char *authkey, bool print_messages) {
    if (!authkey || !authkey[0]) return ESP_ERR_INVALID_ARG;
    if (!s_cfg.enabled) return ESP_ERR_INVALID_STATE;
    if (!wifi_mgr_is_connected()) return ESP_ERR_INVALID_STATE;
    if (s_cfg.backend != TS_BACKEND_SAAS) return ESP_ERR_NOT_SUPPORTED;

    ts_stop_session("restart-before-up");

    snprintf(s_runtime_authkey, sizeof(s_runtime_authkey), "%s", authkey);

    microlink_config_t cfg = {};
    cfg.auth_key = s_runtime_authkey;
    cfg.device_name = hostname_mgr_get();
    cfg.enable_derp = true;
    cfg.enable_stun = true;
    cfg.enable_disco = true;
    cfg.max_peers = 16;
    cfg.wifi_tx_power_dbm = 0;

    s_ml = microlink_init(&cfg);
    if (!s_ml) {
        s_start_fail++;
        ts_sync_state();
        return ESP_FAIL;
    }

    microlink_set_state_callback(s_ml, ts_on_state_change, NULL);
    esp_err_t err = microlink_start(s_ml);
    if (err != ESP_OK) {
        s_start_fail++;
        microlink_destroy(s_ml);
        s_ml = NULL;
        ts_sync_state();
        return err;
    }

    s_start_ok++;
    s_session_armed = true;
    ts_sync_state();

    if (print_messages) {
        shell_print("\r\n  tailscale connecting...");
        shell_print("\r\n  hostname: ");
        shell_print(hostname_mgr_get());
        shell_print("\r\n");
    }

    return ESP_OK;
}

static void ts_print_peer_list(void) {
    if (!s_ml) {
        ts_println("tailscale disconnected");
        return;
    }

    const int count = microlink_get_peer_count(s_ml);
    char line[192];
    snprintf(line, sizeof(line), "peers: %d", count);
    ts_println(line);

    for (int i = 0; i < count; i++) {
        microlink_peer_info_t info = {};
        if (microlink_get_peer_info(s_ml, i, &info) != ESP_OK) continue;
        char ip[16] = {0};
        microlink_ip_to_str(info.vpn_ip, ip);
        snprintf(line,
                 sizeof(line),
                 "%d) %s (%s) %s %s",
                 i + 1,
                 info.hostname[0] ? info.hostname : "(unnamed)",
                 ip,
                 info.online ? "online" : "offline",
                 info.direct_path ? "direct" : "derp");
        ts_println(line);
    }
}

static void ts_on_crypt_pass(const char *crypt_pass) {
    char secret[256] = {0};

    if (s_pending == TS_PENDING_SET_SECRET_SETUP_VAULT) {
        snprintf(s_pending_vault_pass, sizeof(s_pending_vault_pass), "%s", crypt_pass);
        s_pending = TS_PENDING_SET_SECRET_SETUP_VAULT_CONFIRM;
        shell_print("\r\n  Confirm password: ");
        shell_get_hidden_input(ts_on_crypt_pass);
        return;
    }

    if (s_pending == TS_PENDING_SET_SECRET_SETUP_VAULT_CONFIRM) {
        if (strcmp(s_pending_vault_pass, crypt_pass) != 0) {
            ts_println("vault passwords do not match");
            goto done;
        }

        esp_err_t err = secret_vault_password_set(s_pending_vault_pass);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "vault password set failed: %s", esp_err_to_name(err));
            ts_print_err("failed to set vault password", err);
            goto done;
        }

        err = secret_vault_store(s_pending_secret_key, s_pending_secret, s_pending_vault_pass);
        if (err == ESP_OK) {
            s_secret_store_ok++;
            ts_println("secret stored");
        } else {
            s_secret_store_fail++;
            ts_print_err("store failed", err);
        }
        goto done;
    }

    if (s_pending == TS_PENDING_SET_SECRET_STORE) {
        esp_err_t err = secret_vault_password_check(crypt_pass);
        if (err != ESP_OK) {
            ts_println("invalid vault password");
            goto done;
        }

        err = secret_vault_store(s_pending_secret_key, s_pending_secret, crypt_pass);
        if (err == ESP_OK) {
            s_secret_store_ok++;
            ts_println("secret stored");
        } else {
            s_secret_store_fail++;
            ts_print_err("store failed", err);
        }
        goto done;
    }

    if (s_pending == TS_PENDING_UP_VAULT) {
        s_up_requests++;

        if (!s_cfg.enabled) {
            ts_println("tailscale disabled");
            goto done;
        }
        if (!wifi_mgr_is_connected()) {
            ts_println("wifi disconnected");
            goto done;
        }
        if (s_cfg.backend != TS_BACKEND_SAAS) {
            ts_println("headscale backend not supported in this build");
            goto done;
        }

        esp_err_t err = secret_vault_load(TS_SECRET_AUTHKEY, crypt_pass, secret, sizeof(secret));
        if (err != ESP_OK) {
            s_secret_load_fail++;
            ts_println("invalid vault password or missing authkey");
            goto done;
        }

        s_secret_load_ok++;
        err = ts_start_with_authkey(secret, true);
        if (err != ESP_OK) {
            ts_print_err("tailscale start failed", err);
        }
        goto done;
    }

done:
    memset(secret, 0, sizeof(secret));
    memset(s_pending_secret, 0, sizeof(s_pending_secret));
    memset(s_pending_vault_pass, 0, sizeof(s_pending_vault_pass));
    s_pending_secret_key = NULL;
    s_pending = TS_PENDING_NONE;
}

static void ts_on_input(const char *value) {
    if (s_pending == TS_PENDING_SET_AUTHKEY) {
        snprintf(s_pending_secret, sizeof(s_pending_secret), "%s", value);
        s_pending_secret_key = TS_SECRET_AUTHKEY;
    } else if (s_pending == TS_PENDING_SET_TOKEN) {
        snprintf(s_pending_secret, sizeof(s_pending_secret), "%s", value);
        s_pending_secret_key = token_key_for_backend();
    } else {
        s_pending = TS_PENDING_NONE;
        return;
    }

    if (!secret_vault_password_is_set()) {
        s_pending = TS_PENDING_SET_SECRET_SETUP_VAULT;
        shell_print("\r\n  Vault password: ");
    } else {
        s_pending = TS_PENDING_SET_SECRET_STORE;
        shell_print("\r\n  Vault password: ");
    }
    shell_get_hidden_input(ts_on_crypt_pass);
}

esp_err_t tailscale_mgr_init(void) {
    secret_vault_init();
    ts_load_cfg();
    s_last_wifi_connected = wifi_mgr_is_connected();
    s_last_status_log_us = esp_timer_get_time();
    s_last_autoretry_us = 0;
    ts_sync_state();
    ts_log_status("init");
    return ESP_OK;
}

void tailscale_mgr_process_queue(void) {
    const bool wifi_connected = wifi_mgr_is_connected();
    if (wifi_connected != s_last_wifi_connected) {
        ESP_LOGI(TAG, "wifi link state changed: %s", wifi_connected ? "up" : "down");
        s_last_wifi_connected = wifi_connected;
    }

    if (!s_cfg.enabled || !wifi_connected) {
        if (s_ml) {
            ts_stop_session(!s_cfg.enabled ? "disabled" : "wifi-down");
        }
    } else if (!s_ml && s_session_armed && s_runtime_authkey[0] && s_cfg.backend == TS_BACKEND_SAAS) {
        const int64_t now_us = esp_timer_get_time();
        if (s_last_autoretry_us == 0 || (now_us - s_last_autoretry_us) >= TS_AUTORETRY_INTERVAL_US) {
            s_last_autoretry_us = now_us;
            esp_err_t err = ts_start_with_authkey(s_runtime_authkey, false);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "auto-restart failed: %s", esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG, "auto-restart queued");
            }
        }
    }

    ts_sync_state();

    const int64_t now_us = esp_timer_get_time();
    if (now_us - s_last_status_log_us >= TS_STATUS_LOG_INTERVAL_US) {
        ts_log_status("periodic");
        s_last_status_log_us = now_us;
    }
}

bool tailscale_mgr_is_enabled(void) {
    return s_cfg.enabled;
}

bool tailscale_mgr_is_connected(void) {
    return s_connected;
}

bool tailscale_mgr_is_connecting(void) {
    return s_connecting;
}

const char *tailscale_mgr_backend_name(void) {
    return s_cfg.backend == TS_BACKEND_SAAS ? "saas" : "headscale";
}

void tailscale_mgr_get_status_line(char *buf, size_t len) {
    ts_sync_state();

    if (!s_cfg.enabled) {
        snprintf(buf, len, "Tailscale: disabled");
        return;
    }

    if (!wifi_mgr_is_connected()) {
        snprintf(buf, len, "Tailscale: disconnected (wifi down)");
        return;
    }

    if (s_cfg.backend != TS_BACKEND_SAAS) {
        snprintf(buf, len, "Tailscale: backend %s not supported in this build", tailscale_mgr_backend_name());
        return;
    }

    if (!s_ml) {
        snprintf(buf, len, "Tailscale: disconnected (%s, %s)", tailscale_mgr_backend_name(), hostname_mgr_get());
        return;
    }

    if (s_connected) {
        uint32_t vpn_ip = microlink_get_vpn_ip(s_ml);
        char ip[16] = {0};
        if (vpn_ip) microlink_ip_to_str(vpn_ip, ip);
        snprintf(buf,
                 len,
                 "Tailscale: connected (%s, %s, ip %s, peers %d)",
                 tailscale_mgr_backend_name(),
                 hostname_mgr_get(),
                 vpn_ip ? ip : "pending",
                 microlink_get_peer_count(s_ml));
        return;
    }

    if (s_connecting) {
        snprintf(buf,
                 len,
                 "Tailscale: connecting (%s, %s, state %s)",
                 tailscale_mgr_backend_name(),
                 hostname_mgr_get(),
                 ml_state_name(s_ml_state));
        return;
    }

    snprintf(buf,
             len,
             "Tailscale: disconnected (%s, %s, state %s)",
             tailscale_mgr_backend_name(),
             hostname_mgr_get(),
             ml_state_name(s_ml_state));
}

void cmd_tailscale(int argc, char **argv) {
    if (argc < 2) {
        shell_print("\r\n  usage: tailscale enable|disable|backend [saas|headscale]|set tailnet <name>|set control-url <url>|set authkey|set token|up|down|status|devices|clear");
        shell_print("\r\n  defaults: backend (");
        shell_print(tailscale_mgr_backend_name());
        shell_print(")");
        if (s_cfg.tailnet[0]) {
            shell_print(" tailnet (");
            shell_print(s_cfg.tailnet);
            shell_print(")");
        }
        if (s_cfg.control_url[0]) {
            shell_print(" control-url (");
            shell_print(s_cfg.control_url);
            shell_print(")");
        }
        shell_print("\r\n");
        return;
    }

    if (strcmp(argv[1], "enable") == 0) {
        s_cfg.enabled = true;
        ts_save_cfg();
        ts_log_status("cmd-enable");
        ts_println("tailscale enabled");
        return;
    }

    if (strcmp(argv[1], "disable") == 0) {
        s_cfg.enabled = false;
        s_session_armed = false;
        ts_stop_session("cmd-disable");
        ts_save_cfg();
        ts_log_status("cmd-disable");
        ts_println("tailscale disabled");
        return;
    }

    if (strcmp(argv[1], "backend") == 0) {
        if (argc < 3) {
            shell_print("\r\n  backend (");
            shell_print(tailscale_mgr_backend_name());
            shell_print(")\r\n");
            return;
        }

        if (strcmp(argv[2], "saas") == 0) s_cfg.backend = TS_BACKEND_SAAS;
        else if (strcmp(argv[2], "headscale") == 0) s_cfg.backend = TS_BACKEND_HEADSCALE;
        else {
            shell_print("\r\n  usage: tailscale backend [saas|headscale]\r\n");
            return;
        }

        ts_save_cfg();
        ts_log_status("cmd-backend");
        shell_print("\r\n  backend set to ");
        shell_print(tailscale_mgr_backend_name());
        if (s_cfg.backend == TS_BACKEND_HEADSCALE) {
            shell_print(" (up currently saas-only)");
        }
        shell_print("\r\n");
        return;
    }

    if (strcmp(argv[1], "set") == 0) {
        if (argc < 3) {
            shell_print("\r\n  usage: tailscale set tailnet <name>|control-url <url>|authkey|token\r\n");
            return;
        }

        if (strcmp(argv[2], "tailnet") == 0) {
            if (argc < 4) {
                shell_print("\r\n  tailnet (");
                shell_print(s_cfg.tailnet[0] ? s_cfg.tailnet : "unset");
                shell_print(")\r\n");
                return;
            }
            strncpy(s_cfg.tailnet, argv[3], sizeof(s_cfg.tailnet) - 1);
            s_cfg.tailnet[sizeof(s_cfg.tailnet) - 1] = '\0';
            ts_save_cfg();
            ts_log_status("cmd-tailnet");
            ts_println("tailnet set");
            return;
        }

        if (strcmp(argv[2], "control-url") == 0) {
            if (argc < 4) {
                shell_print("\r\n  control-url (");
                shell_print(s_cfg.control_url);
                shell_print(")\r\n");
                return;
            }
            strncpy(s_cfg.control_url, argv[3], sizeof(s_cfg.control_url) - 1);
            s_cfg.control_url[sizeof(s_cfg.control_url) - 1] = '\0';
            ts_save_cfg();
            ts_log_status("cmd-control-url");
            ts_println("control-url set");
            return;
        }

        if (strcmp(argv[2], "authkey") == 0) {
            s_pending = TS_PENDING_SET_AUTHKEY;
            shell_print("\r\n  authkey: ");
            shell_get_input(ts_on_input);
            return;
        }

        if (strcmp(argv[2], "token") == 0) {
            s_pending = TS_PENDING_SET_TOKEN;
            shell_print("\r\n  token: ");
            shell_get_input(ts_on_input);
            return;
        }

        if (strncmp(argv[2], "wg-", 3) == 0) {
            ts_println("wg options removed; microlink manages wireguard internally");
            return;
        }

        shell_print("\r\n  usage: tailscale set tailnet <name>|control-url <url>|authkey|token\r\n");
        return;
    }

    if (strcmp(argv[1], "up") == 0) {
        s_pending = TS_PENDING_UP_VAULT;
        shell_print("\r\n  Vault password: ");
        shell_get_hidden_input(ts_on_crypt_pass);
        return;
    }

    if (strcmp(argv[1], "down") == 0) {
        s_session_armed = false;
        ts_stop_session("cmd-down");
        ts_log_status("cmd-down");
        ts_println("tailscale disconnected");
        return;
    }

    if (strcmp(argv[1], "status") == 0) {
        char line[192];
        tailscale_mgr_get_status_line(line, sizeof(line));
        shell_print("\r\n  ");
        shell_print(line);
        shell_print("\r\n  defaults: backend (");
        shell_print(tailscale_mgr_backend_name());
        shell_print(") tailnet (");
        shell_print(s_cfg.tailnet[0] ? s_cfg.tailnet : "unset");
        shell_print(") control-url (");
        shell_print(s_cfg.control_url[0] ? s_cfg.control_url : "unset");
        shell_print(")\r\n");
        return;
    }

    if (strcmp(argv[1], "devices") == 0) {
        ts_print_peer_list();
        return;
    }

    if (strcmp(argv[1], "clear") == 0) {
        ts_stop_session("cmd-clear");
        s_session_armed = false;
        memset(s_runtime_authkey, 0, sizeof(s_runtime_authkey));

        esp_err_t e1 = secret_vault_remove(TS_SECRET_AUTHKEY);
        esp_err_t e2 = secret_vault_remove(TS_SECRET_SAAS_TOKEN);
        esp_err_t e3 = secret_vault_remove(TS_SECRET_HEADSCALE_TOKEN);
        ESP_LOGI(TAG,
                 "clear secrets: authkey=%s saas=%s headscale=%s",
                 esp_err_to_name(e1),
                 esp_err_to_name(e2),
                 esp_err_to_name(e3));
        ts_println("cleared tailscale secrets");
        return;
    }

    shell_print("\r\n  unknown subcommand: ");
    shell_print(argv[1]);
    shell_print("\r\n");
}
