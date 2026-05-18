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
void     wifi_mgr_get_status(char *buf, size_t len);
void     wifi_mgr_process_queue(void);

#ifdef __cplusplus
}
#endif

#endif
