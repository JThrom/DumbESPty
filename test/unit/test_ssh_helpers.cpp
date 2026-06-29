/* Unit tests for hardware-independent helpers in main/ssh_client.cpp.
 *
 * White-box: the source is #included so file-local helpers (transport rc
 * decode, FNV-1a trust-key hashing, tailnet IPv4 detection, hostkey method
 * preference building, SHA256 fingerprint formatting, DSR fast-query filter)
 * are reachable. libssh2 / mbedtls / sockets are mocked; tests that need
 * controllable libssh2 state use the mock_libssh2_* setters.
 *
 * Because the source is included here it must NOT be in firmware_under_test. */
#include "gtest/gtest.h"

#include <cstring>
#include <string>

// Pull in production source for white-box access.
#include "ssh_client.cpp"

namespace {
class SshHelpers : public ::testing::Test {
protected:
    void SetUp() override {
        mock_nvs_reset();
        mock_libssh2_reset();
        // Reset module globals touched by helpers under test.
        ssh_connected = false;
        channel = nullptr;
        session = nullptr;
    }
};
}  // namespace

/* ----------------------- transport rc decoding --------------------- */
TEST_F(SshHelpers, TransportRcKnownCodes) {
    EXPECT_STREQ(ssh_transport_rc_str(LIBSSH2_ERROR_NONE), "none");
    EXPECT_STREQ(ssh_transport_rc_str(LIBSSH2_ERROR_KEX_FAILURE), "kex-failure");
    EXPECT_STREQ(ssh_transport_rc_str(LIBSSH2_ERROR_AUTHENTICATION_FAILED), "auth-failed");
    EXPECT_STREQ(ssh_transport_rc_str(LIBSSH2_ERROR_EAGAIN), "eagain (would-block)");
}

TEST_F(SshHelpers, TransportRcUnknownCode) {
    EXPECT_STREQ(ssh_transport_rc_str(12345), "unknown");
}

/* ----------------------- FNV-1a hashing ---------------------------- */
TEST_F(SshHelpers, Fnv1aKnownVectors) {
    // FNV-1a 32-bit empty string == offset basis.
    EXPECT_EQ(fnv1a_32(""), 2166136261u);
    // Null pointer also yields offset basis.
    EXPECT_EQ(fnv1a_32(nullptr), 2166136261u);
    // "a" -> 0xe40c292c (canonical FNV-1a 32-bit test vector).
    EXPECT_EQ(fnv1a_32("a"), 0xe40c292cu);
}

TEST_F(SshHelpers, Fnv1aDistinctInputsDiffer) {
    EXPECT_NE(fnv1a_32("host-a:22"), fnv1a_32("host-b:22"));
}

/* ----------------------- trust key formatting ---------------------- */
TEST_F(SshHelpers, MakeTrustKeyFormat) {
    char key[32];
    ASSERT_TRUE(make_trust_key("example.com", 22, key, sizeof(key)));
    // "h" + 8 lowercase hex digits.
    EXPECT_EQ(strlen(key), 9u);
    EXPECT_EQ(key[0], 'h');
    for (int i = 1; i < 9; i++) {
        char c = key[i];
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
}

TEST_F(SshHelpers, MakeTrustKeyDeterministic) {
    char a[32], b[32];
    ASSERT_TRUE(make_trust_key("example.com", 22, a, sizeof(a)));
    ASSERT_TRUE(make_trust_key("example.com", 22, b, sizeof(b)));
    EXPECT_STREQ(a, b);
}

TEST_F(SshHelpers, MakeTrustKeyPortAffectsResult) {
    char a[32], b[32];
    make_trust_key("example.com", 22, a, sizeof(a));
    make_trust_key("example.com", 2222, b, sizeof(b));
    EXPECT_STRNE(a, b);
}

TEST_F(SshHelpers, MakeTrustKeyRejectsBadArgs) {
    char key[32];
    EXPECT_FALSE(make_trust_key(nullptr, 22, key, sizeof(key)));
    EXPECT_FALSE(make_trust_key("", 22, key, sizeof(key)));
    EXPECT_FALSE(make_trust_key("host", 22, nullptr, 0));
    EXPECT_FALSE(make_trust_key("host", 22, key, 0));
}

/* ----------------------- tailnet detection ------------------------- */
TEST_F(SshHelpers, TailnetIpv4Range) {
    // 100.64.0.0/10 covers 100.64.0.0 .. 100.127.255.255
    EXPECT_TRUE(is_tailnet_ipv4(0x64400000u));   // 100.64.0.0
    EXPECT_TRUE(is_tailnet_ipv4(0x647FFFFFu));   // 100.127.255.255
    EXPECT_FALSE(is_tailnet_ipv4(0x643FFFFFu));  // 100.63.255.255 (below)
    EXPECT_FALSE(is_tailnet_ipv4(0x64800000u));  // 100.128.0.0 (above)
    EXPECT_FALSE(is_tailnet_ipv4(0xC0A80001u));  // 192.168.0.1
}

/* ----------------------- hostkey method preference ----------------- */
TEST_F(SshHelpers, HostkeyPrefBuildsInPreferenceOrder) {
    // Server supports ed25519 + rsa; pref order should list ed25519 first.
    const char *algs[] = {"rsa-sha2-256", "ssh-ed25519", "ssh-rsa"};
    mock_libssh2_set_supported_algs(LIBSSH2_METHOD_HOSTKEY, algs, 3);

    char out[128];
    bool has_ed = false;
    LIBSSH2_SESSION *sess = libssh2_session_init();
    ASSERT_TRUE(build_hostkey_method_pref(sess, out, sizeof(out), &has_ed));
    EXPECT_TRUE(has_ed);
    // ed25519 must come before rsa-sha2-256 in the output CSV.
    std::string s(out);
    EXPECT_LT(s.find("ssh-ed25519"), s.find("rsa-sha2-256"));
}

TEST_F(SshHelpers, HostkeyPrefNoOverlapReturnsFalse) {
    const char *algs[] = {"some-unknown-alg"};
    mock_libssh2_set_supported_algs(LIBSSH2_METHOD_HOSTKEY, algs, 1);
    char out[128];
    bool has_ed = true;
    LIBSSH2_SESSION *sess = libssh2_session_init();
    EXPECT_FALSE(build_hostkey_method_pref(sess, out, sizeof(out), &has_ed));
    EXPECT_FALSE(has_ed);
}

TEST_F(SshHelpers, HostkeyPrefRejectsNullSession) {
    char out[128];
    EXPECT_FALSE(build_hostkey_method_pref(nullptr, out, sizeof(out), nullptr));
}

/* ----------------------- SHA256 fingerprint ------------------------ */
TEST_F(SshHelpers, Sha256FingerprintFormat) {
    unsigned char fakekey[] = {1, 2, 3, 4};
    mock_libssh2_set_hostkey(LIBSSH2_HOSTKEY_TYPE_ED25519, fakekey, sizeof(fakekey));
    unsigned char digest[32];
    for (int i = 0; i < 32; i++) digest[i] = (unsigned char)i;
    mock_libssh2_set_hostkey_hash(LIBSSH2_HOSTKEY_HASH_SHA256, digest);

    char fp[80];
    char type[32];
    LIBSSH2_SESSION *sess = libssh2_session_init();
    ASSERT_TRUE(get_server_hostkey_sha256(sess, fp, sizeof(fp), type, sizeof(type)));
    EXPECT_EQ(std::string(fp).rfind("SHA256:", 0), 0u);  // starts with SHA256:
    EXPECT_STREQ(type, "ssh-ed25519");
    // Base64 of 32 bytes, padding stripped -> 43 chars after the prefix.
    EXPECT_EQ(strlen(fp), strlen("SHA256:") + 43u);
}

TEST_F(SshHelpers, Sha256FingerprintReportsHostkeyTypeName) {
    unsigned char fakekey[] = {9};
    unsigned char digest[32] = {0};
    char fp[80], type[32];
    mock_libssh2_set_hostkey_hash(LIBSSH2_HOSTKEY_HASH_SHA256, digest);

    mock_libssh2_set_hostkey(LIBSSH2_HOSTKEY_TYPE_RSA, fakekey, sizeof(fakekey));
    LIBSSH2_SESSION *sess = libssh2_session_init();
    ASSERT_TRUE(get_server_hostkey_sha256(sess, fp, sizeof(fp), type, sizeof(type)));
    EXPECT_STREQ(type, "ssh-rsa");

    mock_libssh2_set_hostkey(LIBSSH2_HOSTKEY_TYPE_ECDSA_256, fakekey, sizeof(fakekey));
    ASSERT_TRUE(get_server_hostkey_sha256(sess, fp, sizeof(fp), type, sizeof(type)));
    EXPECT_STREQ(type, "ecdsa-sha2-nistp256");
}

TEST_F(SshHelpers, Sha256FingerprintFailsWithoutHostkey) {
    char fp[80], type[32];
    LIBSSH2_SESSION *sess = libssh2_session_init();
    // No hostkey set -> libssh2_session_hostkey returns null.
    EXPECT_FALSE(get_server_hostkey_sha256(sess, fp, sizeof(fp), type, sizeof(type)));
}

/* ----------------------- DSR fast-query filter --------------------- */
TEST_F(SshHelpers, FastQueryFilterPassesNormalBytes) {
    const char *src = "hello world";
    char dst[SSH_RX_BUF_SIZE];
    size_t n = filter_and_reply_fast_queries(src, strlen(src), dst);
    EXPECT_EQ(n, strlen(src));
    EXPECT_STREQ(dst, "hello world");
}

TEST_F(SshHelpers, FastQueryFilterStripsDsrWhenConnected) {
    // When connected, CSI 5n is consumed (auto-replied) and removed from output.
    ssh_connected = true;
    static int dummy_ch;
    channel = (LIBSSH2_CHANNEL *)&dummy_ch;
    const char src[] = "ab\033[5ncd";
    char dst[SSH_RX_BUF_SIZE];
    size_t n = filter_and_reply_fast_queries(src, sizeof(src) - 1, dst);
    EXPECT_STREQ(dst, "abcd");
    EXPECT_EQ(n, 4u);
}

TEST_F(SshHelpers, FastQueryFilterKeepsDsrWhenDisconnected) {
    // When not connected, the sequence is passed through unchanged.
    ssh_connected = false;
    channel = nullptr;
    const char src[] = "ab\033[5ncd";
    char dst[SSH_RX_BUF_SIZE];
    size_t n = filter_and_reply_fast_queries(src, sizeof(src) - 1, dst);
    EXPECT_EQ(n, sizeof(src) - 1);
}

/* ----------------------- known-host trust store -------------------- */
TEST_F(SshHelpers, KnownHostRemoveNonexistentReturnsFalse) {
    EXPECT_FALSE(ssh_known_host_remove("nohost.example", 22));
}

TEST_F(SshHelpers, KnownHostsClearEmptyReturnsZero) {
    EXPECT_EQ(ssh_known_hosts_clear(), 0);
}
