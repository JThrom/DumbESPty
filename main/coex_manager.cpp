#include "coex_manager.hpp"
#include "ble_hid_host.hpp"
#include "esp_wifi.h"
#include "esp_coexist.h"
#include "esp_log.h"

static const char *TAG = "coex";
static bool acquired = false;

void coex_acquire(void) {
    if (acquired) return;
    acquired = true;

    ble_hid_pause_scan();
    ble_hid_host_disconnect();
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);

    ESP_LOGD(TAG, "acquired");
}

void coex_release(void) {
    if (!acquired) return;
    acquired = false;

    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
    ble_hid_resume_scan();

    ESP_LOGD(TAG, "released");
}
