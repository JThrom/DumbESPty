#ifndef WIFI_MGR_HPP
#define WIFI_MGR_HPP

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_mgr_init(void);
esp_err_t wifi_mgr_scan(void);
bool     wifi_mgr_connect(const char *ssid, const char *password);
esp_err_t wifi_mgr_disconnect(void);
bool     wifi_mgr_is_connected(void);
const char *wifi_mgr_get_ssid(void);
void     wifi_mgr_get_status(char *buf, size_t len);
void     wifi_mgr_process_queue(void);
int      wifi_mgr_get_saved_ssids(char out[][33], int max_entries);
esp_err_t wifi_mgr_apply_hostname(const char *hostname);
esp_err_t wifi_mgr_get_mac(uint8_t mac[6]);
esp_err_t wifi_mgr_set_mac(const uint8_t mac[6]);

#ifdef __cplusplus
}
#endif

#endif
