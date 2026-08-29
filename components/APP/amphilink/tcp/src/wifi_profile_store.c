#include "wifi_profile_store.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_mac.h"
#include "esp_random.h"
#include "flash_user.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "psa/crypto.h"

#define WIFI_NVS_PARTITION "wifi_nvs"
#define WIFI_NVS_NAMESPACE "wifi"
#define WIFI_NVS_PROVISIONED_SSID_KEY "ssid"
#define WIFI_NVS_PROVISIONED_PASSWORD_KEY "password"
#define WIFI_PROFILE_V1_BLOB_KEY "profiles_v1"
#define WIFI_PROFILE_V2_BLOB_KEY "profiles_v2"
#define WIFI_PROFILE_STAGE_NAMESPACE "wifi_migrate"
#define WIFI_PROFILE_STAGE_KEY "profiles_v2"

#define WIFI_PROFILE_PAYLOAD_MAGIC 0x32464357U
#define WIFI_PROFILE_PAYLOAD_HEADER_SIZE 16U
#define WIFI_PROFILE_RECORD_SIZE 103U
#define WIFI_PROFILE_PAYLOAD_MAX_SIZE                                      \
    (WIFI_PROFILE_PAYLOAD_HEADER_SIZE +                                    \
     (WIFI_PROFILE_MAX_COUNT * WIFI_PROFILE_RECORD_SIZE))

#define WIFI_PROFILE_ENVELOPE_MAGIC 0x32454357U
#define WIFI_PROFILE_SCHEMA_VERSION 2U
#define WIFI_PROFILE_KDF_PBKDF2_SHA256 1U
#define WIFI_PROFILE_CIPHER_AES_256_GCM 1U
#define WIFI_PROFILE_KDF_ITERATIONS 100000U
#define WIFI_PROFILE_ENVELOPE_HEADER_SIZE 48U
#define WIFI_PROFILE_SALT_SIZE 16U
#define WIFI_PROFILE_NONCE_SIZE 12U
#define WIFI_PROFILE_TAG_SIZE 16U
#define WIFI_PROFILE_KEY_SIZE 32U
#define WIFI_PROFILE_DEVICE_ID_SIZE 6U
#define WIFI_PROFILE_BLOB_MAX_SIZE                                         \
    (WIFI_PROFILE_ENVELOPE_HEADER_SIZE + WIFI_PROFILE_PAYLOAD_MAX_SIZE +   \
     WIFI_PROFILE_TAG_SIZE)

typedef struct {
    uint32_t revision;
    uint32_t next_id;
    size_t count;
    wifi_profile_t profiles[WIFI_PROFILE_MAX_COUNT];
} wifi_profile_store_t;

static SemaphoreHandle_t s_mutex;
static wifi_profile_store_t s_store;
static uint8_t s_salt[WIFI_PROFILE_SALT_SIZE];
static uint8_t s_key[WIFI_PROFILE_KEY_SIZE];
static const char *s_admin_password;
static bool s_locked;
static bool s_initialized;

static void secure_zero(void *data, size_t size)
{
    volatile uint8_t *bytes = data;
    while (size-- > 0U) {
        *bytes++ = 0U;
    }
}

static uint16_t read_u16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8U);
}

static uint32_t read_u32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) | ((uint32_t)data[3] << 24U);
}

static void write_u16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8U);
}

static void write_u32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8U);
    data[2] = (uint8_t)(value >> 16U);
    data[3] = (uint8_t)(value >> 24U);
}

static bool valid_utf8(const uint8_t *data, size_t size)
{
    for (size_t index = 0U; index < size;) {
        uint8_t first = data[index++];
        if (first < 0x80U) {
            continue;
        }
        uint32_t codepoint;
        size_t continuation_count;
        if ((first >= 0xC2U) && (first <= 0xDFU)) {
            codepoint = first & 0x1FU;
            continuation_count = 1U;
        } else if ((first >= 0xE0U) && (first <= 0xEFU)) {
            codepoint = first & 0x0FU;
            continuation_count = 2U;
        } else if ((first >= 0xF0U) && (first <= 0xF4U)) {
            codepoint = first & 0x07U;
            continuation_count = 3U;
        } else {
            return false;
        }
        if ((size - index) < continuation_count) {
            return false;
        }
        for (size_t continuation = 0U; continuation < continuation_count;
             ++continuation) {
            uint8_t byte = data[index++];
            if ((byte & 0xC0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (byte & 0x3FU);
        }
        if (((continuation_count == 2U) && (codepoint < 0x800U)) ||
            ((continuation_count == 3U) && (codepoint < 0x10000U)) ||
            ((codepoint >= 0xD800U) && (codepoint <= 0xDFFFU)) ||
            (codepoint > 0x10FFFFU)) {
            return false;
        }
    }
    return true;
}

esp_err_t wifi_profile_validate(const char *ssid, const char *password)
{
    if ((ssid == NULL) || (password == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t ssid_length = strnlen(ssid, WIFI_PROFILE_SSID_SIZE);
    size_t password_length = strnlen(password, WIFI_PROFILE_PASSWORD_SIZE);
    if ((ssid_length == 0U) || (ssid_length >= WIFI_PROFILE_SSID_SIZE) ||
        !valid_utf8((const uint8_t *)ssid, ssid_length)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (password_length >= WIFI_PROFILE_PASSWORD_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    if ((password_length != 0U) && (password_length < 8U)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static void initialize_empty_store(wifi_profile_store_t *store)
{
    memset(store, 0, sizeof(*store));
    store->revision = 1U;
    store->next_id = 1U;
}

static void copy_snapshot(const wifi_profile_store_t *store,
                          wifi_profile_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->locked = s_locked;
    if (s_locked) {
        return;
    }
    snapshot->revision = store->revision;
    snapshot->count = store->count;
    memcpy(snapshot->profiles, store->profiles,
           store->count * sizeof(store->profiles[0]));
}

static int find_profile(const wifi_profile_store_t *store, uint32_t id)
{
    for (size_t index = 0U; index < store->count; ++index) {
        if (store->profiles[index].id == id) {
            return (int)index;
        }
    }
    return -1;
}

static int find_ssid(const wifi_profile_store_t *store, const char *ssid)
{
    for (size_t index = 0U; index < store->count; ++index) {
        if (strcmp(store->profiles[index].ssid, ssid) == 0) {
            return (int)index;
        }
    }
    return -1;
}

static uint32_t allocate_id(wifi_profile_store_t *store)
{
    uint32_t id = (store->next_id == 0U) ? 1U : store->next_id;
    while (find_profile(store, id) >= 0) {
        if (++id == 0U) {
            id = 1U;
        }
    }
    store->next_id = id + 1U;
    if (store->next_id == 0U) {
        store->next_id = 1U;
    }
    return id;
}

static esp_err_t serialize_payload(const wifi_profile_store_t *store,
                                   uint8_t *payload, size_t payload_size,
                                   size_t *bytes_written)
{
    size_t required = WIFI_PROFILE_PAYLOAD_HEADER_SIZE +
                      (store->count * WIFI_PROFILE_RECORD_SIZE);
    if ((payload == NULL) || (bytes_written == NULL) ||
        (store->count > WIFI_PROFILE_MAX_COUNT) ||
        (payload_size < required)) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(payload, 0, required);
    write_u32(payload, WIFI_PROFILE_PAYLOAD_MAGIC);
    write_u16(payload + 4U, WIFI_PROFILE_SCHEMA_VERSION);
    write_u16(payload + 6U, (uint16_t)store->count);
    write_u32(payload + 8U, store->revision);
    write_u32(payload + 12U, store->next_id);
    size_t offset = WIFI_PROFILE_PAYLOAD_HEADER_SIZE;
    for (size_t index = 0U; index < store->count; ++index) {
        const wifi_profile_t *profile = &store->profiles[index];
        size_t ssid_length = strlen(profile->ssid);
        size_t password_length = strlen(profile->password);
        write_u32(payload + offset, profile->id);
        payload[offset + 4U] = (uint8_t)ssid_length;
        payload[offset + 5U] = (uint8_t)password_length;
        memcpy(payload + offset + 8U, profile->ssid, ssid_length);
        memcpy(payload + offset + 40U, profile->password, password_length);
        offset += WIFI_PROFILE_RECORD_SIZE;
    }
    *bytes_written = required;
    return ESP_OK;
}

static esp_err_t deserialize_payload(const uint8_t *payload,
                                     size_t payload_size,
                                     wifi_profile_store_t *store)
{
    if ((payload == NULL) || (store == NULL) ||
        (payload_size < WIFI_PROFILE_PAYLOAD_HEADER_SIZE) ||
        (read_u32(payload) != WIFI_PROFILE_PAYLOAD_MAGIC) ||
        (read_u16(payload + 4U) != WIFI_PROFILE_SCHEMA_VERSION)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    size_t count = read_u16(payload + 6U);
    if ((count > WIFI_PROFILE_MAX_COUNT) ||
        (payload_size != WIFI_PROFILE_PAYLOAD_HEADER_SIZE +
                            (count * WIFI_PROFILE_RECORD_SIZE))) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = ESP_OK;
    wifi_profile_store_t decoded = {
        .revision = read_u32(payload + 8U),
        .next_id = read_u32(payload + 12U),
        .count = count,
    };
    size_t offset = WIFI_PROFILE_PAYLOAD_HEADER_SIZE;
    for (size_t index = 0U; index < count; ++index) {
        size_t ssid_length = payload[offset + 4U];
        size_t password_length = payload[offset + 5U];
        if ((ssid_length == 0U) || (ssid_length > 32U) ||
            (password_length > 63U)) {
            err = ESP_ERR_INVALID_SIZE;
            goto finished;
        }
        wifi_profile_t *profile = &decoded.profiles[index];
        profile->id = read_u32(payload + offset);
        memcpy(profile->ssid, payload + offset + 8U, ssid_length);
        memcpy(profile->password, payload + offset + 40U, password_length);
        if ((profile->id == 0U) ||
            (wifi_profile_validate(profile->ssid, profile->password) !=
             ESP_OK) ||
            (find_profile(&decoded, profile->id) != (int)index) ||
            (find_ssid(&decoded, profile->ssid) != (int)index)) {
            err = ESP_ERR_INVALID_RESPONSE;
            goto finished;
        }
        offset += WIFI_PROFILE_RECORD_SIZE;
    }
    if (decoded.revision == 0U) {
        decoded.revision = 1U;
    }
    if (decoded.next_id == 0U) {
        decoded.next_id = 1U;
    }
    *store = decoded;

finished:
    secure_zero(&decoded, sizeof(decoded));
    return err;
}

static esp_err_t derive_key(const char *password, const uint8_t *salt,
                            uint8_t *key)
{
    uint8_t device_id[WIFI_PROFILE_DEVICE_ID_SIZE];
    if (esp_read_mac(device_id, ESP_MAC_WIFI_STA) != ESP_OK) {
        return ESP_FAIL;
    }
    psa_key_derivation_operation_t operation =
        PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t status = psa_key_derivation_setup(
        &operation, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_integer(
            &operation, PSA_KEY_DERIVATION_INPUT_COST,
            WIFI_PROFILE_KDF_ITERATIONS);
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(
            &operation, PSA_KEY_DERIVATION_INPUT_SALT, salt,
            WIFI_PROFILE_SALT_SIZE);
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(
            &operation, PSA_KEY_DERIVATION_INPUT_SALT, device_id,
            sizeof(device_id));
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(
            &operation, PSA_KEY_DERIVATION_INPUT_PASSWORD,
            (const uint8_t *)password, strlen(password));
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_output_bytes(&operation, key,
                                                 WIFI_PROFILE_KEY_SIZE);
    }
    (void)psa_key_derivation_abort(&operation);
    secure_zero(device_id, sizeof(device_id));
    return (status == PSA_SUCCESS) ? ESP_OK : ESP_FAIL;
}

static esp_err_t import_aead_key(const uint8_t *key, psa_key_id_t *key_id)
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attributes,
                            PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, WIFI_PROFILE_KEY_SIZE * 8U);
    psa_status_t status =
        psa_import_key(&attributes, key, WIFI_PROFILE_KEY_SIZE, key_id);
    psa_reset_key_attributes(&attributes);
    return (status == PSA_SUCCESS) ? ESP_OK : ESP_FAIL;
}

static esp_err_t encrypt_store(const wifi_profile_store_t *store,
                               const uint8_t *salt, const uint8_t *key,
                               uint8_t *blob, size_t blob_size,
                               size_t *bytes_written)
{
    uint8_t *payload = calloc(1U, WIFI_PROFILE_PAYLOAD_MAX_SIZE);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t payload_size = 0U;
    esp_err_t err = serialize_payload(store, payload,
                                      WIFI_PROFILE_PAYLOAD_MAX_SIZE,
                                      &payload_size);
    size_t required = WIFI_PROFILE_ENVELOPE_HEADER_SIZE + payload_size +
                      WIFI_PROFILE_TAG_SIZE;
    if ((err == ESP_OK) &&
        ((blob == NULL) || (bytes_written == NULL) || (blob_size < required))) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) {
        memset(blob, 0, required);
        write_u32(blob, WIFI_PROFILE_ENVELOPE_MAGIC);
        write_u16(blob + 4U, WIFI_PROFILE_SCHEMA_VERSION);
        blob[6U] = WIFI_PROFILE_KDF_PBKDF2_SHA256;
        blob[7U] = WIFI_PROFILE_CIPHER_AES_256_GCM;
        write_u32(blob + 8U, WIFI_PROFILE_KDF_ITERATIONS);
        write_u16(blob + 12U, (uint16_t)payload_size);
        memcpy(blob + 16U, salt, WIFI_PROFILE_SALT_SIZE);
        esp_fill_random(blob + 32U, WIFI_PROFILE_NONCE_SIZE);
        psa_key_id_t key_id = 0;
        err = import_aead_key(key, &key_id);
        if (err == ESP_OK) {
            size_t encrypted_size = 0U;
            psa_status_t status = psa_aead_encrypt(
                key_id, PSA_ALG_GCM, blob + 32U, WIFI_PROFILE_NONCE_SIZE, blob,
                WIFI_PROFILE_ENVELOPE_HEADER_SIZE, payload, payload_size,
                blob + WIFI_PROFILE_ENVELOPE_HEADER_SIZE,
                blob_size - WIFI_PROFILE_ENVELOPE_HEADER_SIZE,
                &encrypted_size);
            if ((status != PSA_SUCCESS) ||
                (encrypted_size != payload_size + WIFI_PROFILE_TAG_SIZE)) {
                err = ESP_FAIL;
            } else {
                *bytes_written = WIFI_PROFILE_ENVELOPE_HEADER_SIZE +
                                 encrypted_size;
            }
            (void)psa_destroy_key(key_id);
        }
    }
    secure_zero(payload, WIFI_PROFILE_PAYLOAD_MAX_SIZE);
    free(payload);
    return err;
}

static esp_err_t decrypt_store(const uint8_t *blob, size_t blob_size,
                               const char *password,
                               wifi_profile_store_t *store, uint8_t *salt,
                               uint8_t *key)
{
    if ((blob == NULL) || (store == NULL) || (salt == NULL) || (key == NULL) ||
        (blob_size < WIFI_PROFILE_ENVELOPE_HEADER_SIZE +
                         WIFI_PROFILE_TAG_SIZE) ||
        (read_u32(blob) != WIFI_PROFILE_ENVELOPE_MAGIC) ||
        (read_u16(blob + 4U) != WIFI_PROFILE_SCHEMA_VERSION) ||
        (blob[6U] != WIFI_PROFILE_KDF_PBKDF2_SHA256) ||
        (blob[7U] != WIFI_PROFILE_CIPHER_AES_256_GCM) ||
        (read_u32(blob + 8U) != WIFI_PROFILE_KDF_ITERATIONS)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    size_t payload_size = read_u16(blob + 12U);
    if ((payload_size < WIFI_PROFILE_PAYLOAD_HEADER_SIZE) ||
        (payload_size > WIFI_PROFILE_PAYLOAD_MAX_SIZE) ||
        (blob_size != WIFI_PROFILE_ENVELOPE_HEADER_SIZE + payload_size +
                          WIFI_PROFILE_TAG_SIZE)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(salt, blob + 16U, WIFI_PROFILE_SALT_SIZE);
    esp_err_t err = derive_key(password, salt, key);
    uint8_t *payload = calloc(1U, payload_size);
    if ((err == ESP_OK) && (payload == NULL)) {
        err = ESP_ERR_NO_MEM;
    }
    psa_key_id_t key_id = 0;
    if (err == ESP_OK) {
        err = import_aead_key(key, &key_id);
    }
    if (err == ESP_OK) {
        size_t decrypted_size = 0U;
        psa_status_t status = psa_aead_decrypt(
            key_id, PSA_ALG_GCM, blob + 32U, WIFI_PROFILE_NONCE_SIZE, blob,
            WIFI_PROFILE_ENVELOPE_HEADER_SIZE,
            blob + WIFI_PROFILE_ENVELOPE_HEADER_SIZE,
            payload_size + WIFI_PROFILE_TAG_SIZE, payload, payload_size,
            &decrypted_size);
        if (status == PSA_ERROR_INVALID_SIGNATURE) {
            err = ESP_ERR_INVALID_CRC;
        } else if ((status != PSA_SUCCESS) ||
                   (decrypted_size != payload_size)) {
            err = ESP_ERR_INVALID_RESPONSE;
        } else {
            err = deserialize_payload(payload, payload_size, store);
        }
        (void)psa_destroy_key(key_id);
    }
    if (payload != NULL) {
        secure_zero(payload, payload_size);
        free(payload);
    }
    if (err != ESP_OK) {
        secure_zero(key, WIFI_PROFILE_KEY_SIZE);
    }
    return err;
}

static bool not_found(esp_err_t err)
{
    return err == ESP_ERR_NOT_FOUND;
}

static esp_err_t read_partition_blob(const char *key, uint8_t *blob,
                                     size_t *blob_size)
{
    return flash_user_nvs_partition_get_blob(
        WIFI_NVS_PARTITION, WIFI_NVS_NAMESPACE, key, blob,
        WIFI_PROFILE_BLOB_MAX_SIZE, blob_size);
}

static esp_err_t read_stage_blob(uint8_t *blob, size_t *blob_size)
{
    return flash_user_nvs_get_blob(WIFI_PROFILE_STAGE_NAMESPACE,
                                   WIFI_PROFILE_STAGE_KEY, blob,
                                   WIFI_PROFILE_BLOB_MAX_SIZE, blob_size);
}

static esp_err_t erase_and_init_partition(void)
{
    esp_err_t err = flash_user_nvs_partition_erase(WIFI_NVS_PARTITION);
    return (err == ESP_OK)
               ? flash_user_nvs_partition_init(WIFI_NVS_PARTITION, false)
               : err;
}

static esp_err_t write_v2_blob(const uint8_t *blob, size_t blob_size)
{
    return flash_user_nvs_partition_set_blob(
        WIFI_NVS_PARTITION, WIFI_NVS_NAMESPACE, WIFI_PROFILE_V2_BLOB_KEY, blob,
        blob_size);
}

static esp_err_t persist_store(wifi_profile_store_t *candidate)
{
    if (s_locked) {
        return ESP_ERR_INVALID_CRC;
    }
    uint8_t *blob = malloc(WIFI_PROFILE_BLOB_MAX_SIZE);
    if (blob == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (++candidate->revision == 0U) {
        candidate->revision = 1U;
    }
    size_t blob_size = 0U;
    esp_err_t err = encrypt_store(candidate, s_salt, s_key, blob,
                                  WIFI_PROFILE_BLOB_MAX_SIZE, &blob_size);
    if (err == ESP_OK) {
        err = write_v2_blob(blob, blob_size);
    }
    if (err == ESP_OK) {
        s_store = *candidate;
    }
    secure_zero(blob, WIFI_PROFILE_BLOB_MAX_SIZE);
    free(blob);
    return err;
}

static esp_err_t create_empty_encrypted_store(void)
{
    wifi_profile_store_t empty;
    initialize_empty_store(&empty);
    esp_fill_random(s_salt, sizeof(s_salt));
    esp_err_t err = derive_key(s_admin_password, s_salt, s_key);
    uint8_t *blob = malloc(WIFI_PROFILE_BLOB_MAX_SIZE);
    size_t blob_size = 0U;
    if ((err == ESP_OK) && (blob == NULL)) {
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        err = encrypt_store(&empty, s_salt, s_key, blob,
                            WIFI_PROFILE_BLOB_MAX_SIZE, &blob_size);
    }
    if (err == ESP_OK) {
        err = write_v2_blob(blob, blob_size);
    }
    if (err == ESP_OK) {
        s_store = empty;
        s_locked = false;
    }
    if (blob != NULL) {
        secure_zero(blob, WIFI_PROFILE_BLOB_MAX_SIZE);
        free(blob);
    }
    secure_zero(&empty, sizeof(empty));
    return err;
}

static esp_err_t recover_staged_store(uint8_t *blob,
                                      wifi_profile_store_t *loaded,
                                      bool *recovered)
{
    size_t blob_size = 0U;
    esp_err_t err = read_stage_blob(blob, &blob_size);
    if (not_found(err)) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    err = decrypt_store(blob, blob_size, s_admin_password, loaded, s_salt,
                        s_key);
    if ((err == ESP_ERR_INVALID_CRC) || (err == ESP_ERR_INVALID_RESPONSE) ||
        (err == ESP_ERR_INVALID_SIZE)) {
        s_locked = true;
        *recovered = true;
        return ESP_OK;
    }
    if (err == ESP_OK) {
        err = erase_and_init_partition();
    }
    if (err == ESP_OK) {
        err = write_v2_blob(blob, blob_size);
    }
    if (err == ESP_OK) {
        esp_err_t erase_err = flash_user_nvs_erase_key(
            WIFI_PROFILE_STAGE_NAMESPACE, WIFI_PROFILE_STAGE_KEY);
        if ((erase_err != ESP_OK) && !not_found(erase_err)) {
            err = erase_err;
        }
    }
    if (err == ESP_OK) {
        *recovered = true;
    }
    return err;
}

static esp_err_t load_provisioned_profile(wifi_profile_store_t *store)
{
    char ssid[WIFI_PROFILE_SSID_SIZE] = {0};
    char password[WIFI_PROFILE_PASSWORD_SIZE] = {0};
    esp_err_t err = flash_user_nvs_partition_get_string(
        WIFI_NVS_PARTITION, WIFI_NVS_NAMESPACE,
        WIFI_NVS_PROVISIONED_SSID_KEY, ssid, sizeof(ssid), NULL);
    if (err == ESP_OK) {
        err = flash_user_nvs_partition_get_string(
            WIFI_NVS_PARTITION, WIFI_NVS_NAMESPACE,
            WIFI_NVS_PROVISIONED_PASSWORD_KEY, password, sizeof(password),
            NULL);
    }
    if (err == ESP_OK) {
        err = wifi_profile_validate(ssid, password);
    }
    if (err == ESP_OK) {
        store->profiles[0].id = allocate_id(store);
        strcpy(store->profiles[0].ssid, ssid);
        strcpy(store->profiles[0].password, password);
        store->count = 1U;
    }
    secure_zero(ssid, sizeof(ssid));
    secure_zero(password, sizeof(password));
    return err;
}

static esp_err_t convert_provisioned_profile(uint8_t *blob,
                                             wifi_profile_store_t *loaded)
{
    esp_err_t err = load_provisioned_profile(loaded);
    if (err != ESP_OK) {
        return err;
    }
    esp_fill_random(s_salt, sizeof(s_salt));
    err = derive_key(s_admin_password, s_salt, s_key);
    size_t blob_size = 0U;
    if (err == ESP_OK) {
        err = encrypt_store(loaded, s_salt, s_key, blob,
                            WIFI_PROFILE_BLOB_MAX_SIZE, &blob_size);
    }
    if (err == ESP_OK) {
        err = flash_user_nvs_set_blob(WIFI_PROFILE_STAGE_NAMESPACE,
                                      WIFI_PROFILE_STAGE_KEY, blob, blob_size);
    }
    if (err == ESP_OK) {
        err = erase_and_init_partition();
    }
    if (err == ESP_OK) {
        err = write_v2_blob(blob, blob_size);
    }
    if (err == ESP_OK) {
        esp_err_t erase_err = flash_user_nvs_erase_key(
            WIFI_PROFILE_STAGE_NAMESPACE, WIFI_PROFILE_STAGE_KEY);
        if ((erase_err != ESP_OK) && !not_found(erase_err)) {
            err = erase_err;
        }
    }
    return err;
}

esp_err_t wifi_profile_store_init(const char *admin_password)
{
    if ((admin_password == NULL) || (admin_password[0] == '\0')) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    psa_status_t crypto_status = psa_crypto_init();
    if (crypto_status != PSA_SUCCESS) {
        return ESP_FAIL;
    }
    esp_err_t err = flash_user_nvs_partition_init(WIFI_NVS_PARTITION, false);
    if (err != ESP_OK) {
        return err;
    }
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_admin_password = admin_password;
    initialize_empty_store(&s_store);
    wifi_profile_store_t *loaded = calloc(1U, sizeof(*loaded));
    uint8_t *blob = malloc(WIFI_PROFILE_BLOB_MAX_SIZE);
    if ((loaded == NULL) || (blob == NULL)) {
        err = ESP_ERR_NO_MEM;
        goto failed;
    }
    initialize_empty_store(loaded);
    bool recovered = false;
    err = recover_staged_store(blob, loaded, &recovered);
    if (err != ESP_OK) {
        goto finished;
    }
    if ((err == ESP_OK) && recovered) {
        if (!s_locked) {
            s_store = *loaded;
        }
        goto finished;
    }
    size_t blob_size = 0U;
    err = read_partition_blob(WIFI_PROFILE_V2_BLOB_KEY, blob, &blob_size);
    if (err == ESP_OK) {
        err = decrypt_store(blob, blob_size, admin_password, loaded, s_salt,
                            s_key);
        if ((err == ESP_ERR_INVALID_CRC) ||
            (err == ESP_ERR_INVALID_RESPONSE) ||
            (err == ESP_ERR_INVALID_SIZE)) {
            s_locked = true;
            err = ESP_OK;
        } else if (err == ESP_OK) {
            s_store = *loaded;
        }
        goto finished;
    }
    if ((err == ESP_ERR_INVALID_RESPONSE) || (err == ESP_ERR_INVALID_SIZE)) {
        s_locked = true;
        err = ESP_OK;
        goto finished;
    }
    if (!not_found(err)) {
        goto finished;
    }
    err = read_partition_blob(WIFI_PROFILE_V1_BLOB_KEY, blob, &blob_size);
    if (err == ESP_OK) {
        err = erase_and_init_partition();
        if (err == ESP_OK) {
            err = create_empty_encrypted_store();
        }
        goto finished;
    }
    if (!not_found(err)) {
        goto finished;
    }
    err = convert_provisioned_profile(blob, loaded);
    if (err == ESP_OK) {
        s_store = *loaded;
        goto finished;
    }
    if (!not_found(err)) {
        goto finished;
    }
    err = erase_and_init_partition();
    if (err == ESP_OK) {
        err = create_empty_encrypted_store();
    }

finished:
    if (err == ESP_OK) {
        s_initialized = true;
    }
failed:
    if (blob != NULL) {
        secure_zero(blob, WIFI_PROFILE_BLOB_MAX_SIZE);
        free(blob);
    }
    if (loaded != NULL) {
        secure_zero(loaded, sizeof(*loaded));
        free(loaded);
    }
    if (err != ESP_OK) {
        secure_zero(s_key, sizeof(s_key));
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        s_admin_password = NULL;
    }
    return err;
}

esp_err_t wifi_profile_store_get(wifi_profile_snapshot_t *snapshot)
{
    if ((snapshot == NULL) || !s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    copy_snapshot(&s_store, snapshot);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t wifi_profile_store_get_password(uint32_t id, char *password,
                                          size_t size)
{
    if (password != NULL) {
        secure_zero(password, size);
    }
    if ((id == 0U) || (password == NULL) ||
        (size < WIFI_PROFILE_PASSWORD_SIZE) || !s_initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    int index = find_profile(&s_store, id);
    if (s_locked) {
        err = ESP_ERR_INVALID_CRC;
    } else if (index < 0) {
        err = ESP_ERR_NOT_FOUND;
    } else {
        strcpy(password, s_store.profiles[index].password);
    }
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t wifi_profile_store_reset(wifi_profile_snapshot_t *snapshot)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = flash_user_nvs_erase_key(WIFI_PROFILE_STAGE_NAMESPACE,
                                             WIFI_PROFILE_STAGE_KEY);
    if (not_found(err)) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = erase_and_init_partition();
    }
    if (err == ESP_OK) {
        secure_zero(&s_store, sizeof(s_store));
        initialize_empty_store(&s_store);
        s_locked = false;
        err = create_empty_encrypted_store();
        if (err != ESP_OK) {
            s_locked = true;
        }
    }
    copy_snapshot(&s_store, snapshot);
    xSemaphoreGive(s_mutex);
    return err;
}

static esp_err_t prepare_mutation(uint32_t expected_revision,
                                  wifi_profile_store_t *candidate)
{
    if (!s_initialized || (candidate == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_locked) {
        return ESP_ERR_INVALID_CRC;
    }
    if (expected_revision != s_store.revision) {
        return ESP_ERR_INVALID_STATE;
    }
    *candidate = s_store;
    return ESP_OK;
}

esp_err_t wifi_profile_store_add(uint32_t expected_revision,
                                 const char *ssid, const char *password,
                                 wifi_profile_snapshot_t *snapshot)
{
    esp_err_t err = wifi_profile_validate(ssid, password);
    if (err != ESP_OK) {
        return err;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    wifi_profile_store_t candidate;
    err = prepare_mutation(expected_revision, &candidate);
    if ((err == ESP_OK) && (candidate.count >= WIFI_PROFILE_MAX_COUNT)) {
        err = ESP_ERR_NO_MEM;
    }
    if ((err == ESP_OK) && (find_ssid(&candidate, ssid) >= 0)) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK) {
        wifi_profile_t *profile = &candidate.profiles[candidate.count++];
        profile->id = allocate_id(&candidate);
        strcpy(profile->ssid, ssid);
        strcpy(profile->password, password);
        err = persist_store(&candidate);
    }
    copy_snapshot(&s_store, snapshot);
    secure_zero(&candidate, sizeof(candidate));
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t wifi_profile_store_update(uint32_t expected_revision, uint32_t id,
                                    const char *ssid, const char *password,
                                    wifi_profile_snapshot_t *snapshot)
{
    esp_err_t err =
        wifi_profile_validate(ssid, (password != NULL) ? password : "");
    if (err != ESP_OK) {
        return err;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    wifi_profile_store_t candidate;
    err = prepare_mutation(expected_revision, &candidate);
    int index = (err == ESP_OK) ? find_profile(&candidate, id) : -1;
    int duplicate = (err == ESP_OK) ? find_ssid(&candidate, ssid) : -1;
    if ((err == ESP_OK) && (index < 0)) {
        err = ESP_ERR_NOT_FOUND;
    } else if ((err == ESP_OK) && (duplicate >= 0) && (duplicate != index)) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK) {
        strcpy(candidate.profiles[index].ssid, ssid);
        if (password != NULL) {
            strcpy(candidate.profiles[index].password, password);
        }
        err = persist_store(&candidate);
    }
    copy_snapshot(&s_store, snapshot);
    secure_zero(&candidate, sizeof(candidate));
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t wifi_profile_store_remove(uint32_t expected_revision, uint32_t id,
                                    wifi_profile_snapshot_t *snapshot)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    wifi_profile_store_t candidate;
    esp_err_t err = prepare_mutation(expected_revision, &candidate);
    int index = (err == ESP_OK) ? find_profile(&candidate, id) : -1;
    if ((err == ESP_OK) && (index < 0)) {
        err = ESP_ERR_NOT_FOUND;
    }
    if (err == ESP_OK) {
        size_t trailing = candidate.count - (size_t)index - 1U;
        if (trailing > 0U) {
            memmove(&candidate.profiles[index], &candidate.profiles[index + 1],
                    trailing * sizeof(candidate.profiles[0]));
        }
        --candidate.count;
        memset(&candidate.profiles[candidate.count], 0,
               sizeof(candidate.profiles[0]));
        err = persist_store(&candidate);
    }
    copy_snapshot(&s_store, snapshot);
    secure_zero(&candidate, sizeof(candidate));
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t wifi_profile_store_reorder(uint32_t expected_revision,
                                     const uint32_t *ids, size_t count,
                                     wifi_profile_snapshot_t *snapshot)
{
    if ((ids == NULL) && (count != 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    wifi_profile_store_t candidate;
    esp_err_t err = prepare_mutation(expected_revision, &candidate);
    if ((err == ESP_OK) && (count != candidate.count)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    wifi_profile_t ordered[WIFI_PROFILE_MAX_COUNT] = {0};
    for (size_t index = 0U; (err == ESP_OK) && (index < count); ++index) {
        int source = find_profile(&candidate, ids[index]);
        if (source < 0) {
            err = ESP_ERR_NOT_FOUND;
            break;
        }
        for (size_t prior = 0U; prior < index; ++prior) {
            if (ids[prior] == ids[index]) {
                err = ESP_ERR_INVALID_ARG;
                break;
            }
        }
        if (err == ESP_OK) {
            ordered[index] = candidate.profiles[source];
        }
    }
    if (err == ESP_OK) {
        memcpy(candidate.profiles, ordered,
               count * sizeof(candidate.profiles[0]));
        err = persist_store(&candidate);
    }
    copy_snapshot(&s_store, snapshot);
    secure_zero(ordered, sizeof(ordered));
    secure_zero(&candidate, sizeof(candidate));
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t wifi_profile_store_upsert_and_promote(
    const char *ssid, const char *password, wifi_profile_snapshot_t *snapshot)
{
    esp_err_t err = wifi_profile_validate(ssid, password);
    if (err != ESP_OK) {
        return err;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_locked) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_CRC;
    }
    wifi_profile_store_t candidate = s_store;
    int index = find_ssid(&candidate, ssid);
    if ((index < 0) && (candidate.count >= WIFI_PROFILE_MAX_COUNT)) {
        err = ESP_ERR_NO_MEM;
    } else if (index < 0) {
        index = (int)candidate.count++;
        candidate.profiles[index].id = allocate_id(&candidate);
    }
    if (err == ESP_OK) {
        wifi_profile_t promoted = candidate.profiles[index];
        strcpy(promoted.ssid, ssid);
        strcpy(promoted.password, password);
        if (index > 0) {
            memmove(&candidate.profiles[1], &candidate.profiles[0],
                    (size_t)index * sizeof(candidate.profiles[0]));
        }
        candidate.profiles[0] = promoted;
        err = persist_store(&candidate);
        secure_zero(&promoted, sizeof(promoted));
    }
    copy_snapshot(&s_store, snapshot);
    secure_zero(&candidate, sizeof(candidate));
    xSemaphoreGive(s_mutex);
    return err;
}
