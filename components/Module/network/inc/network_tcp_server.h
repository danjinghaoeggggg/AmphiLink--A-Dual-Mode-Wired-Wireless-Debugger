#ifndef NETWORK_TCP_SERVER_H
#define NETWORK_TCP_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef esp_err_t (*network_tcp_server_rx_callback_t)(const uint8_t *data,
                                                       size_t size,
                                                       void *context);
typedef void (*network_tcp_server_client_callback_t)(bool connected,
                                                      void *context);

typedef struct {
    uint16_t port;
    network_tcp_server_rx_callback_t received;
    network_tcp_server_client_callback_t client_changed;
    void *context;
} network_tcp_server_config_t;

esp_err_t network_tcp_server_start(const network_tcp_server_config_t *config);
esp_err_t network_tcp_server_stop(void);
void network_tcp_server_set_enabled(bool enabled);
bool network_tcp_server_has_client(void);
esp_err_t network_tcp_server_send(const uint8_t *data, size_t size);
void network_tcp_server_disconnect_client(void);

#endif
