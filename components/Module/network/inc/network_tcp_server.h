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

typedef struct network_tcp_server network_tcp_server_t;

esp_err_t network_tcp_server_start(network_tcp_server_t **server,
                                   const network_tcp_server_config_t *config);
esp_err_t network_tcp_server_stop(network_tcp_server_t *server);
void network_tcp_server_set_enabled(network_tcp_server_t *server, bool enabled);
bool network_tcp_server_has_client(network_tcp_server_t *server);
esp_err_t network_tcp_server_send(network_tcp_server_t *server,
                                  const uint8_t *data, size_t size);
void network_tcp_server_disconnect_client(network_tcp_server_t *server);

#endif
