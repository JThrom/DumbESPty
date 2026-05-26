#include "wifi_mgr.hpp"

#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>

#include "coex_manager.hpp"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "hostname_mgr.hpp"
#include "shell.hpp"
#include "secret_vault.hpp"

static const char *TAG = "wifi";

#define MAX_APS 32
#define WIFI_MSG_QUEUE_SIZE 8
#define CONNECT_TIMEOUT_MS 15000
#define CONNECT_RETRY_TIMEOUT_MS 18000
#define CONNECT_POLL_MS 200
#define MAX_WIFI_PROFILES 16
#define WIFI_PROFILE_NS "wificfg"
#define WIFI_PROFILE_KEY "ssids"

typedef struct {
    char text[80];
} wifi_msg_t;

typedef struct {
    uint8_t count;
    char ssids[MAX_WIFI_PROFILES][33];
} wifi_profiles_t;

static bool initialized = false;
static bool scanning = false;
static bool connected = false;
static char scan_results[MAX_APS][33];
static int scan_count = 0;
static char current_ssid[33] = "";
static char connect_ssid[33] = "";

static QueueHandle_t wifi_msg_queue = NULL;
static bool connect_success = false;
static char connect_ip[16] = "";
static esp_netif_t *s_sta_netif = NULL;

static wifi_profiles_t s_profiles = {};

enum wifi_pending_action_t {
    WIFI_PENDING_NONE = 0,
    WIFI_PENDING_CONNECT_WIFI_PASS,
    WIFI_PENDING_CONNECT_VAULT,
    WIFI_PENDING_CONNECT_SETUP_VAULT,
    WIFI_PENDING_CONNECT_SETUP_VAULT_CONFIRM,
    WIFI_PENDING_CONNECT_STORE_VAULT,
};

static wifi_pending_action_t s_pending_action = WIFI_PENDING_NONE;
static char s_pending_ssid[33] = "";
static char s_pending_secret[256] = "";
static char s_pending_vault_pass[128] = "";

static void copy_ssid_trimmed(const char *src, char *dst, size_t dst_len) {
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    if (!src) return;

    size_t n = strnlen(src, dst_len - 1);
    while (n > 0 && isspace((unsigned char)src[n - 1])) n--;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void vault_key_for_ssid_v1(const char *ssid, char *out, size_t out_len) {
    snprintf(out, out_len, "wifi/%s", ssid);
}

static bool check_current_ip(void) {
    if (!s_sta_netif) return false;
    esp_netif_ip_info_t ip_info = {};
    if (esp_netif_get_ip_info(s_sta_netif, &ip_info) != ESP_OK) return false;
    if (ip_info.ip.addr == 0) return false;
    snprintf(connect_ip, sizeof(connect_ip), IPSTR, IP2STR(&ip_info.ip));
    connect_success = true;
    return true;
}

static bool wait_for_ip_or_connected(int timeout_ms, const char *phase) {
    const int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline_us) {
        if (connect_success || check_current_ip()) {
            return true;
        }
        if (!connected) {
            vTaskDelay(pdMS_TO_TICKS(CONNECT_POLL_MS));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(CONNECT_POLL_MS));
    }
    ESP_LOGW(TAG, "%s timeout; connected=%d ip=%s", phase, connected ? 1 : 0, connect_ip[0] ? connect_ip : "none");
    return connect_success || check_current_ip();
}

static void load_profiles(void) {
    memset(&s_profiles, 0, sizeof(s_profiles));
    nvs_handle_t nvs = 0;
    if (nvs_open(WIFI_PROFILE_NS, NVS_READONLY, &nvs) != ESP_OK) return;
    size_t len = sizeof(s_profiles);
    esp_err_t err = nvs_get_blob(nvs, WIFI_PROFILE_KEY, &s_profiles, &len);
    nvs_close(nvs);
    if (err != ESP_OK || len != sizeof(s_profiles) || s_profiles.count > MAX_WIFI_PROFILES) {
        memset(&s_profiles, 0, sizeof(s_profiles));
    }
}

static esp_err_t save_profiles(void) {
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(WIFI_PROFILE_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(nvs, WIFI_PROFILE_KEY, &s_profiles, sizeof(s_profiles));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static int find_profile_index(const char *ssid) {
    for (int i = 0; i < s_profiles.count; i++) {
        if (strcmp(s_profiles.ssids[i], ssid) == 0) return i;
    }
    return -1;
}

static esp_err_t remember_ssid(const char *ssid) {
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    if (find_profile_index(ssid) >= 0) return ESP_OK;
    if (s_profiles.count >= MAX_WIFI_PROFILES) {
        for (int i = 1; i < MAX_WIFI_PROFILES; i++) {
            snprintf(s_profiles.ssids[i - 1], sizeof(s_profiles.ssids[i - 1]), "%s", s_profiles.ssids[i]);
        }
        s_profiles.count = MAX_WIFI_PROFILES - 1;
    }
    snprintf(s_profiles.ssids[s_profiles.count], sizeof(s_profiles.ssids[s_profiles.count]), "%s", ssid);
    s_profiles.count++;
    return save_profiles();
}

static esp_err_t forget_ssid_profile(const char *ssid) {
    const int idx = find_profile_index(ssid);
    if (idx < 0) return ESP_ERR_NOT_FOUND;
    for (int i = idx + 1; i < s_profiles.count; i++) {
        snprintf(s_profiles.ssids[i - 1], sizeof(s_profiles.ssids[i - 1]), "%s", s_profiles.ssids[i]);
    }
    memset(s_profiles.ssids[s_profiles.count - 1], 0, sizeof(s_profiles.ssids[s_profiles.count - 1]));
    s_profiles.count--;
    return save_profiles();
}

static void on_crypt_pass_input(const char *crypt_pass);

static void vault_key_for_ssid(const char *ssid, char *out, size_t out_len) {
    uint32_t h = 2166136261u;
    const uint8_t *p = (const uint8_t *)ssid;
    while (*p) {
        h ^= *p++;
        h *= 16777619u;
    }
    snprintf(out, out_len, "wf%08x", (unsigned)h);
}

static void dump_scan_results(void) {
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > MAX_APS) ap_count = MAX_APS;

    wifi_ap_record_t *aps = (wifi_ap_record_t *)malloc(ap_count * sizeof(wifi_ap_record_t));
    if (!aps) {
        shell_print("\r\n  scan: malloc failed");
        scanning = false;
        return;
    }

    esp_err_t err = esp_wifi_scan_get_ap_records(&ap_count, aps);
    if (err != ESP_OK) {
        shell_print("\r\n  scan get failed");
        free(aps);
        scanning = false;
        return;
    }

    scan_count = ap_count;
    char buf[256];
    for (int i = 0; i < ap_count; i++) {
        snprintf(scan_results[i], sizeof(scan_results[i]), "%s", (const char *)aps[i].ssid);
        int rssi = aps[i].rssi;
        const char *auth = "";
        switch (aps[i].authmode) {
            case WIFI_AUTH_OPEN: auth = "OPEN"; break;
            case WIFI_AUTH_WEP: auth = "WEP"; break;
            case WIFI_AUTH_WPA_PSK: auth = "WPA"; break;
            case WIFI_AUTH_WPA2_PSK: auth = "WPA2"; break;
            case WIFI_AUTH_WPA_WPA2_PSK: auth = "WPA/WPA2"; break;
            case WIFI_AUTH_WPA2_ENTERPRISE: auth = "WPA2-E"; break;
            case WIFI_AUTH_WPA3_PSK: auth = "WPA3"; break;
            case WIFI_AUTH_WPA2_WPA3_PSK: auth = "WPA2/WPA3"; break;
            default: auth = "?"; break;
        }
        snprintf(buf, sizeof(buf), "\r\n  %.32s  %-8s  %d dBm", scan_results[i], auth, rssi);
        shell_print(buf);
    }
    free(aps);
    scanning = false;
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA started");
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        connected = true;
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        connected = false;
        connect_success = false;
        coex_release();
        connect_ip[0] = '\0';
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)data;
        if (disc) {
            ESP_LOGW(TAG, "STA disconnected reason=%u", (unsigned)disc->reason);
        }
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        snprintf(connect_ip, sizeof(connect_ip), IPSTR, IP2STR(&evt->ip_info.ip));
        connect_success = true;
        ESP_LOGI(TAG, "Got IP %s", connect_ip);
        return;
    }
}

esp_err_t wifi_mgr_init(void) {
    if (initialized) return ESP_OK;

    load_profiles();
    secret_vault_init();

    wifi_msg_queue = xQueueCreate(WIFI_MSG_QUEUE_SIZE, sizeof(wifi_msg_t));
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK) { ESP_LOGE(TAG, "netif init: %s", esp_err_to_name(ret)); return ret; }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(ret)); return ret;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "wifi init: %s", esp_err_to_name(ret)); return ret; }

    ret = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "reg wifi event: %s", esp_err_to_name(ret)); return ret; }

    ret = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "reg got ip: %s", esp_err_to_name(ret)); return ret; }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif) {
        ESP_LOGE(TAG, "create default STA netif failed");
        return ESP_FAIL;
    }

    wifi_mgr_apply_hostname(hostname_mgr_get());

    wifi_country_t country = {};
    strcpy(country.cc, "US");
    country.schan = 1;
    country.nchan = 11;
    country.policy = WIFI_COUNTRY_POLICY_AUTO;
    esp_wifi_set_country(&country);

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "set mode: %s", esp_err_to_name(ret)); return ret; }

    ret = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "set storage: %s", esp_err_to_name(ret)); return ret; }

    ret = esp_wifi_start();
    if (ret != ESP_OK) { ESP_LOGE(TAG, "start: %s", esp_err_to_name(ret)); return ret; }

    esp_wifi_set_ps(WIFI_PS_NONE);

    initialized = true;
    ESP_LOGI(TAG, "WiFi initialized (STA mode)");
    return ESP_OK;
}

esp_err_t wifi_mgr_apply_hostname(const char *hostname) {
    if (!hostname || !hostname[0]) return ESP_ERR_INVALID_ARG;
    if (!s_sta_netif) return ESP_ERR_INVALID_STATE;
    return esp_netif_set_hostname(s_sta_netif, hostname);
}

void wifi_mgr_process_queue(void) {
    wifi_msg_t msg;
    while (xQueueReceive(wifi_msg_queue, &msg, 0) == pdTRUE) {
        shell_print(msg.text);
    }
}

esp_err_t wifi_mgr_scan(void) {
    if (scanning) {
        shell_print("\r\n  scan already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    scanning = true;
    shell_print("\r\n  scanning...");
    wifi_scan_config_t conf = {};
    conf.show_hidden = false;
    conf.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    esp_err_t ret = esp_wifi_scan_start(&conf, true);
    if (ret != ESP_OK) {
        scanning = false;
        shell_print("\r\n  scan failed");
        return ret;
    }
    dump_scan_results();
    return ESP_OK;
}

bool wifi_mgr_connect(const char *ssid, const char *password) {
    wifi_config_t cfg = {};
    snprintf((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), "%s", ssid);
    if (password) snprintf((char *)cfg.sta.password, sizeof(cfg.sta.password), "%s", password);
    cfg.sta.threshold.authmode = (password && password[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (ret != ESP_OK) return false;

    snprintf(current_ssid, sizeof(current_ssid), "%s", ssid);

    shell_print("\r\n  connecting to ");
    shell_print(ssid);

    coex_acquire();

    connect_success = false;
    connect_ip[0] = '\0';

    if (connected) {
        esp_wifi_disconnect();
        connected = false;
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        shell_print("\r\n  connect failed\r\n");
        coex_release();
        return false;
    }

    char buf[64];
    if (wait_for_ip_or_connected(CONNECT_TIMEOUT_MS, "connect phase1")) {
        if (connect_ip[0]) {
            snprintf(buf, sizeof(buf), "\r\n  IP: %s", connect_ip);
            shell_print(buf);
            shell_print("\r\n");
        }
        coex_release();
        return true;
    }

    shell_print("\r\n  retrying...");
    shell_print("\r\n");
    connect_success = false;
    connect_ip[0] = '\0';
    ret = esp_wifi_connect();
    if (ret == ESP_OK && wait_for_ip_or_connected(CONNECT_RETRY_TIMEOUT_MS, "connect phase2")) {
        if (connect_ip[0]) {
            snprintf(buf, sizeof(buf), "\r\n  IP: %s", connect_ip);
            shell_print(buf);
            shell_print("\r\n");
        }
        coex_release();
        return true;
    }

    shell_print("\r\n  connection timeout\r\n");
    esp_wifi_disconnect();
    coex_release();
    return false;
}

static void on_password_input(const char *password) {
    if (s_pending_action != WIFI_PENDING_CONNECT_WIFI_PASS) return;

    snprintf(s_pending_secret, sizeof(s_pending_secret), "%s", password);
    if (!secret_vault_password_is_set()) {
        s_pending_action = WIFI_PENDING_CONNECT_SETUP_VAULT;
        shell_print("\r\n  Vault password: ");
        shell_get_hidden_input(on_crypt_pass_input);
        return;
    }

    s_pending_action = WIFI_PENDING_CONNECT_STORE_VAULT;
    shell_print("\r\n  Vault password: ");
    shell_get_hidden_input(on_crypt_pass_input);
}

static void on_crypt_pass_input(const char *crypt_pass) {
    if (s_pending_action == WIFI_PENDING_CONNECT_SETUP_VAULT) {
        snprintf(s_pending_vault_pass, sizeof(s_pending_vault_pass), "%s", crypt_pass);
        s_pending_action = WIFI_PENDING_CONNECT_SETUP_VAULT_CONFIRM;
        shell_print("\r\n  Confirm password: ");
        shell_get_hidden_input(on_crypt_pass_input);
        return;
    }

    if (s_pending_action == WIFI_PENDING_CONNECT_SETUP_VAULT_CONFIRM) {
        if (strcmp(s_pending_vault_pass, crypt_pass) != 0) {
            shell_print("\r\n  vault passwords do not match\r\n");
            s_pending_action = WIFI_PENDING_NONE;
            s_pending_ssid[0] = '\0';
            memset(s_pending_secret, 0, sizeof(s_pending_secret));
            memset(s_pending_vault_pass, 0, sizeof(s_pending_vault_pass));
            return;
        }
        if (secret_vault_password_set(s_pending_vault_pass) != ESP_OK) {
            shell_print("\r\n  failed to set vault password\r\n");
            s_pending_action = WIFI_PENDING_NONE;
            s_pending_ssid[0] = '\0';
            memset(s_pending_secret, 0, sizeof(s_pending_secret));
            memset(s_pending_vault_pass, 0, sizeof(s_pending_vault_pass));
            return;
        }
        snprintf((char *)connect_ssid, sizeof(connect_ssid), "%s", s_pending_ssid);
        char vault_key[64];
        vault_key_for_ssid(s_pending_ssid, vault_key, sizeof(vault_key));
        if (secret_vault_store(vault_key, s_pending_secret, s_pending_vault_pass) == ESP_OK) {
            remember_ssid(s_pending_ssid);
            wifi_mgr_connect(connect_ssid, s_pending_secret);
        } else {
            shell_print("\r\n  save failed\r\n");
        }
        memset(s_pending_vault_pass, 0, sizeof(s_pending_vault_pass));
        memset(s_pending_secret, 0, sizeof(s_pending_secret));
        s_pending_action = WIFI_PENDING_NONE;
        s_pending_ssid[0] = '\0';
        return;
    }

    if (s_pending_action == WIFI_PENDING_CONNECT_STORE_VAULT) {
        if (secret_vault_password_check(crypt_pass) != ESP_OK) {
            shell_print("\r\n  invalid vault password\r\n");
            memset(s_pending_secret, 0, sizeof(s_pending_secret));
            s_pending_action = WIFI_PENDING_NONE;
            s_pending_ssid[0] = '\0';
            return;
        }
        snprintf((char *)connect_ssid, sizeof(connect_ssid), "%s", s_pending_ssid);
        char vault_key[64];
        vault_key_for_ssid(s_pending_ssid, vault_key, sizeof(vault_key));
        if (secret_vault_store(vault_key, s_pending_secret, crypt_pass) == ESP_OK) {
            remember_ssid(s_pending_ssid);
            wifi_mgr_connect(connect_ssid, s_pending_secret);
        } else {
            shell_print("\r\n  save failed\r\n");
        }
        memset(s_pending_secret, 0, sizeof(s_pending_secret));
        s_pending_action = WIFI_PENDING_NONE;
        s_pending_ssid[0] = '\0';
        return;
    }

    if (s_pending_action != WIFI_PENDING_CONNECT_VAULT) {
        s_pending_action = WIFI_PENDING_NONE;
        return;
    }
    char vault_key[64];
    char legacy_vault_key[64];
    char pass[256] = {0};
    vault_key_for_ssid(s_pending_ssid, vault_key, sizeof(vault_key));
    vault_key_for_ssid_v1(s_pending_ssid, legacy_vault_key, sizeof(legacy_vault_key));
    esp_err_t err = secret_vault_load(vault_key, crypt_pass, pass, sizeof(pass));
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = secret_vault_load(legacy_vault_key, crypt_pass, pass, sizeof(pass));
    }
    if (err != ESP_OK) {
        shell_print("\r\n  invalid vault password or missing secret\r\n");
    } else {
        snprintf(connect_ssid, sizeof(connect_ssid), "%s", s_pending_ssid);
        wifi_mgr_connect(connect_ssid, pass);
    }
    memset(pass, 0, sizeof(pass));
    memset(s_pending_secret, 0, sizeof(s_pending_secret));
    s_pending_action = WIFI_PENDING_NONE;
    s_pending_ssid[0] = '\0';
}

esp_err_t wifi_mgr_disconnect(void) {
    shell_print("\r\n  disconnecting...\r\n");
    esp_err_t ret = esp_wifi_disconnect();
    if (ret == ESP_OK) {
        connected = false;
        current_ssid[0] = '\0';
    }
    return ret;
}

bool wifi_mgr_is_connected(void) {
    return connected;
}

const char *wifi_mgr_get_ssid(void) {
    return current_ssid;
}

void wifi_mgr_get_status(char *buf, size_t len) {
    if (connected) {
        snprintf(buf, len, "connected to %s", current_ssid);
    } else {
        snprintf(buf, len, "disconnected");
    }
}

int wifi_mgr_get_saved_ssids(char out[][33], int max_entries) {
    if (!out || max_entries <= 0) return 0;
    int n = s_profiles.count < max_entries ? s_profiles.count : max_entries;
    for (int i = 0; i < n; i++) {
        snprintf(out[i], 33, "%s", s_profiles.ssids[i]);
    }
    return n;
}

void cmd_wifi(int argc, char **argv) {
    if (argc < 2) {
        shell_print("\r\n  usage: wifi scan|status|list|connect <ssid>|disconnect|forget <ssid>\r\n");
        return;
    }
    if (strcmp(argv[1], "scan") == 0) {
        wifi_mgr_scan();
    } else if (strcmp(argv[1], "status") == 0) {
        char buf[64];
        wifi_mgr_get_status(buf, sizeof(buf));
        shell_print("\r\n  ");
        shell_print(buf);
        shell_print("\r\n");
    } else if (strcmp(argv[1], "list") == 0) {
        shell_print("\r\n  saved SSIDs:");
        if (s_profiles.count == 0) {
            shell_print("\r\n    (none)");
        } else {
            for (int i = 0; i < s_profiles.count; i++) {
                shell_print("\r\n    ");
                shell_print(s_profiles.ssids[i]);
            }
        }
        shell_print("\r\n");
    } else if (strcmp(argv[1], "connect") == 0) {
        if (argc < 3) {
            shell_print("\r\n  usage: wifi connect <ssid>\r\n");
            shell_print("\r\n  tip: use TAB for saved SSID completion\r\n");
            return;
        }
        if (argc >= 4) {
            shell_print("\r\n  usage: wifi connect <ssid>\r\n");
            return;
        }

        char ssid[33];
        copy_ssid_trimmed(argv[2], ssid, sizeof(ssid));
        if (!ssid[0]) {
            shell_print("\r\n  usage: wifi connect <ssid>\r\n");
            return;
        }

        char vault_key[64];
        char legacy_vault_key[64];
        vault_key_for_ssid(ssid, vault_key, sizeof(vault_key));
        vault_key_for_ssid_v1(ssid, legacy_vault_key, sizeof(legacy_vault_key));
        const bool has_profile = find_profile_index(ssid) >= 0;
        const bool has_secret = secret_vault_exists(vault_key) || secret_vault_exists(legacy_vault_key);
        if (has_profile && has_secret) {
            snprintf(s_pending_ssid, sizeof(s_pending_ssid), "%s", ssid);
            s_pending_action = WIFI_PENDING_CONNECT_VAULT;
            shell_print("\r\n  Vault password: ");
            shell_get_hidden_input(on_crypt_pass_input);
            return;
        }

        snprintf(s_pending_ssid, sizeof(s_pending_ssid), "%s", ssid);
        shell_print("\r\n  wifi password: ");
        s_pending_action = WIFI_PENDING_CONNECT_WIFI_PASS;
        shell_get_hidden_input(on_password_input);
    } else if (strcmp(argv[1], "disconnect") == 0) {
        wifi_mgr_disconnect();
    } else if (strcmp(argv[1], "forget") == 0) {
        if (argc < 3) {
            shell_print("\r\n  usage: wifi forget <ssid>\r\n");
            return;
        }
        char ssid[33];
        copy_ssid_trimmed(argv[2], ssid, sizeof(ssid));
        if (!ssid[0]) {
            shell_print("\r\n  usage: wifi forget <ssid>\r\n");
            return;
        }
        char vault_key[64];
        char legacy_vault_key[64];
        vault_key_for_ssid(ssid, vault_key, sizeof(vault_key));
        vault_key_for_ssid_v1(ssid, legacy_vault_key, sizeof(legacy_vault_key));
        secret_vault_remove(vault_key);
        secret_vault_remove(legacy_vault_key);
        forget_ssid_profile(ssid);
        shell_print("\r\n  forgot ");
        shell_print(ssid);
        shell_print("\r\n");
    } else {
        shell_print("\r\n  unknown subcommand: ");
        shell_print(argv[1]);
        shell_print("\r\n");
    }
}
