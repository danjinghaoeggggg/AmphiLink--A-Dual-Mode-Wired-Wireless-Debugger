#include "wifi_profile_store.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "flash_user.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define WIFI_NVS_PARTITION "wifi_nvs"
#define WIFI_NVS_NAMESPACE "wifi"
#define WIFI_NVS_PROVISIONED_SSID_KEY "ssid"
#define WIFI_NVS_PROVISIONED_PASSWORD_KEY "password"
#define WIFI_PROFILE_BLOB_KEY "profiles_v1"
#define WIFI_PROFILE_MAGIC 0x47464357U
#define WIFI_PROFILE_SCHEMA_VERSION 1U
#define WIFI_PROFILE_HEADER_SIZE 16U
#define WIFI_PROFILE_RECORD_SIZE 103U
#define WIFI_PROFILE_BLOB_MAX_SIZE                                          \
    (WIFI_PROFILE_HEADER_SIZE +                                             \
     (WIFI_PROFILE_MAX_COUNT * WIFI_PROFILE_RECORD_SIZE))

typedef struct {
    uint32_t revision;
    uint32_t next_id;
    size_t count;
    wifi_profile_t profiles[WIFI_PROFILE_MAX_COUNT];
} wifi_profile_store_t;

static SemaphoreHandle_t s_mutex;
static wifi_profile_store_t s_store;
static bool s_initialized;

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

static void copy_snapshot(const wifi_profile_store_t *store,
                          wifi_profile_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
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
    uint32_t id = store->next_id;
    if (id == 0U) {
        id = 1U;
    }
    while (find_profile(store, id) >= 0) {
        ++id;
        if (id == 0U) {
            id = 1U;
        }
    }
    store->next_id = id + 1U;
    if (store->next_id == 0U) {
        store->next_id = 1U;
    }
    return id;
}

static esp_err_t serialize_store(const wifi_profile_store_t *store,
                                 uint8_t *blob, size_t blob_size,
                                 size_t *bytes_written)
{
    size_t required = WIFI_PROFILE_HEADER_SIZE +
                      (store->count * WIFI_PROFILE_RECORD_SIZE);
    if ((blob == NULL) || (bytes_written == NULL) ||
        (blob_size < required) || (store->count > WIFI_PROFILE_MAX_COUNT)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(blob, 0, required);
    write_u32(blob, WIFI_PROFILE_MAGIC);
    write_u16(blob + 4U, WIFI_PROFILE_SCHEMA_VERSION);
    write_u16(blob + 6U, (uint16_t)store->count);
    write_u32(blob + 8U, store->revision);
    write_u32(blob + 12U, store->next_id);

    size_t offset = WIFI_PROFILE_HEADER_SIZE;
    for (size_t index = 0U; index < store->count; ++index) {
        const wifi_profile_t *profile = &store->profiles[index];
        size_t ssid_length = strlen(profile->ssid);
        size_t password_length = strlen(profile->password);
        write_u32(blob + offset, profile->id);
        blob[offset + 4U] = (uint8_t)ssid_length;
        blob[offset + 5U] = (uint8_t)password_length;
        memcpy(blob + offset + 8U, profile->ssid, ssid_length);
        memcpy(blob + offset + 40U, profile->password, password_length);
        offset += WIFI_PROFILE_RECORD_SIZE;
    }
    *bytes_written = required;
    return ESP_OK;
}

static esp_err_t deserialize_store(const uint8_t *blob, size_t blob_size,
                                   wifi_profile_store_t *store)
{
    if ((blob == NULL) || (store == NULL) ||
        (blob_size < WIFI_PROFILE_HEADER_SIZE) ||
        (read_u32(blob) != WIFI_PROFILE_MAGIC) ||
        (read_u16(blob + 4U) != WIFI_PROFILE_SCHEMA_VERSION)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t count = read_u16(blob + 6U);
    if ((count > WIFI_PROFILE_MAX_COUNT) ||
        (blob_size != WIFI_PROFILE_HEADER_SIZE +
                          (count * WIFI_PROFILE_RECORD_SIZE))) {
        return ESP_ERR_INVALID_SIZE;
    }

    wifi_profile_store_t decoded = {
        .revision = read_u32(blob + 8U),
        .next_id = read_u32(blob + 12U),
        .count = count,
    };
    size_t offset = WIFI_PROFILE_HEADER_SIZE;
    for (size_t index = 0U; index < count; ++index) {
        size_t ssid_length = blob[offset + 4U];
        size_t password_length = blob[offset + 5U];
        if ((ssid_length == 0U) || (ssid_length > 32U) ||
            (password_length > 63U)) {
            return ESP_ERR_INVALID_SIZE;
        }
        wifi_profile_t *profile = &decoded.profiles[index];
        profile->id = read_u32(blob + offset);
        memcpy(profile->ssid, blob + offset + 8U, ssid_length);
        memcpy(profile->password, blob + offset + 40U, password_length);
        if ((profile->id == 0U) ||
            (wifi_profile_validate(profile->ssid, profile->password) !=
             ESP_OK) ||
            (find_profile(&decoded, profile->id) != (int)index) ||
            (find_ssid(&decoded, profile->ssid) != (int)index)) {
            return ESP_ERR_INVALID_RESPONSE;
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
    return ESP_OK;
}

static esp_err_t persist_store(wifi_profile_store_t *candidate)
{
    uint8_t *blob = malloc(WIFI_PROFILE_BLOB_MAX_SIZE);
    if (blob == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t blob_size = 0U;
    ++candidate->revision;
    if (candidate->revision == 0U) {
        candidate->revision = 1U;
    }
    esp_err_t err = serialize_store(candidate, blob,
                                    WIFI_PROFILE_BLOB_MAX_SIZE, &blob_size);
    if (err == ESP_OK) {
        err = flash_user_nvs_partition_set_blob(
            WIFI_NVS_PARTITION, WIFI_NVS_NAMESPACE, WIFI_PROFILE_BLOB_KEY,
            blob, blob_size);
    }
    if (err == ESP_OK) {
        s_store = *candidate;
    }
    free(blob);
    return err;
}

static esp_err_t load_provisioned_profile(wifi_profile_store_t *store)
{
    char ssid[WIFI_PROFILE_SSID_SIZE];
    char password[WIFI_PROFILE_PASSWORD_SIZE];
    esp_err_t err = flash_user_nvs_partition_get_string(
        WIFI_NVS_PARTITION, WIFI_NVS_NAMESPACE,
        WIFI_NVS_PROVISIONED_SSID_KEY, ssid, sizeof(ssid), NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = flash_user_nvs_partition_get_string(
        WIFI_NVS_PARTITION, WIFI_NVS_NAMESPACE,
        WIFI_NVS_PROVISIONED_PASSWORD_KEY, password, sizeof(password), NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = wifi_profile_validate(ssid, password);
    if (err != ESP_OK) {
        return err;
    }
    store->profiles[0].id = allocate_id(store);
    strcpy(store->profiles[0].ssid, ssid);
    strcpy(store->profiles[0].password, password);
    store->count = 1U;
    return ESP_OK;
}

static void initialize_empty_store(wifi_profile_store_t *store)
{
    memset(store, 0, sizeof(*store));
    store->revision = 1U;
    store->next_id = 1U;
}

static esp_err_t load_store(wifi_profile_store_t *store, uint8_t *blob,
                            bool *needs_persist)
{
    size_t blob_size = 0U;
    esp_err_t err = flash_user_nvs_partition_get_blob(
        WIFI_NVS_PARTITION, WIFI_NVS_NAMESPACE, WIFI_PROFILE_BLOB_KEY, blob,
        WIFI_PROFILE_BLOB_MAX_SIZE, &blob_size);
    if (err == ESP_OK) {
        return deserialize_store(blob, blob_size, store);
    }
    if (err != ESP_ERR_NOT_FOUND) {
        return err;
    }
    err = load_provisioned_profile(store);
    if (err == ESP_OK) {
        *needs_persist = true;
    }
    return err;
}

esp_err_t wifi_profile_store_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = flash_user_nvs_partition_init(WIFI_NVS_PARTITION, true);
    if (err != ESP_OK) {
        return err;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    wifi_profile_store_t *loaded = calloc(1U, sizeof(*loaded));
    uint8_t *blob = malloc(WIFI_PROFILE_BLOB_MAX_SIZE);
    if ((loaded == NULL) || (blob == NULL)) {
        free(blob);
        free(loaded);
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    initialize_empty_store(loaded);
    bool needs_persist = false;
    err = load_store(loaded, blob, &needs_persist);
    if (err == ESP_ERR_NOT_FOUND) {
        err = ESP_OK;
    }
    if ((err == ESP_OK) && needs_persist) {
        err = persist_store(loaded);
    }
    if (err != ESP_OK) {
        free(blob);
        free(loaded);
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return err;
    }
    s_store = *loaded;
    free(blob);
    free(loaded);
    s_initialized = true;
    return ESP_OK;
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

static esp_err_t prepare_mutation(uint32_t expected_revision,
                                  wifi_profile_store_t *candidate)
{
    if (!s_initialized || (candidate == NULL)) {
        return ESP_ERR_INVALID_STATE;
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
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t wifi_profile_store_update(uint32_t expected_revision, uint32_t id,
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
    int index = (err == ESP_OK) ? find_profile(&candidate, id) : -1;
    int duplicate = (err == ESP_OK) ? find_ssid(&candidate, ssid) : -1;
    if ((err == ESP_OK) && (index < 0)) {
        err = ESP_ERR_NOT_FOUND;
    } else if ((err == ESP_OK) && (duplicate >= 0) && (duplicate != index)) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK) {
        strcpy(candidate.profiles[index].ssid, ssid);
        strcpy(candidate.profiles[index].password, password);
        err = persist_store(&candidate);
    }
    copy_snapshot(&s_store, snapshot);
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
    }
    copy_snapshot(&s_store, snapshot);
    xSemaphoreGive(s_mutex);
    return err;
}
