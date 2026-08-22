#ifndef BSP_RMT_TX_H
#define BSP_RMT_TX_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define BSP_RMT_TX_MAX_SYMBOLS 64U

typedef struct {
    uint16_t duration0;
    uint16_t duration1;
    uint8_t level0;
    uint8_t level1;
} bsp_rmt_tx_symbol_t;

esp_err_t bsp_rmt_tx_init(uint32_t pin, uint32_t resolution_hz);
esp_err_t bsp_rmt_tx_write(const bsp_rmt_tx_symbol_t *symbols,
                           size_t symbol_count);
esp_err_t bsp_rmt_tx_deinit(void);

#endif
