/* Host test mock of ESP-IDF esp_err.h.
 * Minimal subset of error codes/types referenced by the firmware sources
 * under test. Not a faithful reproduction of the SDK. */
#ifndef MOCK_ESP_ERR_H
#define MOCK_ESP_ERR_H

#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK                  0
#define ESP_FAIL               -1
#define ESP_ERR_NO_MEM          0x101
#define ESP_ERR_INVALID_ARG     0x102
#define ESP_ERR_INVALID_STATE   0x103
#define ESP_ERR_INVALID_SIZE    0x104
#define ESP_ERR_NOT_FOUND       0x105
#define ESP_ERR_NOT_SUPPORTED   0x106
#define ESP_ERR_TIMEOUT         0x107
#define ESP_ERR_NVS_NOT_FOUND   0x1102

#ifdef __cplusplus
extern "C" {
#endif

const char *esp_err_to_name(esp_err_t code);

#ifdef __cplusplus
}
#endif

#endif
