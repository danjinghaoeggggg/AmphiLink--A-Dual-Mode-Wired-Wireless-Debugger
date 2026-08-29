#ifndef BSP_UART_H
#define BSP_UART_H

#include <stddef.h>
#include <stdint.h>

#include "driver/uart.h"
#include "esp_err.h"

typedef struct {
    uint32_t baudrate;
    uint8_t data_bits;
    uart_parity_t parity;
    uint8_t stop_bits;
} bsp_uart_config_t;

esp_err_t bsp_uart_init(const bsp_uart_config_t *config);
esp_err_t bsp_uart_reconfigure(const bsp_uart_config_t *config);
esp_err_t bsp_uart_deinit(void);
int bsp_uart_read(uint8_t *data, size_t size, uint32_t timeout_ms);
esp_err_t bsp_uart_write(const uint8_t *data, size_t size);
esp_err_t bsp_uart_flush(void);

#endif
