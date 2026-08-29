#include "serial_config_store.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "wireless_config.h"

static SemaphoreHandle_t s_mutex;
static serial_config_t s_config;
static uint32_t s_revision;
static bool s_initialized;

static const uint32_t s_baudrates[] = {
    9600U, 19200U, 38400U, 57600U,
    115200U, 230400U, 460800U, 921600U,
};

bool serial_config_store_baudrate_allowed(uint32_t baudrate)
{
    for (size_t index = 0U; index < sizeof(s_baudrates) / sizeof(s_baudrates[0]);
         ++index) {
        if (s_baudrates[index] == baudrate) {
            return true;
        }
    }
    return false;
}

static bool valid_config(const serial_config_t *config)
{
    return (config != NULL) &&
           serial_config_store_baudrate_allowed(config->baudrate) &&
           (config->data_bits >= 5U) && (config->data_bits <= 8U) &&
           (config->parity <= SERIAL_PARITY_ODD) &&
           (config->stop_bits >= 1U) && (config->stop_bits <= 2U);
}

static void defaults(serial_config_t *config)
{
    *config = (serial_config_t){
        .enabled = SERIAL_DEFAULT_ENABLED,
        .baudrate = SERIAL_DEFAULT_BAUDRATE,
        .data_bits = SERIAL_DEFAULT_DATA_BITS,
        .parity = SERIAL_PARITY_NONE,
        .stop_bits = SERIAL_DEFAULT_STOP_BITS,
    };
}

esp_err_t serial_config_store_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    defaults(&s_config);
    s_revision = 1U;
    s_initialized = true;
    return ESP_OK;
}

esp_err_t serial_config_store_get(serial_config_t *config, uint32_t *revision)
{
    if ((config == NULL) || (revision == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *config = s_config;
    *revision = s_revision;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t serial_config_store_set(const serial_config_t *config,
                                  uint32_t expected_revision,
                                  uint32_t *revision)
{
    if (!valid_config(config) || (revision == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (expected_revision != s_revision) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t next_revision = (s_revision == UINT32_MAX) ? 1U : s_revision + 1U;
    s_config = *config;
    s_revision = next_revision;
    *revision = next_revision;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}
