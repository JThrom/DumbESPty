/* Host test mock of ESP-IDF nvs.h.
 * Backed by a tiny in-memory key/value store implemented in
 * stubs_esp.cpp so persistence logic under test exercises real code paths
 * (open/get/set/commit/close) deterministically on host. */
#ifndef MOCK_NVS_H
#define MOCK_NVS_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t nvs_handle_t;

typedef enum {
    NVS_READONLY,
    NVS_READWRITE,
} nvs_open_mode_t;

typedef struct {
    char namespace_name[16];
    char key[16];
    uint8_t type;
} nvs_entry_info_t;

typedef enum {
    NVS_TYPE_U8 = 0x01,
    NVS_TYPE_STR = 0x21,
    NVS_TYPE_BLOB = 0x42,
    NVS_TYPE_ANY = 0xff,
} nvs_type_t;

#define NVS_DEFAULT_PART_NAME "nvs"

typedef struct nvs_iterator_opaque *nvs_iterator_t;

esp_err_t nvs_entry_find(const char *part_name, const char *namespace_name,
                         nvs_type_t type, nvs_iterator_t *output_iterator);
esp_err_t nvs_entry_next(nvs_iterator_t *iterator);
void nvs_entry_info(nvs_iterator_t iterator, nvs_entry_info_t *out_info);
void nvs_release_iterator(nvs_iterator_t iterator);

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out_handle);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_commit(nvs_handle_t handle);

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value);
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out_value, size_t *length);

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value);
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out_value);

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length);

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key);
esp_err_t nvs_erase_all(nvs_handle_t handle);

/* Test-only: wipe the in-memory store between tests. */
void mock_nvs_reset(void);

#ifdef __cplusplus
}
#endif

#endif
