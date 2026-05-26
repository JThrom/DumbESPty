#ifndef SECRET_VAULT_HPP
#define SECRET_VAULT_HPP

#include <cstddef>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t secret_vault_init(void);
bool secret_vault_password_is_set(void);
esp_err_t secret_vault_password_set(const char *vault_pass);
esp_err_t secret_vault_password_check(const char *vault_pass);
esp_err_t secret_vault_rekey(const char *old_vault_pass, const char *new_vault_pass);
esp_err_t secret_vault_store(const char *key, const char *value, const char *crypt_pass);
esp_err_t secret_vault_load(const char *key, const char *crypt_pass, char *out, size_t out_len);
esp_err_t secret_vault_remove(const char *key);
bool secret_vault_exists(const char *key);
void cmd_vault(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif
