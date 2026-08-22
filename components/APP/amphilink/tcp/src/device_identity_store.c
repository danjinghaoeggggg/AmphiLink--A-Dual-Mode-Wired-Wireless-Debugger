#include "device_identity_store.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "flash_user.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define IDENTITY_NVS_PARTITION "identity_nvs"
#define IDENTITY_NVS_NAMESPACE "identity"
#define IDENTITY_NVS_BLOB_KEY "identity_v2"
#define IDENTITY_NVS_NAME_KEY "name"
#define IDENTITY_NVS_ID_KEY "id"
#define DEVICE_IDENTITY_MAGIC 0x31444944U
#define DEVICE_IDENTITY_VERSION 2U
#define DEVICE_IDENTITY_BLOB_SIZE 40U

_Static_assert(DEVICE_IDENTITY_BLOB_SIZE >=
                   (8U + DEVICE_IDENTITY_NAME_SIZE - 1U),
               "Identity blob is too small for the maximum device name");

static SemaphoreHandle_t s_mutex;
static device_identity_t s_identity;
static bool s_initialized;

esp_err_t device_identity_validate(const char *name, uint32_t id)
{
    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t length = strnlen(name, DEVICE_IDENTITY_NAME_SIZE);
    if ((length == 0U) || (length >= DEVICE_IDENTITY_NAME_SIZE) ||
        (id < DEVICE_IDENTITY_ID_MIN) || (id > DEVICE_IDENTITY_ID_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t index = 0U; index < length; ++index) {
        uint8_t byte = (uint8_t)name[index];
        if ((byte < 0x20U) || (byte > 0x7EU)) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

static uint32_t read_u32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) | ((uint32_t)data[3] << 24U);
}

static void write_u32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8U);
    data[2] = (uint8_t)(value >> 16U);
    data[3] = (uint8_t)(value >> 24U);
}

static void set_default(device_identity_t *identity)
{
    memset(identity, 0, sizeof(*identity));
    strcpy(identity->name, DEVICE_IDENTITY_DEFAULT_NAME);
    identity->id = DEVICE_IDENTITY_DEFAULT_ID;
}

static void serialize(const device_identity_t *identity,
                      uint8_t blob[DEVICE_IDENTITY_BLOB_SIZE])
{
    size_t name_length = strlen(identity->name);
    memset(blob, 0, DEVICE_IDENTITY_BLOB_SIZE);
    write_u32(blob, DEVICE_IDENTITY_MAGIC);
    blob[4] = DEVICE_IDENTITY_VERSION;
    blob[5] = identity->id;
    blob[6] = (uint8_t)name_length;
    memcpy(blob + 8U, identity->name, name_length);
}

static esp_err_t deserialize(const uint8_t *blob, size_t blob_size,
                             device_identity_t *identity)
{
    if ((blob_size != DEVICE_IDENTITY_BLOB_SIZE) ||
        (read_u32(blob) != DEVICE_IDENTITY_MAGIC) ||
        (blob[4] != DEVICE_IDENTITY_VERSION) || (blob[6] == 0U) ||
        (blob[6] >= DEVICE_IDENTITY_NAME_SIZE)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    device_identity_t decoded = {.id = blob[5]};
    memcpy(decoded.name, blob + 8U, blob[6]);
    if (device_identity_validate(decoded.name, decoded.id) != ESP_OK) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *identity = decoded;
    return ESP_OK;
}

static esp_err_t persist(const device_identity_t *identity)
{
    uint8_t blob[DEVICE_IDENTITY_BLOB_SIZE];
    serialize(identity, blob);
    return flash_user_nvs_partition_set_blob(
        IDENTITY_NVS_PARTITION, IDENTITY_NVS_NAMESPACE,
        IDENTITY_NVS_BLOB_KEY, blob, sizeof(blob));
}

static esp_err_t load_csv_identity(device_identity_t *identity)
{
    device_identity_t loaded = {0};
    size_t name_size = 0U;
    esp_err_t err = flash_user_nvs_partition_get_string(
        IDENTITY_NVS_PARTITION, IDENTITY_NVS_NAMESPACE, IDENTITY_NVS_NAME_KEY,
        loaded.name, sizeof(loaded.name), &name_size);
    if (err != ESP_OK) {
        return err;
    }
    err = flash_user_nvs_partition_get_u8(
        IDENTITY_NVS_PARTITION, IDENTITY_NVS_NAMESPACE, IDENTITY_NVS_ID_KEY,
        &loaded.id);
    if (err != ESP_OK) {
        return err;
    }
    if ((name_size == 0U) ||
        (device_identity_validate(loaded.name, loaded.id) != ESP_OK)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *identity = loaded;
    return ESP_OK;
}

esp_err_t device_identity_store_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err =
        flash_user_nvs_partition_init(IDENTITY_NVS_PARTITION, true);
    if (err != ESP_OK) {
        return err;
    }
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t blob[DEVICE_IDENTITY_BLOB_SIZE];
    size_t blob_size = 0U;
    device_identity_t loaded;
    bool needs_persist = false;
    err = flash_user_nvs_partition_get_blob(
        IDENTITY_NVS_PARTITION, IDENTITY_NVS_NAMESPACE,
        IDENTITY_NVS_BLOB_KEY, blob, sizeof(blob), &blob_size);
    if (err == ESP_OK) {
        err = deserialize(blob, blob_size, &loaded);
    }
    if ((err == ESP_ERR_NOT_FOUND) || (err == ESP_ERR_INVALID_RESPONSE) ||
        (err == ESP_ERR_INVALID_SIZE)) {
        err = load_csv_identity(&loaded);
        needs_persist = (err == ESP_OK);
    }
    if ((err == ESP_ERR_NOT_FOUND) || (err == ESP_ERR_INVALID_RESPONSE) ||
        (err == ESP_ERR_INVALID_SIZE)) {
        set_default(&loaded);
        needs_persist = true;
        err = ESP_OK;
    }
    if ((err == ESP_OK) && needs_persist) {
        err = persist(&loaded);
    }
    if (err != ESP_OK) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return err;
    }

    s_identity = loaded;
    s_initialized = true;
    return ESP_OK;
}

esp_err_t device_identity_store_get(device_identity_t *identity)
{
    if (identity == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *identity = s_identity;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t device_identity_store_set(const char *name, uint32_t id,
                                    device_identity_t *identity)
{
    esp_err_t err = device_identity_validate(name, id);
    if (err != ESP_OK) {
        return err;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    device_identity_t candidate = {.id = (uint8_t)id};
    strcpy(candidate.name, name);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    err = persist(&candidate);
    if (err == ESP_OK) {
        s_identity = candidate;
        if (identity != NULL) {
            *identity = candidate;
        }
    }
    xSemaphoreGive(s_mutex);
    return err;
}
