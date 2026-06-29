/* Unit tests for main/usb_hid_host.cpp HID keycode -> ASCII mapping.
 *
 * White-box: the source is #included directly so the file-local
 * dispatch_keycode() / parse_boot_keyboard_report() / map_shifted() can be
 * driven and the produced characters observed on the (mocked) key queue.
 *
 * Because the source is included here, it must NOT also be in the
 * firmware_under_test library (see CMakeLists). */
#include "gtest/gtest.h"

#include <vector>
#include <string>

// Pull in the production source for white-box access to its statics.
#include "usb_hid_host.cpp"

namespace {

// Drain the module's key queue (s_key_queue) into a string.
std::string drain_keys() {
    std::string out;
    if (!s_key_queue) return out;
    char c = 0;
    while (xQueueReceive(s_key_queue, &c, 0) == pdTRUE) out.push_back(c);
    return out;
}

class UsbHidTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Provide a real (mock) queue for queue_key() to write into.
        if (!s_key_queue) s_key_queue = xQueueCreate(64, sizeof(char));
        mock_queue_reset(s_key_queue);
        memset(s_prev_keys, 0, sizeof(s_prev_keys));
    }
};

}  // namespace

TEST_F(UsbHidTest, MapsLowercaseLetters) {
    EXPECT_TRUE(dispatch_keycode(0x00, 0x04));  // 'a'
    EXPECT_TRUE(dispatch_keycode(0x00, 0x1D));  // 'z'
    EXPECT_EQ(drain_keys(), "az");
}

TEST_F(UsbHidTest, MapsDigits) {
    dispatch_keycode(0x00, 0x1E);  // '1'
    dispatch_keycode(0x00, 0x27);  // '0'
    EXPECT_EQ(drain_keys(), "10");
}

TEST_F(UsbHidTest, ShiftUppercasesLetters) {
    dispatch_keycode(0x02, 0x04);  // Left-Shift + 'a' -> 'A'
    EXPECT_EQ(drain_keys(), "A");
}

TEST_F(UsbHidTest, ShiftMapsSymbols) {
    dispatch_keycode(0x02, 0x1E);  // Shift + '1' -> '!'
    dispatch_keycode(0x20, 0x1F);  // Right-Shift + '2' -> '@'
    EXPECT_EQ(drain_keys(), "!@");
}

TEST_F(UsbHidTest, CtrlLetterProducesControlCode) {
    dispatch_keycode(0x01, 0x06);  // Ctrl + 'c' -> 0x03
    std::string k = drain_keys();
    ASSERT_EQ(k.size(), 1u);
    EXPECT_EQ((uint8_t)k[0], 0x03);
}

TEST_F(UsbHidTest, EnterAndTabAndSpace) {
    dispatch_keycode(0x00, 0x28);  // Enter -> '\r'
    dispatch_keycode(0x00, 0x2B);  // Tab -> '\t'
    dispatch_keycode(0x00, 0x2C);  // Space
    EXPECT_EQ(drain_keys(), "\r\t ");
}

TEST_F(UsbHidTest, ArrowKeysProduceCursorBytes) {
    dispatch_keycode(0x00, 0x52);  // Up -> 0x1E
    dispatch_keycode(0x00, 0x51);  // Down -> 0x1F
    dispatch_keycode(0x00, 0x4F);  // Right -> 0x1D
    dispatch_keycode(0x00, 0x50);  // Left -> 0x1C
    std::string k = drain_keys();
    ASSERT_EQ(k.size(), 4u);
    EXPECT_EQ((uint8_t)k[0], 0x1E);
    EXPECT_EQ((uint8_t)k[1], 0x1F);
    EXPECT_EQ((uint8_t)k[2], 0x1D);
    EXPECT_EQ((uint8_t)k[3], 0x1C);
}

TEST_F(UsbHidTest, KeypadEnterAndSlash) {
    dispatch_keycode(0x00, 0x58);  // Keypad Enter -> '\r'
    dispatch_keycode(0x00, 0x54);  // Keypad '/'
    EXPECT_EQ(drain_keys(), "\r/");
}

TEST_F(UsbHidTest, UnmappedKeycodeReturnsFalse) {
    EXPECT_FALSE(dispatch_keycode(0x00, 0x00));  // reserved/no event
    EXPECT_EQ(drain_keys(), "");
}

TEST_F(UsbHidTest, OutOfRangeKeycodeReturnsFalse) {
    EXPECT_FALSE(dispatch_keycode(0x00, 0xF0));
}

TEST_F(UsbHidTest, MapShiftedTableLookup) {
    EXPECT_EQ(map_shifted('1'), '!');
    EXPECT_EQ(map_shifted('-'), '_');
    EXPECT_EQ(map_shifted('='), '+');
    EXPECT_EQ(map_shifted('/'), '?');
    EXPECT_EQ(map_shifted('z'), 'z');  // not in shift table -> unchanged
}

TEST_F(UsbHidTest, BootReportDispatchesNewKeysOnce) {
    // Report: modifier=0, reserved=0, keys: 'a'(0x04)
    uint8_t r1[8] = {0, 0, 0x04, 0, 0, 0, 0, 0};
    parse_boot_keyboard_report(r1, sizeof(r1));
    EXPECT_EQ(drain_keys(), "a");

    // Same key still held -> no repeat dispatch.
    parse_boot_keyboard_report(r1, sizeof(r1));
    EXPECT_EQ(drain_keys(), "");

    // Key released, new key 'b'(0x05) pressed.
    uint8_t r2[8] = {0, 0, 0x05, 0, 0, 0, 0, 0};
    parse_boot_keyboard_report(r2, sizeof(r2));
    EXPECT_EQ(drain_keys(), "b");
}

TEST_F(UsbHidTest, BootReportIgnoresShortBuffers) {
    uint8_t r[4] = {0, 0, 0x04, 0};
    parse_boot_keyboard_report(r, sizeof(r));  // len < 8 -> ignored
    EXPECT_EQ(drain_keys(), "");
}
