#ifndef SERIAL_CONFIG_STORE_H
#define SERIAL_CONFIG_STORE_H

#include "esp_err.h"
#include "serial.h"

esp_err_t serial_config_store_init(void);
esp_err_t serial_config_store_get(serial_config_t *config, uint32_t *revision);
esp_err_t serial_config_store_set(const serial_config_t *config,
                                  uint32_t expected_revision,
                                  uint32_t *revision);
bool serial_config_store_baudrate_allowed(uint32_t baudrate);

#endif
