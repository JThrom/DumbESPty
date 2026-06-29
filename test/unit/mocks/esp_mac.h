/* Host test mock of esp_mac.h. Returns a fixed deterministic MAC. */
#ifndef MOCK_ESP_MAC_H
#define MOCK_ESP_MAC_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t esp_efuse_mac_get_default(uint8_t *mac);
esp_err_t esp_read_mac(uint8_t *mac, int type);

/* Test-only: override the MAC the mock returns. */
void mock_set_default_mac(const uint8_t mac[6]);

#ifdef __cplusplus
}
#endif

#endif
