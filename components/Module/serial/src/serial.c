#include "serial.h"

#include <stddef.h>
#include <stdint.h>

#include "bsp_pin_config.h"
#include "bsp_uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network_tcp_server.h"

#define SERIAL_TCP_PORT 4443U
#define SERIAL_RX_CHUNK_SIZE 128U
#define SERIAL_RX_TIMEOUT_MS 20U

static const char *TAG = "SERIAL";
static network_tcp_server_t *s_server;
static TaskHandle_t s_task;
static volatile bool s_started;
static volatile bool s_network_enabled;
static serial_config_t s_config;

static bool valid_config(const serial_config_t *config)
{
    return (config != NULL) && (config->baudrate > 0U) &&
           (config->data_bits >= 5U) && (config->data_bits <= 8U) &&
           (config->parity <= SERIAL_PARITY_ODD) &&
           (config->stop_bits >= 1U) && (config->stop_bits <= 2U);
}

static bsp_uart_config_t bsp_config(const serial_config_t *config)
{
    return (bsp_uart_config_t){
        .baudrate = config->baudrate,
        .data_bits = config->data_bits,
        .parity = (uart_parity_t)config->parity,
        .stop_bits = config->stop_bits,
    };
}

static esp_err_t tcp_received(const uint8_t *data, size_t size, void *context)
{
    (void)context;
    return bsp_uart_write(data, size);
}

static void tcp_client_changed(bool connected, void *context)
{
    (void)context;
    ESP_LOGI(TAG, "Wireless UART client %s", connected ? "connected" : "ended");
}

static void serial_task(void *argument)
{
    (void)argument;
    uint8_t buffer[SERIAL_RX_CHUNK_SIZE];
    while (s_started) {
        int received = bsp_uart_read(buffer, sizeof(buffer),
                                     SERIAL_RX_TIMEOUT_MS);
        if (received <= 0) {
            continue;
        }
        if (!s_network_enabled || !network_tcp_server_has_client(s_server)) {
            continue;
        }
        esp_err_t err = network_tcp_server_send(s_server, buffer,
                                                (size_t)received);
        if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
            ESP_LOGW(TAG, "UART to TCP send failed: %s", esp_err_to_name(err));
            network_tcp_server_disconnect_client(s_server);
        }
    }
    vTaskDelete(NULL);
}

esp_err_t serial_start(const serial_config_t *config)
{
    if (!valid_config(config)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    bsp_uart_config_t uart = bsp_config(config);
    esp_err_t err = bsp_uart_init(&uart);
    if (err != ESP_OK) {
        return err;
    }
    const network_tcp_server_config_t server_config = {
        .port = SERIAL_TCP_PORT,
        .received = tcp_received,
        .client_changed = tcp_client_changed,
        .context = NULL,
    };
    err = network_tcp_server_start(&s_server, &server_config);
    if (err != ESP_OK) {
        (void)bsp_uart_deinit();
        return err;
    }
    s_config = *config;
    s_network_enabled = false;
    s_started = true;
    if (xTaskCreatePinnedToCore(serial_task, "Serial-Bridge", 3072, NULL,
                                configMAX_PRIORITIES - 3, &s_task, 0) != pdPASS) {
        s_started = false;
        (void)network_tcp_server_stop(s_server);
        s_server = NULL;
        (void)bsp_uart_deinit();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t serial_stop(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    s_started = false;
    s_network_enabled = false;
    network_tcp_server_set_enabled(s_server, false);
    if (s_task != NULL) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    esp_err_t server_err = network_tcp_server_stop(s_server);
    s_server = NULL;
    esp_err_t uart_err = bsp_uart_deinit();
    s_config = (serial_config_t){0};
    return (server_err != ESP_OK) ? server_err : uart_err;
}

void serial_set_network_enabled(bool enabled)
{
    if (!s_started) {
        return;
    }
    s_network_enabled = enabled && s_config.enabled;
    network_tcp_server_set_enabled(s_server, s_network_enabled);
}

esp_err_t serial_apply_config(const serial_config_t *config)
{
    if (!valid_config(config)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    serial_config_t previous = s_config;
    bool network_was_enabled = s_network_enabled;
    s_network_enabled = false;
    network_tcp_server_set_enabled(s_server, false);
    bsp_uart_config_t uart = bsp_config(config);
    esp_err_t err = bsp_uart_reconfigure(&uart);
    if (err != ESP_OK) {
        bsp_uart_config_t previous_uart = bsp_config(&previous);
        (void)bsp_uart_reconfigure(&previous_uart);
        s_network_enabled = network_was_enabled;
        network_tcp_server_set_enabled(s_server, network_was_enabled);
        return err;
    }
    (void)bsp_uart_flush();
    s_config = *config;
    s_network_enabled = network_was_enabled && config->enabled;
    network_tcp_server_set_enabled(s_server, s_network_enabled);
    return ESP_OK;
}

bool serial_has_client(void)
{
    return network_tcp_server_has_client(s_server);
}
