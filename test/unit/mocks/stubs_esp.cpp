/* Host-side implementations of mocked ESP-IDF facilities:
 *  - esp_err_to_name
 *  - a tiny in-memory NVS key/value store (namespaced)
 *  - heap_caps_* backed by malloc/free
 *  - esp_mac, esp_timer, esp_random
 * These let firmware persistence/allocation code run deterministically on host. */
#include "esp_err.h"
#include "nvs.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"

#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <string>
#include <map>
#include <vector>

/* ----------------------------- esp_err ----------------------------- */
extern "C" const char *esp_err_to_name(esp_err_t code) {
    switch (code) {
        case ESP_OK: return "ESP_OK";
        case ESP_FAIL: return "ESP_FAIL";
        case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
        case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
        case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
        case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
        case ESP_ERR_NVS_NOT_FOUND: return "ESP_ERR_NVS_NOT_FOUND";
        default: return "ESP_ERR_UNKNOWN";
    }
}

/* ------------------------------ NVS -------------------------------- */
namespace {
struct NvsEntry {
    std::vector<uint8_t> data;  // raw bytes (str stored with trailing NUL)
    bool is_str = false;
};
struct NvsHandle {
    std::string ns;
    bool writable = false;
    bool open = false;
};

std::map<std::string, std::map<std::string, NvsEntry>> g_store;  // ns -> key -> entry
std::vector<NvsHandle> g_handles;  // index+1 == handle id (0 reserved/invalid)
}

extern "C" void mock_nvs_reset(void) {
    g_store.clear();
    g_handles.clear();
}

extern "C" esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out_handle) {
    if (!name || !out_handle) return ESP_ERR_INVALID_ARG;
    if (mode == NVS_READONLY && g_store.find(name) == g_store.end()) {
        return ESP_ERR_NVS_NOT_FOUND;  // matches IDF: no namespace yet in RO mode
    }
    NvsHandle h;
    h.ns = name;
    h.writable = (mode == NVS_READWRITE);
    h.open = true;
    g_handles.push_back(h);
    *out_handle = (nvs_handle_t)g_handles.size();  // 1-based
    return ESP_OK;
}

static NvsHandle *resolve(nvs_handle_t handle) {
    if (handle == 0 || handle > g_handles.size()) return nullptr;
    NvsHandle *h = &g_handles[handle - 1];
    return h->open ? h : nullptr;
}

extern "C" void nvs_close(nvs_handle_t handle) {
    NvsHandle *h = resolve(handle);
    if (h) h->open = false;
}

extern "C" esp_err_t nvs_commit(nvs_handle_t handle) {
    return resolve(handle) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

extern "C" esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value) {
    NvsHandle *h = resolve(handle);
    if (!h || !h->writable || !key || !value) return ESP_ERR_INVALID_ARG;
    NvsEntry e;
    e.is_str = true;
    e.data.assign(value, value + strlen(value) + 1);
    g_store[h->ns][key] = e;
    return ESP_OK;
}

extern "C" esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out_value, size_t *length) {
    NvsHandle *h = resolve(handle);
    if (!h || !key || !length) return ESP_ERR_INVALID_ARG;
    auto ns = g_store.find(h->ns);
    if (ns == g_store.end()) return ESP_ERR_NVS_NOT_FOUND;
    auto it = ns->second.find(key);
    if (it == ns->second.end()) return ESP_ERR_NVS_NOT_FOUND;
    size_t need = it->second.data.size();
    if (!out_value) { *length = need; return ESP_OK; }
    if (*length < need) { *length = need; return ESP_ERR_INVALID_SIZE; }
    memcpy(out_value, it->second.data.data(), need);
    *length = need;
    return ESP_OK;
}

extern "C" esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value) {
    NvsHandle *h = resolve(handle);
    if (!h || !h->writable || !key) return ESP_ERR_INVALID_ARG;
    NvsEntry e;
    e.data.assign(1, value);
    g_store[h->ns][key] = e;
    return ESP_OK;
}

extern "C" esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out_value) {
    NvsHandle *h = resolve(handle);
    if (!h || !key || !out_value) return ESP_ERR_INVALID_ARG;
    auto ns = g_store.find(h->ns);
    if (ns == g_store.end()) return ESP_ERR_NVS_NOT_FOUND;
    auto it = ns->second.find(key);
    if (it == ns->second.end() || it->second.data.empty()) return ESP_ERR_NVS_NOT_FOUND;
    *out_value = it->second.data[0];
    return ESP_OK;
}

extern "C" esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length) {
    NvsHandle *h = resolve(handle);
    if (!h || !h->writable || !key || (!value && length)) return ESP_ERR_INVALID_ARG;
    NvsEntry e;
    e.data.assign((const uint8_t *)value, (const uint8_t *)value + length);
    g_store[h->ns][key] = e;
    return ESP_OK;
}

extern "C" esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length) {
    NvsHandle *h = resolve(handle);
    if (!h || !key || !length) return ESP_ERR_INVALID_ARG;
    auto ns = g_store.find(h->ns);
    if (ns == g_store.end()) return ESP_ERR_NVS_NOT_FOUND;
    auto it = ns->second.find(key);
    if (it == ns->second.end()) return ESP_ERR_NVS_NOT_FOUND;
    size_t need = it->second.data.size();
    if (!out_value) { *length = need; return ESP_OK; }
    if (*length < need) { *length = need; return ESP_ERR_INVALID_SIZE; }
    memcpy(out_value, it->second.data.data(), need);
    *length = need;
    return ESP_OK;
}

extern "C" esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key) {
    NvsHandle *h = resolve(handle);
    if (!h || !h->writable || !key) return ESP_ERR_INVALID_ARG;
    auto ns = g_store.find(h->ns);
    if (ns == g_store.end()) return ESP_ERR_NVS_NOT_FOUND;
    return ns->second.erase(key) ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

extern "C" esp_err_t nvs_erase_all(nvs_handle_t handle) {
    NvsHandle *h = resolve(handle);
    if (!h || !h->writable) return ESP_ERR_INVALID_ARG;
    g_store[h->ns].clear();
    return ESP_OK;
}

/* --------------------------- NVS iterator -------------------------- */
struct nvs_iterator_opaque {
    std::string ns;
    std::vector<std::string> keys;
    size_t idx = 0;
};

extern "C" esp_err_t nvs_entry_find(const char *, const char *namespace_name,
                                    nvs_type_t, nvs_iterator_t *output_iterator) {
    if (!namespace_name || !output_iterator) return ESP_ERR_INVALID_ARG;
    auto ns = g_store.find(namespace_name);
    if (ns == g_store.end() || ns->second.empty()) {
        *output_iterator = nullptr;
        return ESP_ERR_NVS_NOT_FOUND;
    }
    nvs_iterator_opaque *it = new nvs_iterator_opaque();
    it->ns = namespace_name;
    for (auto &kv : ns->second) it->keys.push_back(kv.first);
    it->idx = 0;
    *output_iterator = it;
    return ESP_OK;
}

extern "C" esp_err_t nvs_entry_next(nvs_iterator_t *iterator) {
    if (!iterator || !*iterator) return ESP_ERR_INVALID_ARG;
    nvs_iterator_opaque *it = *iterator;
    it->idx++;
    if (it->idx >= it->keys.size()) {
        delete it;
        *iterator = nullptr;
        return ESP_ERR_NVS_NOT_FOUND;
    }
    return ESP_OK;
}

extern "C" void nvs_entry_info(nvs_iterator_t iterator, nvs_entry_info_t *out_info) {
    if (!iterator || !out_info) return;
    memset(out_info, 0, sizeof(*out_info));
    snprintf(out_info->namespace_name, sizeof(out_info->namespace_name), "%s", iterator->ns.c_str());
    if (iterator->idx < iterator->keys.size())
        snprintf(out_info->key, sizeof(out_info->key), "%s", iterator->keys[iterator->idx].c_str());
    out_info->type = NVS_TYPE_STR;
}

extern "C" void nvs_release_iterator(nvs_iterator_t iterator) {
    delete iterator;
}

/* --------------------------- heap_caps ----------------------------- */
extern "C" void *heap_caps_malloc(size_t size, uint32_t) { return malloc(size); }
extern "C" void *heap_caps_calloc(size_t n, size_t size, uint32_t) { return calloc(n, size); }
extern "C" void *heap_caps_realloc(void *ptr, size_t size, uint32_t) { return realloc(ptr, size); }
extern "C" void heap_caps_free(void *ptr) { free(ptr); }
extern "C" size_t heap_caps_get_free_size(uint32_t) { return 1u << 20; }
extern "C" size_t heap_caps_get_largest_free_block(uint32_t) { return 1u << 20; }

/* ------------------------------ MAC -------------------------------- */
namespace { uint8_t g_mac[6] = {0x24, 0x6f, 0x28, 0xaa, 0xbb, 0xcc}; }

extern "C" void mock_set_default_mac(const uint8_t mac[6]) { memcpy(g_mac, mac, 6); }
extern "C" esp_err_t esp_efuse_mac_get_default(uint8_t *mac) {
    if (!mac) return ESP_ERR_INVALID_ARG;
    memcpy(mac, g_mac, 6);
    return ESP_OK;
}
extern "C" esp_err_t esp_read_mac(uint8_t *mac, int) {
    if (!mac) return ESP_ERR_INVALID_ARG;
    memcpy(mac, g_mac, 6);
    return ESP_OK;
}

/* ----------------------------- timer ------------------------------- */
extern "C" int64_t esp_timer_get_time(void) {
    static int64_t t = 0;
    t += 1000;  // advance 1ms each call so timeout loops terminate
    return t;
}

/* ----------------------------- random ------------------------------ */
extern "C" uint32_t esp_random(void) { return (uint32_t)rand(); }
extern "C" void esp_fill_random(void *buf, size_t len) {
    uint8_t *b = (uint8_t *)buf;
    for (size_t i = 0; i < len; i++) b[i] = (uint8_t)rand();
}
