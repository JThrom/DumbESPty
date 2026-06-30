// Idle low-power manager. See power_mgr.hpp for the contract.
//
// Pure decision logic kept free of FreeRTOS/display includes so it can be
// exercised by the host unit tests (test/unit/test_power_mgr.cpp) with injected
// fakes for the clock and backlight. console_base.cpp installs the real hooks.

#include "power_mgr.hpp"

#include <stddef.h>

namespace {

power_mgr_hooks_t s_hooks = {};
bool s_have_hooks = false;

uint32_t s_last_activity_ms = 0;
bool s_low_power = false;
int s_saved_brightness = -1;

uint32_t now_ms(void) {
    return (s_have_hooks && s_hooks.now_ms) ? s_hooks.now_ms() : 0;
}

}  // namespace

void power_mgr_init(const power_mgr_hooks_t *hooks) {
    if (hooks) {
        s_hooks = *hooks;
        s_have_hooks = true;
    } else {
        s_have_hooks = false;
    }
    s_low_power = false;
    s_saved_brightness = -1;
    s_last_activity_ms = now_ms();
}

void power_mark_activity(void) {
    s_last_activity_ms = now_ms();
    if (!s_low_power) return;

    // Wake: restore the brightness captured at sleep entry.
    if (s_have_hooks && s_hooks.brightness_supported && s_hooks.set_brightness) {
        int restore = (s_saved_brightness > 0) ? s_saved_brightness : 25;
        s_hooks.set_brightness(restore);
    }
    s_low_power = false;
}

unsigned power_mgr_step(void) {
    const uint32_t now = now_ms();
    // Unsigned subtraction is wrap-safe for a monotonic millisecond clock.
    const uint32_t idle = now - s_last_activity_ms;

    if (!s_low_power && idle >= POWER_IDLE_TIMEOUT_MS) {
        if (s_have_hooks && s_hooks.brightness_supported &&
            s_hooks.get_brightness && s_hooks.set_brightness) {
            s_saved_brightness = s_hooks.get_brightness();
            s_hooks.set_brightness(0);  // backlight off
        }
        s_low_power = true;
    }

    return s_low_power ? POWER_LOOP_DELAY_IDLE_MS : POWER_LOOP_DELAY_ACTIVE_MS;
}

bool power_mgr_is_low_power(void) {
    return s_low_power;
}
