#include "secret_vault.hpp"

#include <cstring>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "shell.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "psa/crypto.h"

static const char *TAG = "vault";

static constexpr char VAULT_NS[] = "vault";
static constexpr char VAULT_META_SALT[] = "_salt";
static constexpr char VAULT_META_HASH[] = "_hash";
static constexpr char VAULT_META_SET[] = "_set";
static constexpr uint32_t VAULT_MAGIC = 0x564C5431;
static constexpr uint16_t VAULT_VERSION = 1;
static constexpr uint32_t VAULT_PBKDF2_ITERS = 20000;

static constexpr size_t SALT_LEN = 16;
static constexpr size_t NONCE_LEN = 12;
static constexpr size_t TAG_LEN = 16;
static constexpr size_t KEY_LEN = 32;
static constexpr size_t MAX_SECRET_LEN = 256;
static constexpr size_t MAX_BLOB_LEN = 1024;
static constexpr int MAX_VAULT_KEYS = 32;
static constexpr uint32_t KDF_YIELD_INTERVAL = 32;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t iterations;
    uint16_t ciphertext_len;
    uint16_t reserved2;
    uint8_t salt[SALT_LEN];
    uint8_t nonce[NONCE_LEN];
    uint8_t tag[TAG_LEN];
} vault_hdr_t;

typedef enum {
    VAULT_PENDING_NONE = 0,
    VAULT_PENDING_REKEY_OLD,
    VAULT_PENDING_REKEY_NEW,
    VAULT_PENDING_REKEY_CONFIRM,
    VAULT_PENDING_REKEY_INIT,
    VAULT_PENDING_REKEY_INIT_CONFIRM,
} vault_pending_t;

static vault_pending_t s_pending = VAULT_PENDING_NONE;
static char s_old_pass[128] = {0};
static char s_new_pass[128] = {0};

static inline void secure_zero(void *p, size_t len) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (len--) *v++ = 0;
}

static bool is_meta_key(const char *key) {
    return strcmp(key, VAULT_META_SALT) == 0 || strcmp(key, VAULT_META_HASH) == 0 || strcmp(key, VAULT_META_SET) == 0;
}

static esp_err_t derive_key(const char *crypt_pass, const uint8_t *salt, size_t salt_len, uint8_t out_key[KEY_LEN]) {
    if (!crypt_pass || !crypt_pass[0]) return ESP_ERR_INVALID_ARG;

    uint8_t digest[32] = {0};
    uint8_t block[160] = {0};
    size_t pass_len = strlen(crypt_pass);
    if (pass_len > 128) pass_len = 128;
    const size_t block_len = pass_len + salt_len;
    memcpy(block, crypt_pass, pass_len);
    memcpy(block + pass_len, salt, salt_len);

    size_t hash_len = 0;
    psa_status_t st = psa_hash_compute(PSA_ALG_SHA_256,
                                       block,
                                       block_len,
                                       digest,
                                       sizeof(digest),
                                       &hash_len);
    if (st != PSA_SUCCESS || hash_len != sizeof(digest)) {
        secure_zero(digest, sizeof(digest));
        secure_zero(block, sizeof(block));
        return ESP_FAIL;
    }

    for (uint32_t i = 1; i < VAULT_PBKDF2_ITERS; i++) {
        uint8_t iter_block[32 + 128 + SALT_LEN] = {0};
        size_t off = 0;
        memcpy(iter_block + off, digest, 32);
        off += 32;
        memcpy(iter_block + off, crypt_pass, pass_len);
        off += pass_len;
        memcpy(iter_block + off, salt, salt_len);
        off += salt_len;

        st = psa_hash_compute(PSA_ALG_SHA_256,
                              iter_block,
                              off,
                              digest,
                              sizeof(digest),
                              &hash_len);
        secure_zero(iter_block, sizeof(iter_block));
        if (st != PSA_SUCCESS || hash_len != sizeof(digest)) {
            secure_zero(digest, sizeof(digest));
            secure_zero(block, sizeof(block));
            return ESP_FAIL;
        }

        if ((i % KDF_YIELD_INTERVAL) == 0) taskYIELD();
    }

    memcpy(out_key, digest, KEY_LEN);
    secure_zero(digest, sizeof(digest));
    secure_zero(block, sizeof(block));
    return ESP_OK;
}

static psa_status_t import_aes_key(const uint8_t key[KEY_LEN], psa_key_usage_t usage, psa_key_id_t *key_id) {
    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attrs, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attrs, 256);
    psa_set_key_usage_flags(&attrs, usage);
    psa_set_key_algorithm(&attrs, PSA_ALG_GCM);
    return psa_import_key(&attrs, key, KEY_LEN, key_id);
}

static esp_err_t read_password_metadata(uint8_t salt[SALT_LEN], uint8_t hash[KEY_LEN], bool *is_set) {
    *is_set = false;
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(VAULT_NS, NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;

    uint8_t setv = 0;
    size_t len = 0;
    err = nvs_get_u8(nvs, VAULT_META_SET, &setv);
    if (err != ESP_OK || setv == 0) {
        nvs_close(nvs);
        return ESP_OK;
    }

    len = SALT_LEN;
    err = nvs_get_blob(nvs, VAULT_META_SALT, salt, &len);
    if (err != ESP_OK || len != SALT_LEN) {
        nvs_close(nvs);
        return ESP_ERR_INVALID_RESPONSE;
    }
    len = KEY_LEN;
    err = nvs_get_blob(nvs, VAULT_META_HASH, hash, &len);
    nvs_close(nvs);
    if (err != ESP_OK || len != KEY_LEN) return ESP_ERR_INVALID_RESPONSE;

    *is_set = true;
    return ESP_OK;
}

static esp_err_t write_password_metadata(const uint8_t salt[SALT_LEN], const uint8_t hash[KEY_LEN]) {
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(VAULT_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(nvs, VAULT_META_SALT, salt, SALT_LEN);
    if (err == ESP_OK) err = nvs_set_blob(nvs, VAULT_META_HASH, hash, KEY_LEN);
    if (err == ESP_OK) err = nvs_set_u8(nvs, VAULT_META_SET, 1);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static void parse_blob_key(nvs_entry_info_t *info, char out[][NVS_KEY_NAME_MAX_SIZE], int *count) {
    if (*count >= MAX_VAULT_KEYS) return;
    if (info->type != NVS_TYPE_BLOB) return;
    if (is_meta_key(info->key)) return;
    snprintf(out[*count], NVS_KEY_NAME_MAX_SIZE, "%s", info->key);
    (*count)++;
}

esp_err_t secret_vault_init(void) {
    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa init failed: %d", (int)st);
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool secret_vault_password_is_set(void) {
    uint8_t salt[SALT_LEN] = {0};
    uint8_t hash[KEY_LEN] = {0};
    bool set = false;
    esp_err_t err = read_password_metadata(salt, hash, &set);
    secure_zero(salt, sizeof(salt));
    secure_zero(hash, sizeof(hash));
    return err == ESP_OK && set;
}

esp_err_t secret_vault_password_set(const char *vault_pass) {
    if (!vault_pass || !vault_pass[0]) return ESP_ERR_INVALID_ARG;
    if (secret_vault_password_is_set()) return ESP_ERR_INVALID_STATE;

    uint8_t salt[SALT_LEN] = {0};
    uint8_t hash[KEY_LEN] = {0};
    esp_fill_random(salt, SALT_LEN);
    esp_err_t err = derive_key(vault_pass, salt, SALT_LEN, hash);
    if (err == ESP_OK) err = write_password_metadata(salt, hash);
    secure_zero(salt, sizeof(salt));
    secure_zero(hash, sizeof(hash));
    return err;
}

esp_err_t secret_vault_password_check(const char *vault_pass) {
    if (!vault_pass || !vault_pass[0]) return ESP_ERR_INVALID_ARG;
    uint8_t salt[SALT_LEN] = {0};
    uint8_t stored[KEY_LEN] = {0};
    uint8_t derived[KEY_LEN] = {0};
    bool set = false;
    esp_err_t err = read_password_metadata(salt, stored, &set);
    if (err != ESP_OK) {
        secure_zero(salt, sizeof(salt));
        secure_zero(stored, sizeof(stored));
        return err;
    }
    if (!set) {
        secure_zero(salt, sizeof(salt));
        secure_zero(stored, sizeof(stored));
        return ESP_ERR_INVALID_STATE;
    }
    err = derive_key(vault_pass, salt, SALT_LEN, derived);
    if (err != ESP_OK) {
        secure_zero(salt, sizeof(salt));
        secure_zero(stored, sizeof(stored));
        secure_zero(derived, sizeof(derived));
        return err;
    }
    bool same = memcmp(stored, derived, KEY_LEN) == 0;
    secure_zero(salt, sizeof(salt));
    secure_zero(stored, sizeof(stored));
    secure_zero(derived, sizeof(derived));
    return same ? ESP_OK : ESP_ERR_INVALID_CRC;
}

esp_err_t secret_vault_store(const char *key, const char *value, const char *vault_pass) {
    if (!key || !key[0] || !value || !vault_pass || !vault_pass[0]) return ESP_ERR_INVALID_ARG;
    if (is_meta_key(key)) return ESP_ERR_INVALID_ARG;

    esp_err_t err = secret_vault_password_check(vault_pass);
    if (err != ESP_OK) return err;

    const size_t value_len = strnlen(value, MAX_SECRET_LEN + 1);
    if (value_len == 0 || value_len > MAX_SECRET_LEN) return ESP_ERR_INVALID_SIZE;

    uint8_t aes_key[KEY_LEN] = {0};
    uint8_t ciphertext[MAX_SECRET_LEN] = {0};
    uint8_t aead_out[MAX_SECRET_LEN + TAG_LEN] = {0};
    vault_hdr_t hdr = {};

    esp_fill_random(hdr.salt, SALT_LEN);
    esp_fill_random(hdr.nonce, NONCE_LEN);
    hdr.magic = VAULT_MAGIC;
    hdr.version = VAULT_VERSION;
    hdr.iterations = VAULT_PBKDF2_ITERS;
    hdr.ciphertext_len = (uint16_t)value_len;

    err = derive_key(vault_pass, hdr.salt, SALT_LEN, aes_key);
    if (err != ESP_OK) {
        secure_zero(aes_key, sizeof(aes_key));
        return err;
    }

    size_t aead_out_len = 0;
    psa_key_id_t key_id = 0;
    psa_status_t st = import_aes_key(aes_key, PSA_KEY_USAGE_ENCRYPT, &key_id);
    if (st == PSA_SUCCESS) {
        st = psa_aead_encrypt(key_id,
                              PSA_ALG_GCM,
                              hdr.nonce,
                              NONCE_LEN,
                              NULL,
                              0,
                              (const uint8_t *)value,
                              value_len,
                              aead_out,
                              sizeof(aead_out),
                              &aead_out_len);
        psa_destroy_key(key_id);
    }
    if (st != PSA_SUCCESS || aead_out_len != value_len + TAG_LEN) {
        secure_zero(aes_key, sizeof(aes_key));
        secure_zero(aead_out, sizeof(aead_out));
        return ESP_FAIL;
    }

    memcpy(ciphertext, aead_out, value_len);
    memcpy(hdr.tag, aead_out + value_len, TAG_LEN);

    uint8_t blob[MAX_BLOB_LEN] = {0};
    const size_t blob_len = sizeof(hdr) + value_len;
    if (blob_len > sizeof(blob)) {
        secure_zero(aes_key, sizeof(aes_key));
        secure_zero(ciphertext, sizeof(ciphertext));
        secure_zero(aead_out, sizeof(aead_out));
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(blob, &hdr, sizeof(hdr));
    memcpy(blob + sizeof(hdr), ciphertext, value_len);

    nvs_handle_t nvs = 0;
    err = nvs_open(VAULT_NS, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, key, blob, blob_len);
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }

    secure_zero(aes_key, sizeof(aes_key));
    secure_zero(ciphertext, sizeof(ciphertext));
    secure_zero(aead_out, sizeof(aead_out));
    secure_zero(blob, sizeof(blob));
    return err;
}

esp_err_t secret_vault_load(const char *key, const char *vault_pass, char *out, size_t out_cap) {
    if (!key || !key[0] || !vault_pass || !vault_pass[0] || !out || out_cap == 0) return ESP_ERR_INVALID_ARG;
    if (is_meta_key(key)) return ESP_ERR_INVALID_ARG;

    esp_err_t err = secret_vault_password_check(vault_pass);
    if (err != ESP_OK) return err;

    uint8_t blob[MAX_BLOB_LEN] = {0};
    size_t blob_len = sizeof(blob);
    nvs_handle_t nvs = 0;
    err = nvs_open(VAULT_NS, NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_get_blob(nvs, key, blob, &blob_len);
    nvs_close(nvs);
    if (err != ESP_OK) return err;

    if (blob_len < sizeof(vault_hdr_t)) {
        secure_zero(blob, sizeof(blob));
        return ESP_ERR_INVALID_SIZE;
    }

    vault_hdr_t hdr = {};
    memcpy(&hdr, blob, sizeof(hdr));
    if (hdr.magic != VAULT_MAGIC || hdr.version != VAULT_VERSION || hdr.ciphertext_len == 0) {
        secure_zero(blob, sizeof(blob));
        return ESP_ERR_INVALID_RESPONSE;
    }
    if ((size_t)hdr.ciphertext_len + sizeof(hdr) != blob_len || hdr.ciphertext_len >= out_cap) {
        secure_zero(blob, sizeof(blob));
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t *ciphertext = blob + sizeof(hdr);
    uint8_t aes_key[KEY_LEN] = {0};
    err = derive_key(vault_pass, hdr.salt, SALT_LEN, aes_key);
    if (err != ESP_OK) {
        secure_zero(blob, sizeof(blob));
        secure_zero(aes_key, sizeof(aes_key));
        return err;
    }

    uint8_t aead_in[MAX_SECRET_LEN + TAG_LEN] = {0};
    memcpy(aead_in, ciphertext, hdr.ciphertext_len);
    memcpy(aead_in + hdr.ciphertext_len, hdr.tag, TAG_LEN);

    size_t dec_len = 0;
    psa_key_id_t key_id = 0;
    psa_status_t st = import_aes_key(aes_key, PSA_KEY_USAGE_DECRYPT, &key_id);
    if (st == PSA_SUCCESS) {
        st = psa_aead_decrypt(key_id,
                              PSA_ALG_GCM,
                              hdr.nonce,
                              NONCE_LEN,
                              NULL,
                              0,
                              aead_in,
                              hdr.ciphertext_len + TAG_LEN,
                              (uint8_t *)out,
                              out_cap,
                              &dec_len);
        psa_destroy_key(key_id);
    }

    secure_zero(blob, sizeof(blob));
    secure_zero(aes_key, sizeof(aes_key));
    secure_zero(aead_in, sizeof(aead_in));

    if (st != PSA_SUCCESS || dec_len != hdr.ciphertext_len) {
        secure_zero(out, out_cap);
        return ESP_ERR_INVALID_CRC;
    }

    out[hdr.ciphertext_len] = '\0';
    return ESP_OK;
}

esp_err_t secret_vault_remove(const char *key) {
    if (!key || !key[0]) return ESP_ERR_INVALID_ARG;
    if (is_meta_key(key)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(VAULT_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(nvs, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

bool secret_vault_exists(const char *key) {
    if (!key || !key[0] || is_meta_key(key)) return false;
    nvs_handle_t nvs = 0;
    if (nvs_open(VAULT_NS, NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t len = 0;
    esp_err_t err = nvs_get_blob(nvs, key, NULL, &len);
    nvs_close(nvs);
    return err == ESP_OK && len > 0;
}

esp_err_t secret_vault_rekey(const char *old_vault_pass, const char *new_vault_pass) {
    if (!new_vault_pass || !new_vault_pass[0]) return ESP_ERR_INVALID_ARG;

    if (!secret_vault_password_is_set()) {
        return secret_vault_password_set(new_vault_pass);
    }

    esp_err_t err = secret_vault_password_check(old_vault_pass);
    if (err != ESP_OK) return err;

    char keys[MAX_VAULT_KEYS][NVS_KEY_NAME_MAX_SIZE] = {};
    int key_count = 0;

    nvs_iterator_t it = NULL;
    esp_err_t it_err = nvs_entry_find(NULL, VAULT_NS, NVS_TYPE_BLOB, &it);
    while (it_err == ESP_OK && it != NULL) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        parse_blob_key(&info, keys, &key_count);
        it_err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);

    for (int i = 0; i < key_count; i++) {
        char plain[MAX_SECRET_LEN + 1] = {0};
        err = secret_vault_load(keys[i], old_vault_pass, plain, sizeof(plain));
        if (err != ESP_OK) {
            secure_zero(plain, sizeof(plain));
            return err;
        }

        uint8_t set_salt[SALT_LEN] = {0};
        uint8_t new_hash[KEY_LEN] = {0};
        esp_fill_random(set_salt, SALT_LEN);
        err = derive_key(new_vault_pass, set_salt, SALT_LEN, new_hash);
        if (err != ESP_OK) {
            secure_zero(plain, sizeof(plain));
            secure_zero(set_salt, sizeof(set_salt));
            secure_zero(new_hash, sizeof(new_hash));
            return err;
        }
        err = write_password_metadata(set_salt, new_hash);
        secure_zero(set_salt, sizeof(set_salt));
        secure_zero(new_hash, sizeof(new_hash));
        if (err != ESP_OK) {
            secure_zero(plain, sizeof(plain));
            return err;
        }

        err = secret_vault_store(keys[i], plain, new_vault_pass);
        secure_zero(plain, sizeof(plain));
        if (err != ESP_OK) return err;
    }

    uint8_t final_salt[SALT_LEN] = {0};
    uint8_t final_hash[KEY_LEN] = {0};
    esp_fill_random(final_salt, SALT_LEN);
    err = derive_key(new_vault_pass, final_salt, SALT_LEN, final_hash);
    if (err != ESP_OK) {
        secure_zero(final_salt, sizeof(final_salt));
        secure_zero(final_hash, sizeof(final_hash));
        return err;
    }
    err = write_password_metadata(final_salt, final_hash);
    secure_zero(final_salt, sizeof(final_salt));
    secure_zero(final_hash, sizeof(final_hash));
    return err;
}

static void vault_reset_pending(void) {
    s_pending = VAULT_PENDING_NONE;
    secure_zero(s_old_pass, sizeof(s_old_pass));
    secure_zero(s_new_pass, sizeof(s_new_pass));
}

static void vault_rekey_input_cb(const char *input) {
    if (s_pending == VAULT_PENDING_REKEY_INIT) {
        snprintf(s_new_pass, sizeof(s_new_pass), "%s", input);
        s_pending = VAULT_PENDING_REKEY_INIT_CONFIRM;
        shell_print("\r\n  Confirm password: ");
        shell_get_hidden_input(vault_rekey_input_cb);
        return;
    }
    if (s_pending == VAULT_PENDING_REKEY_INIT_CONFIRM) {
        if (strcmp(s_new_pass, input) != 0) {
            shell_print("\r\n  new passwords do not match");
            vault_reset_pending();
            return;
        }
        esp_err_t err = secret_vault_rekey(NULL, s_new_pass);
        shell_print(err == ESP_OK ? "\r\n  vault password set" : "\r\n  vault password set failed");
        vault_reset_pending();
        return;
    }
    if (s_pending == VAULT_PENDING_REKEY_OLD) {
        snprintf(s_old_pass, sizeof(s_old_pass), "%s", input);
        s_pending = VAULT_PENDING_REKEY_NEW;
        shell_print("\r\n  Vault password: ");
        shell_get_hidden_input(vault_rekey_input_cb);
        return;
    }
    if (s_pending == VAULT_PENDING_REKEY_NEW) {
        snprintf(s_new_pass, sizeof(s_new_pass), "%s", input);
        s_pending = VAULT_PENDING_REKEY_CONFIRM;
        shell_print("\r\n  Confirm password: ");
        shell_get_hidden_input(vault_rekey_input_cb);
        return;
    }
    if (s_pending == VAULT_PENDING_REKEY_CONFIRM) {
        if (strcmp(s_new_pass, input) != 0) {
            shell_print("\r\n  new passwords do not match");
            vault_reset_pending();
            return;
        }
        esp_err_t err = secret_vault_rekey(s_old_pass, s_new_pass);
        if (err == ESP_OK) shell_print("\r\n  vault rekey complete");
        else if (err == ESP_ERR_INVALID_CRC) shell_print("\r\n  invalid existing vault password");
        else shell_print("\r\n  vault rekey failed");
        vault_reset_pending();
    }
}

void cmd_vault(int argc, char **argv) {
    if (argc < 2) {
        shell_print("\r\n  usage: vault status|rekey");
        return;
    }
    if (strcmp(argv[1], "status") == 0) {
        shell_print(secret_vault_password_is_set() ? "\r\n  vault password: set" : "\r\n  vault password: unset");
        return;
    }
    if (strcmp(argv[1], "rekey") == 0) {
        if (!secret_vault_password_is_set()) {
            s_pending = VAULT_PENDING_REKEY_INIT;
            shell_print("\r\n  Vault password: ");
            shell_get_hidden_input(vault_rekey_input_cb);
            return;
        }
        s_pending = VAULT_PENDING_REKEY_OLD;
        shell_print("\r\n  Vault password: ");
        shell_get_hidden_input(vault_rekey_input_cb);
        return;
    }
    shell_print("\r\n  usage: vault status|rekey");
}
