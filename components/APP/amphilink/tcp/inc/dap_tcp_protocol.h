#ifndef DAP_TCP_PROTOCOL_H
#define DAP_TCP_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define DAP_TCP_HEADER_SIZE 8U
#define DAP_TCP_MAX_PAYLOAD_SIZE 64U
#define DAP_TCP_MAX_FRAME_SIZE (DAP_TCP_HEADER_SIZE + DAP_TCP_MAX_PAYLOAD_SIZE)

typedef esp_err_t (*dap_tcp_request_callback_t)(const uint8_t *payload,
                                                size_t size,
                                                void *context);

typedef struct {
    size_t buffered;
    uint8_t frame[DAP_TCP_MAX_FRAME_SIZE];
} dap_tcp_parser_t;

void dap_tcp_parser_reset(dap_tcp_parser_t *parser);

esp_err_t dap_tcp_parser_feed(dap_tcp_parser_t *parser,
                              const uint8_t *data,
                              size_t size,
                              dap_tcp_request_callback_t request_callback,
                              void *context);

esp_err_t dap_tcp_encode_response(const uint8_t *payload,
                                  size_t payload_size,
                                  uint8_t *frame,
                                  size_t frame_capacity,
                                  size_t *frame_size);

#endif
