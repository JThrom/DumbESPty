/* Host test mock of mbedtls/pk.h. Opaque types + stub funcs; the RSA keygen
 * path that uses these is not exercised, it just needs to link. */
#ifndef MOCK_MBEDTLS_PK_H
#define MOCK_MBEDTLS_PK_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { MBEDTLS_PK_NONE = 0, MBEDTLS_PK_RSA = 1 } mbedtls_pk_type_t;

typedef struct { void *opaque; } mbedtls_pk_context;
typedef struct mbedtls_pk_info_t mbedtls_pk_info_t;
typedef struct mbedtls_rsa_context mbedtls_rsa_context;

void mbedtls_pk_init(mbedtls_pk_context *ctx);
void mbedtls_pk_free(mbedtls_pk_context *ctx);
const mbedtls_pk_info_t *mbedtls_pk_info_from_type(mbedtls_pk_type_t type);
int mbedtls_pk_setup(mbedtls_pk_context *ctx, const mbedtls_pk_info_t *info);
mbedtls_rsa_context *mbedtls_pk_rsa(const mbedtls_pk_context ctx);
int mbedtls_pk_write_key_pem(mbedtls_pk_context *ctx, unsigned char *buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif
