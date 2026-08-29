#ifndef SERIAL_H
#define SERIAL_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    SERIAL_PARITY_NONE = 0,
    SERIAL_PARITY_EVEN,
    SERIAL_PARITY_ODD,
} serial_parity_t;

typedef struct {
    bool enabled;
    uint32_t baudrate;
    uint8_t data_bits;
    serial_parity_t parity;
    uint8_t stop_bits;
} serial_config_t;

esp_err_t serial_start(const serial_config_t *config);
esp_err_t serial_stop(void);
void serial_set_network_enabled(bool enabled);
esp_err_t serial_apply_config(const serial_config_t *config);
bool serial_has_client(void);

#endif
