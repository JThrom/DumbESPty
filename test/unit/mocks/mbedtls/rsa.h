/* Host test mock of mbedtls/rsa.h. */
#ifndef MOCK_MBEDTLS_RSA_H
#define MOCK_MBEDTLS_RSA_H

#include "pk.h"

#ifdef __cplusplus
extern "C" {
#endif

int mbedtls_rsa_gen_key(mbedtls_rsa_context *ctx,
                        int (*f_rng)(void *, unsigned char *, size_t),
                        void *p_rng, unsigned int nbits, int exponent);

#ifdef __cplusplus
}
#endif

#endif
