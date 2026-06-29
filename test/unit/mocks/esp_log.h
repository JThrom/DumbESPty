/* Host test mock of ESP-IDF esp_log.h.
 * Logging is compiled to no-ops so source under test links and runs on host
 * without an ESP-IDF logging backend. */
#ifndef MOCK_ESP_LOG_H
#define MOCK_ESP_LOG_H

typedef enum {
    ESP_LOG_NONE,
    ESP_LOG_ERROR,
    ESP_LOG_WARN,
    ESP_LOG_INFO,
    ESP_LOG_DEBUG,
    ESP_LOG_VERBOSE,
} esp_log_level_t;

#define ESP_LOGE(tag, ...) ((void)0)
#define ESP_LOGW(tag, ...) ((void)0)
#define ESP_LOGI(tag, ...) ((void)0)
#define ESP_LOGD(tag, ...) ((void)0)
#define ESP_LOGV(tag, ...) ((void)0)

#define ESP_LOG_BUFFER_HEX(tag, buf, len) ((void)0)
#define ESP_LOG_BUFFER_HEXDUMP(tag, buf, len, lvl) ((void)0)

static inline void esp_log_level_set(const char *tag, esp_log_level_t level) {
    (void)tag;
    (void)level;
}

#endif
