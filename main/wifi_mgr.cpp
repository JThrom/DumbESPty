#include "wifi_mgr.hpp"
#include "shell.hpp"
#include "coex_manager.hpp"
#include <cstring>
#include <cstdio>
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

extern "C" bool esp_wifi_skip_supp_pmkcaching(void) {
    return true;
}


static const char *TAG = "wifi";

#define MAX_APS 32
#define WIFI_MSG_QUEUE_SIZE 8
#define CONNECT_TIMEOUT_MS 15000

static bool initialized = false;
static bool scanning = false;
static bool connected = false;
static char scan_results[MAX_APS][33];
static int scan_count = 0;
static char current_ssid[33] = "";
static char connect_ssid[33] = "";

static QueueHandle_t wifi_msg_queue = NULL;
static SemaphoreHandle_t connect_sem = NULL;
static bool connect_success = false;
static char connect_ip[16] = "";

typedef struct {
    char text[80];
} wifi_msg_t;

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
        strncpy(scan_results[i], (const char *)aps[i].ssid, 32);
        scan_results[i][32] = '\0';
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
        xSemaphoreGive(connect_sem);
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        snprintf(connect_ip, sizeof(connect_ip), IPSTR, IP2STR(&evt->ip_info.ip));
        connect_success = true;
        xSemaphoreGive(connect_sem);
        return;
    }
}

esp_err_t wifi_mgr_init(void) {
    if (initialized) return ESP_OK;

    wifi_msg_queue = xQueueCreate(WIFI_MSG_QUEUE_SIZE, sizeof(wifi_msg_t));
    connect_sem = xSemaphoreCreateBinary();

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

    esp_netif_create_default_wifi_sta();

    wifi_country_t country;
    memset(&country, 0, sizeof(country));
    strcpy(country.cc, "US");
    country.schan = 1;
    country.nchan = 11;
    country.policy = WIFI_COUNTRY_POLICY_AUTO;
    esp_wifi_set_country(&country);

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "set mode: %s", esp_err_to_name(ret)); return ret; }

    ret = esp_wifi_start();
    if (ret != ESP_OK) { ESP_LOGE(TAG, "start: %s", esp_err_to_name(ret)); return ret; }

    esp_wifi_set_ps(WIFI_PS_NONE);

    initialized = true;
    ESP_LOGI(TAG, "WiFi initialized (STA mode)");
    return ESP_OK;
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
    wifi_scan_config_t conf;
    memset(&conf, 0, sizeof(conf));
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
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    if (password) strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = (password && password[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (ret != ESP_OK) return false;

    strncpy(current_ssid, ssid, 32);
    current_ssid[32] = '\0';

    shell_print("\r\n  connecting to ");
    shell_print(ssid);

    coex_acquire();

    connect_success = false;
    xSemaphoreTake(connect_sem, 0);

    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        shell_print("\r\n  connect failed");
        coex_release();
        return false;
    }

    if (xSemaphoreTake(connect_sem, pdMS_TO_TICKS(CONNECT_TIMEOUT_MS)) == pdTRUE) {
        char buf[64];
        if (connect_success && connect_ip[0]) {
            snprintf(buf, sizeof(buf), "\r\n  IP: %s", connect_ip);
            shell_print(buf);
            shell_print("\r\n");
        }
        if (!connect_success) {
            shell_print("\r\n  retrying...");
            shell_print("\r\n");
            coex_acquire();
            connect_success = false;
            xSemaphoreTake(connect_sem, 0);
            ret = esp_wifi_connect();
            if (ret == ESP_OK) {
                xSemaphoreTake(connect_sem, pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
            }
            if (connect_success && connect_ip[0]) {
                snprintf(buf, sizeof(buf), "\r\n  IP: %s", connect_ip);
                shell_print(buf);
                shell_print("\r\n");
            }
        }
        coex_release();
        return connect_success;
    }

    shell_print("\r\n  connection timeout");
    esp_wifi_disconnect();
    coex_release();
    return false;
}

static void connect_with_password(const char *password) {
    wifi_mgr_connect(connect_ssid, password);
}

esp_err_t wifi_mgr_disconnect(void) {
    shell_print("\r\n  disconnecting...");
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

void wifi_mgr_get_status(char *buf, size_t len) {
    if (connected) {
        snprintf(buf, len, "connected to %s", current_ssid);
    } else {
        snprintf(buf, len, "disconnected");
    }
}

void cmd_wifi(int argc, char **argv) {
    if (argc < 2) {
        shell_print("\r\n  usage: wifi scan|status|connect <ssid>|disconnect");
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
    } else if (strcmp(argv[1], "connect") == 0) {
        if (argc < 3) {
            shell_print("\r\n  usage: wifi connect <ssid> [password]");
            return;
        }
        if (argc >= 4) {
            wifi_mgr_connect(argv[2], argv[3]);
        } else {
            strncpy(connect_ssid, argv[2], 32);
            connect_ssid[32] = '\0';
            shell_print("\r\n  password: ");
            shell_get_hidden_input(connect_with_password);
        }
    } else if (strcmp(argv[1], "disconnect") == 0) {
        wifi_mgr_disconnect();
    } else {
        shell_print("\r\n  unknown subcommand: ");
        shell_print(argv[1]);
    }
}
