#include "hostname_mgr.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>

#include "esp_err.h"
#include "esp_mac.h"
#include "nvs.h"
#include "shell.hpp"
#include "wifi_mgr.hpp"

static constexpr char HOST_NS[] = "devicecfg";
static constexpr char HOST_KEY[] = "hostname";
static constexpr size_t HOST_MAX_LEN = 63;

static char s_hostname[HOST_MAX_LEN + 1] = {0};
static bool s_init = false;

static bool hostname_is_valid(const char *name) {
    if (!name || !name[0]) return false;
    const size_t n = strlen(name);
    if (n > HOST_MAX_LEN) return false;
    if (name[0] == '-' || name[n - 1] == '-') return false;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)name[i];
        if (!(std::isalnum(c) || c == '-')) return false;
    }
    return true;
}

static void hostname_to_lower(char *dst, size_t len, const char *src) {
    if (!dst || len == 0) return;
    size_t i = 0;
    for (; src && src[i] && i < len - 1; i++) {
        dst[i] = (char)std::tolower((unsigned char)src[i]);
    }
    dst[i] = '\0';
}

static void build_default_hostname(char *out, size_t out_len) {
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    snprintf(out,
             out_len,
             "dumbespty-%02x%02x%02x%02x",
             mac[2],
             mac[3],
             mac[4],
             mac[5]);
}

static esp_err_t save_hostname(const char *hostname) {
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(HOST_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_str(nvs, HOST_KEY, hostname);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t hostname_mgr_init(void) {
    if (s_init) return ESP_OK;

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(HOST_NS, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        size_t len = sizeof(s_hostname);
        err = nvs_get_str(nvs, HOST_KEY, s_hostname, &len);
        nvs_close(nvs);
    }

    if (err != ESP_OK || !hostname_is_valid(s_hostname)) {
        char def[HOST_MAX_LEN + 1] = {0};
        build_default_hostname(def, sizeof(def));
        hostname_to_lower(s_hostname, sizeof(s_hostname), def);
        save_hostname(s_hostname);
    } else {
        char normalized[HOST_MAX_LEN + 1] = {0};
        hostname_to_lower(normalized, sizeof(normalized), s_hostname);
        snprintf(s_hostname, sizeof(s_hostname), "%s", normalized);
    }

    s_init = true;
    return ESP_OK;
}

const char *hostname_mgr_get(void) {
    if (!s_init) hostname_mgr_init();
    return s_hostname;
}

esp_err_t hostname_mgr_set(const char *hostname) {
    if (!hostname) return ESP_ERR_INVALID_ARG;
    char normalized[HOST_MAX_LEN + 1] = {0};
    hostname_to_lower(normalized, sizeof(normalized), hostname);
    if (!hostname_is_valid(normalized)) return ESP_ERR_INVALID_ARG;

    esp_err_t err = save_hostname(normalized);
    if (err != ESP_OK) return err;

    snprintf(s_hostname, sizeof(s_hostname), "%s", normalized);
    wifi_mgr_apply_hostname(s_hostname);
    return ESP_OK;
}

void cmd_hostname(int argc, char **argv) {
    if (argc == 1) {
        shell_print("\r\n  ");
        shell_print(hostname_mgr_get());
        return;
    }
    if (argc == 3 && strcmp(argv[1], "set") == 0) {
        const esp_err_t err = hostname_mgr_set(argv[2]);
        if (err != ESP_OK) {
            shell_print("\r\n  invalid hostname");
            shell_print("\r\n  allowed: [a-z0-9-], max 63, no leading/trailing '-' ");
            return;
        }
        shell_print("\r\n  hostname set: ");
        shell_print(hostname_mgr_get());
        return;
    }
    shell_print("\r\n  usage: hostname | hostname set <hostname>");
}
