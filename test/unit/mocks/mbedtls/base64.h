/* Host test mock of mbedtls/base64.h. Real base64 encode implemented in
 * stubs_ssh.cpp so SSH SHA256 fingerprint formatting is exercised correctly. */
#ifndef MOCK_MBEDTLS_BASE64_H
#define MOCK_MBEDTLS_BASE64_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int mbedtls_base64_encode(unsigned char *dst, size_t dlen, size_t *olen,
                          const unsigned char *src, size_t slen);
int mbedtls_base64_decode(unsigned char *dst, size_t dlen, size_t *olen,
                          const unsigned char *src, size_t slen);

#ifdef __cplusplus
}
#endif

#endif
