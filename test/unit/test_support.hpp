/* Shared helpers for host unit tests. */
#ifndef TEST_SUPPORT_HPP
#define TEST_SUPPORT_HPP

#include <vector>

/* Implemented in mocks/stubs_firmware.cpp */
const char *mock_shell_output(void);
void mock_shell_reset(void);
const std::vector<char> &mock_shell_keys(void);

/* Implemented in mocks/stubs_esp.cpp */
extern "C" void mock_nvs_reset(void);
extern "C" void mock_set_default_mac(const unsigned char mac[6]);

#endif
