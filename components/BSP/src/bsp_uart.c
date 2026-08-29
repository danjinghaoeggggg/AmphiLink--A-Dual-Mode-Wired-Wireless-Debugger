#include "bsp_uart.h"

#include <stdbool.h>

#include "bsp_pin_config.h"
#include "freertos/FreeRTOS.h"

#define BSP_UART_PORT UART_NUM_1
#define BSP_UART_RX_BUFFER_SIZE 4096U
#define BSP_UART_TX_BUFFER_SIZE 4096U

static bool s_initialized;

static bool valid_config(const bsp_uart_config_t *config)
{
    return (config != NULL) && (config->baudrate > 0U) &&
           (config->data_bits >= 5U) && (config->data_bits <= 8U) &&
           (config->parity <= UART_PARITY_ODD) &&
           (config->stop_bits >= 1U) && (config->stop_bits <= 2U);
}

static uart_word_length_t word_length(uint8_t data_bits)
{
    switch (data_bits) {
    case 5U:
        return UART_DATA_5_BITS;
    case 6U:
        return UART_DATA_6_BITS;
    case 7U:
        return UART_DATA_7_BITS;
    case 8U:
    default:
        return UART_DATA_8_BITS;
    }
}

static uart_stop_bits_t stop_bits(uint8_t bits)
{
    return (bits == 2U) ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
}

static esp_err_t apply_config(const bsp_uart_config_t *config)
{
    const uart_config_t uart_config = {
        .baud_rate = (int)config->baudrate,
        .data_bits = word_length(config->data_bits),
        .parity = config->parity,
        .stop_bits = stop_bits(config->stop_bits),
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config(BSP_UART_PORT, &uart_config);
    if (err != ESP_OK) {
        return err;
    }
    return uart_set_pin(BSP_UART_PORT, BSP_PIN_UART_TX, BSP_PIN_UART_RX,
                        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

esp_err_t bsp_uart_init(const bsp_uart_config_t *config)
{
    if (!valid_config(config)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = uart_driver_install(BSP_UART_PORT, BSP_UART_RX_BUFFER_SIZE,
                                        BSP_UART_TX_BUFFER_SIZE, 0, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    err = apply_config(config);
    if (err != ESP_OK) {
        (void)uart_driver_delete(BSP_UART_PORT);
        return err;
    }
    s_initialized = true;
    return ESP_OK;
}

esp_err_t bsp_uart_reconfigure(const bsp_uart_config_t *config)
{
    if (!valid_config(config)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return apply_config(config);
}

esp_err_t bsp_uart_deinit(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = uart_driver_delete(BSP_UART_PORT);
    if (err == ESP_OK) {
        s_initialized = false;
    }
    return err;
}

int bsp_uart_read(uint8_t *data, size_t size, uint32_t timeout_ms)
{
    if ((data == NULL) || (size == 0U)) {
        return -1;
    }
    if (!s_initialized) {
        return -1;
    }
    return uart_read_bytes(BSP_UART_PORT, data, size,
                           pdMS_TO_TICKS(timeout_ms));
}

esp_err_t bsp_uart_write(const uint8_t *data, size_t size)
{
    if ((data == NULL) || (size == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    int written = uart_write_bytes(BSP_UART_PORT, data, size);
    return (written == (int)size) ? ESP_OK : ESP_FAIL;
}

esp_err_t bsp_uart_flush(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return uart_flush(BSP_UART_PORT);
}
