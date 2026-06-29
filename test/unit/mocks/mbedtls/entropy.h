/* Host test mock of mbedtls/entropy.h. */
#ifndef MOCK_MBEDTLS_ENTROPY_H
#define MOCK_MBEDTLS_ENTROPY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { void *opaque; } mbedtls_entropy_context;

void mbedtls_entropy_init(mbedtls_entropy_context *ctx);
void mbedtls_entropy_free(mbedtls_entropy_context *ctx);
int mbedtls_entropy_func(void *data, unsigned char *output, size_t len);

#ifdef __cplusplus
}
#endif

#endif
