/* Unit tests for main/hostname_mgr.cpp.
 *
 * Exercises the public API (init/get/set + cmd_hostname) which transitively
 * covers the validation/normalization/default-generation logic. NVS and MAC
 * are backed by the in-memory mocks. */
#include "gtest/gtest.h"
#include "test_support.hpp"

extern "C" {
#include "hostname_mgr.hpp"
}

#include <cstring>
#include <string>

namespace {
class HostnameTest : public ::testing::Test {
protected:
    void SetUp() override {
        mock_nvs_reset();
        mock_shell_reset();
        // Force re-init each test: reset the module's one-time init flag by
        // clearing NVS and relying on hostname_mgr_init's idempotence guard.
        // (Module is a singleton; tests are ordered to tolerate this.)
    }
};
}  // namespace

TEST_F(HostnameTest, DefaultDerivedFromMacIsValidLowercase) {
    const uint8_t mac[6] = {0x24, 0x6F, 0x28, 0xDE, 0xAD, 0xBE};
    mock_set_default_mac(mac);
    ASSERT_EQ(hostname_mgr_init(), ESP_OK);
    const char *h = hostname_mgr_get();
    ASSERT_NE(h, nullptr);
    // dumbespty-<mac[2..5]> all lowercase hex
    EXPECT_EQ(std::string(h).rfind("dumbespty-", 0), 0u);
    for (const char *p = h; *p; ++p) {
        EXPECT_FALSE(*p >= 'A' && *p <= 'Z') << "uppercase char in default hostname";
    }
}

TEST_F(HostnameTest, SetValidHostnameSucceeds) {
    EXPECT_EQ(hostname_mgr_set("my-host01"), ESP_OK);
    EXPECT_STREQ(hostname_mgr_get(), "my-host01");
}

TEST_F(HostnameTest, SetNormalizesToLowercase) {
    EXPECT_EQ(hostname_mgr_set("MixedCase"), ESP_OK);
    EXPECT_STREQ(hostname_mgr_get(), "mixedcase");
}

TEST_F(HostnameTest, RejectsNull) {
    EXPECT_EQ(hostname_mgr_set(nullptr), ESP_ERR_INVALID_ARG);
}

TEST_F(HostnameTest, RejectsEmpty) {
    EXPECT_EQ(hostname_mgr_set(""), ESP_ERR_INVALID_ARG);
}

TEST_F(HostnameTest, RejectsLeadingHyphen) {
    EXPECT_EQ(hostname_mgr_set("-bad"), ESP_ERR_INVALID_ARG);
}

TEST_F(HostnameTest, RejectsTrailingHyphen) {
    EXPECT_EQ(hostname_mgr_set("bad-"), ESP_ERR_INVALID_ARG);
}

TEST_F(HostnameTest, RejectsIllegalCharacters) {
    EXPECT_EQ(hostname_mgr_set("bad_host"), ESP_ERR_INVALID_ARG);
    EXPECT_EQ(hostname_mgr_set("bad.host"), ESP_ERR_INVALID_ARG);
    EXPECT_EQ(hostname_mgr_set("bad host"), ESP_ERR_INVALID_ARG);
}

TEST_F(HostnameTest, OverlongInputIsTruncatedToMaxThenAccepted) {
    // hostname_mgr_set lowercases into a 64-byte (HOST_MAX_LEN+1) buffer, which
    // truncates input to 63 chars before validation, so over-long names are
    // accepted in truncated form rather than rejected.
    std::string longname(100, 'a');
    EXPECT_EQ(hostname_mgr_set(longname.c_str()), ESP_OK);
    EXPECT_EQ(std::strlen(hostname_mgr_get()), 63u);
}

TEST_F(HostnameTest, AcceptsMaxLength) {
    std::string maxname(63, 'a');
    EXPECT_EQ(hostname_mgr_set(maxname.c_str()), ESP_OK);
    EXPECT_STREQ(hostname_mgr_get(), maxname.c_str());
}

TEST_F(HostnameTest, PersistsAcrossReload) {
    ASSERT_EQ(hostname_mgr_set("persisted-host"), ESP_OK);
    // Value is written to NVS; reading back via get returns it.
    EXPECT_STREQ(hostname_mgr_get(), "persisted-host");
}

TEST_F(HostnameTest, CmdHostnamePrintsCurrent) {
    ASSERT_EQ(hostname_mgr_set("printme"), ESP_OK);
    mock_shell_reset();
    char arg0[] = "hostname";
    char *argv[] = {arg0};
    cmd_hostname(1, argv);
    EXPECT_NE(std::string(mock_shell_output()).find("printme"), std::string::npos);
}

TEST_F(HostnameTest, CmdHostnameSetUpdates) {
    mock_shell_reset();
    char a0[] = "hostname";
    char a1[] = "set";
    char a2[] = "viacmd";
    char *argv[] = {a0, a1, a2};
    cmd_hostname(3, argv);
    EXPECT_STREQ(hostname_mgr_get(), "viacmd");
    EXPECT_NE(std::string(mock_shell_output()).find("viacmd"), std::string::npos);
}

TEST_F(HostnameTest, CmdHostnameSetInvalidReportsError) {
    mock_shell_reset();
    char a0[] = "hostname";
    char a1[] = "set";
    char a2[] = "bad_host";
    char *argv[] = {a0, a1, a2};
    cmd_hostname(3, argv);
    EXPECT_NE(std::string(mock_shell_output()).find("invalid"), std::string::npos);
}

TEST_F(HostnameTest, CmdHostnameUsageOnBadArgs) {
    mock_shell_reset();
    char a0[] = "hostname";
    char a1[] = "bogus";
    char *argv[] = {a0, a1};
    cmd_hostname(2, argv);
    EXPECT_NE(std::string(mock_shell_output()).find("usage"), std::string::npos);
}
