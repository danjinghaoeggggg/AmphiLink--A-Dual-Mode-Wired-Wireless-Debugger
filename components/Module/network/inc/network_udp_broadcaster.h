#ifndef NETWORK_UDP_BROADCASTER_H
#define NETWORK_UDP_BROADCASTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define NETWORK_UDP_BROADCASTER_MAX_PAYLOAD_SIZE 256U

typedef struct {
    uint16_t port;
    uint32_t interval_ms;
    const void *payload;
    size_t payload_size;
} network_udp_broadcaster_config_t;

esp_err_t
network_udp_broadcaster_start(const network_udp_broadcaster_config_t *config);
esp_err_t network_udp_broadcaster_update_payload(const void *payload,
                                                 size_t payload_size);
esp_err_t network_udp_broadcaster_set_enabled(bool enabled);
esp_err_t network_udp_broadcaster_stop(void);

#endif
