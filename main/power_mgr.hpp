#ifndef POWER_MGR_HPP
#define POWER_MGR_HPP

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Idle low-power management.
//
// The device enters a low-power state when no user HID input and no SSH
// terminal output have occurred for POWER_IDLE_TIMEOUT_MS. Low power means:
//   - display backlight turned off
//   - main loop slowed (longer vTaskDelay) to reduce CPU wakeups
// Any subsequent activity (key press or SSH output) restores full brightness
// and the fast loop cadence immediately.
//
// The decision logic is hardware-independent and host-unit-testable: time and
// the backlight are injected via the hooks below. console_base.cpp wires the
// real FreeRTOS tick clock and the Waveshare backlight driver at startup; the
// host tests inject deterministic fakes.

#define POWER_IDLE_TIMEOUT_MS 30000

// Loop delay (milliseconds) returned by power_mgr_step() in each state.
#define POWER_LOOP_DELAY_ACTIVE_MS 10
#define POWER_LOOP_DELAY_IDLE_MS 100

// Injected dependencies.
//   now_ms()              -> a monotonic millisecond clock.
//   get_brightness()      -> current backlight percent (0..100).
//   set_brightness(p)     -> apply backlight percent (0 == off).
//   brightness_supported  -> if false, backlight control is skipped.
typedef struct {
    uint32_t (*now_ms)(void);
    int (*get_brightness)(void);
    void (*set_brightness)(int percent);
    bool brightness_supported;
} power_mgr_hooks_t;

// Install the hooks and reset internal state to "active". Call once at startup
// (and in test setup). Marks current time as the last activity.
void power_mgr_init(const power_mgr_hooks_t *hooks);

// Record user/SSH activity. Resets the idle timer; if currently in low-power,
// restores the saved brightness and returns to the active state immediately.
void power_mark_activity(void);

// Run one idle-management step. Returns the recommended loop delay in ms for
// this iteration (short when active, long when idle). Call once per main loop.
unsigned power_mgr_step(void);

// True when currently in the low-power (backlight-off) state. Exposed for
// tests and diagnostics.
bool power_mgr_is_low_power(void);

#ifdef __cplusplus
}
#endif

#endif
