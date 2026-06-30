/* Unit tests for main/power_mgr.cpp.
 *
 * The idle low-power decision logic is hardware-independent: the clock and the
 * backlight are injected via power_mgr_hooks_t. These tests drive a fake clock
 * and a fake backlight to verify the full state machine deterministically (no
 * FreeRTOS, no display driver, no real time). */
#include "gtest/gtest.h"

extern "C" {
#include "power_mgr.hpp"
}

namespace {

// Fake injected dependencies, shared via file-static state because the hooks
// are plain C function pointers.
uint32_t g_now = 0;
int g_brightness = 25;
int g_set_calls = 0;
int g_last_set = -999;

uint32_t fake_now_ms(void) { return g_now; }
int fake_get_brightness(void) { return g_brightness; }
void fake_set_brightness(int p) {
    g_set_calls++;
    g_last_set = p;
    g_brightness = p;
}

class PowerMgrTest : public ::testing::Test {
protected:
    void SetUp() override {
        g_now = 1000;            // arbitrary non-zero start
        g_brightness = 25;
        g_set_calls = 0;
        g_last_set = -999;
        installHooks(true);
        // Timeout is module-static and persists across tests; reset to default.
        power_mgr_set_idle_timeout_ms(POWER_IDLE_TIMEOUT_MS);
    }

    void installHooks(bool brightness_supported) {
        power_mgr_hooks_t hooks = {};
        hooks.now_ms = fake_now_ms;
        hooks.get_brightness = fake_get_brightness;
        hooks.set_brightness = fake_set_brightness;
        hooks.brightness_supported = brightness_supported;
        power_mgr_init(&hooks);
    }

    void advance(uint32_t ms) { g_now += ms; }
};

TEST_F(PowerMgrTest, StartsActive) {
    EXPECT_FALSE(power_mgr_is_low_power());
    EXPECT_EQ(power_mgr_step(), POWER_LOOP_DELAY_ACTIVE_MS);
}

TEST_F(PowerMgrTest, StaysActiveBeforeTimeout) {
    advance(POWER_IDLE_TIMEOUT_MS - 1);
    EXPECT_EQ(power_mgr_step(), POWER_LOOP_DELAY_ACTIVE_MS);
    EXPECT_FALSE(power_mgr_is_low_power());
    EXPECT_EQ(g_set_calls, 0);
}

TEST_F(PowerMgrTest, EntersLowPowerExactlyAtTimeout) {
    advance(POWER_IDLE_TIMEOUT_MS);
    EXPECT_EQ(power_mgr_step(), POWER_LOOP_DELAY_IDLE_MS);
    EXPECT_TRUE(power_mgr_is_low_power());
}

TEST_F(PowerMgrTest, TurnsBacklightOffOnEntry) {
    advance(POWER_IDLE_TIMEOUT_MS);
    power_mgr_step();
    EXPECT_EQ(g_last_set, 0);          // backlight off
    EXPECT_EQ(g_set_calls, 1);
}

TEST_F(PowerMgrTest, SavesAndRestoresBrightness) {
    g_brightness = 70;                 // user-set brightness before idle
    advance(POWER_IDLE_TIMEOUT_MS);
    power_mgr_step();
    EXPECT_TRUE(power_mgr_is_low_power());
    EXPECT_EQ(g_brightness, 0);

    power_mark_activity();             // wake
    EXPECT_FALSE(power_mgr_is_low_power());
    EXPECT_EQ(g_brightness, 70);       // restored to saved value
}

TEST_F(PowerMgrTest, ActivityResetsIdleTimer) {
    advance(POWER_IDLE_TIMEOUT_MS - 5);
    power_mgr_step();
    EXPECT_FALSE(power_mgr_is_low_power());

    power_mark_activity();             // reset timer
    advance(POWER_IDLE_TIMEOUT_MS - 5);
    EXPECT_EQ(power_mgr_step(), POWER_LOOP_DELAY_ACTIVE_MS);
    EXPECT_FALSE(power_mgr_is_low_power());
}

TEST_F(PowerMgrTest, ActivityWhileActiveDoesNotTouchBacklight) {
    power_mark_activity();
    EXPECT_EQ(g_set_calls, 0);         // no backlight call when already active
}

TEST_F(PowerMgrTest, WakeOnlySetsBrightnessOnce) {
    advance(POWER_IDLE_TIMEOUT_MS);
    power_mgr_step();                  // off (1 call)
    power_mark_activity();             // restore (2nd call)
    EXPECT_EQ(g_set_calls, 2);
    power_mark_activity();             // already active, no further call
    EXPECT_EQ(g_set_calls, 2);
}

TEST_F(PowerMgrTest, ReentersLowPowerAfterWake) {
    advance(POWER_IDLE_TIMEOUT_MS);
    power_mgr_step();
    EXPECT_TRUE(power_mgr_is_low_power());

    power_mark_activity();
    EXPECT_FALSE(power_mgr_is_low_power());

    advance(POWER_IDLE_TIMEOUT_MS);
    EXPECT_EQ(power_mgr_step(), POWER_LOOP_DELAY_IDLE_MS);
    EXPECT_TRUE(power_mgr_is_low_power());
}

TEST_F(PowerMgrTest, StepIsIdempotentWhileIdle) {
    advance(POWER_IDLE_TIMEOUT_MS);
    power_mgr_step();                  // enter (1 set call)
    advance(5000);
    power_mgr_step();                  // still idle, no extra backlight calls
    power_mgr_step();
    EXPECT_EQ(g_set_calls, 1);
    EXPECT_TRUE(power_mgr_is_low_power());
}

TEST_F(PowerMgrTest, NoBacklightCallsWhenUnsupported) {
    installHooks(/*brightness_supported=*/false);
    g_set_calls = 0;
    advance(POWER_IDLE_TIMEOUT_MS);
    EXPECT_EQ(power_mgr_step(), POWER_LOOP_DELAY_IDLE_MS);
    EXPECT_TRUE(power_mgr_is_low_power());  // still tracks state...
    power_mark_activity();
    EXPECT_FALSE(power_mgr_is_low_power());
    EXPECT_EQ(g_set_calls, 0);              // ...but never touches backlight
}

TEST_F(PowerMgrTest, ClockWrapAroundIsHandled) {
    // Start the activity timestamp near UINT32_MAX, then wrap the clock.
    g_now = 0xFFFFFFF0u;
    installHooks(true);                // last_activity = 0xFFFFFFF0
    power_mgr_set_idle_timeout_ms(POWER_IDLE_TIMEOUT_MS);
    g_now += POWER_IDLE_TIMEOUT_MS;    // wraps past zero
    EXPECT_EQ(power_mgr_step(), POWER_LOOP_DELAY_IDLE_MS);
    EXPECT_TRUE(power_mgr_is_low_power());
}

// --- configurable / disable-able timeout -------------------------------------

TEST_F(PowerMgrTest, DefaultTimeoutGetterMatchesDefine) {
    EXPECT_EQ(power_mgr_get_idle_timeout_ms(), (uint32_t)POWER_IDLE_TIMEOUT_MS);
}

TEST_F(PowerMgrTest, CustomTimeoutIsHonored) {
    power_mgr_set_idle_timeout_ms(5000);
    EXPECT_EQ(power_mgr_get_idle_timeout_ms(), 5000u);

    advance(4999);
    EXPECT_EQ(power_mgr_step(), POWER_LOOP_DELAY_ACTIVE_MS);
    EXPECT_FALSE(power_mgr_is_low_power());

    advance(1);                        // total 5000
    EXPECT_EQ(power_mgr_step(), POWER_LOOP_DELAY_IDLE_MS);
    EXPECT_TRUE(power_mgr_is_low_power());
}

TEST_F(PowerMgrTest, ZeroTimeoutDisablesLowPower) {
    power_mgr_set_idle_timeout_ms(0);
    EXPECT_EQ(power_mgr_get_idle_timeout_ms(), 0u);

    advance(POWER_IDLE_TIMEOUT_MS * 10);   // far beyond any normal timeout
    EXPECT_EQ(power_mgr_step(), POWER_LOOP_DELAY_ACTIVE_MS);
    EXPECT_FALSE(power_mgr_is_low_power());
    EXPECT_EQ(g_set_calls, 0);             // backlight never touched
}

TEST_F(PowerMgrTest, SettingTimeoutResetsIdleTimer) {
    advance(POWER_IDLE_TIMEOUT_MS - 5);    // almost idle on the default timeout
    power_mgr_set_idle_timeout_ms(POWER_IDLE_TIMEOUT_MS);  // resets the timer
    advance(10);                           // would have tripped the old deadline
    EXPECT_EQ(power_mgr_step(), POWER_LOOP_DELAY_ACTIVE_MS);
    EXPECT_FALSE(power_mgr_is_low_power());
}

TEST_F(PowerMgrTest, DisablingWhileIdleStopsFurtherSleepButKeepsState) {
    // Enter low-power on a short timeout, then disable.
    power_mgr_set_idle_timeout_ms(1000);
    advance(1000);
    power_mgr_step();
    EXPECT_TRUE(power_mgr_is_low_power());

    // Disabling does not force a wake on its own; activity (key/touch) does.
    power_mgr_set_idle_timeout_ms(0);
    power_mark_activity();
    EXPECT_FALSE(power_mgr_is_low_power());

    // With timeout disabled it never re-sleeps.
    advance(POWER_IDLE_TIMEOUT_MS * 10);
    EXPECT_EQ(power_mgr_step(), POWER_LOOP_DELAY_ACTIVE_MS);
    EXPECT_FALSE(power_mgr_is_low_power());
}

}  // namespace
