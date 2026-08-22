#include "dap_tcp_protocol.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define DAP_TCP_SIGNATURE 0x00504144UL
#define DAP_TCP_PACKET_TYPE_REQUEST 0x01U
#define DAP_TCP_PACKET_TYPE_RESPONSE 0x02U

#define SIGNATURE_OFFSET 0U
#define LENGTH_OFFSET 4U
#define PACKET_TYPE_OFFSET 6U
#define RESERVED_OFFSET 7U

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8U);
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) |
           ((uint32_t)data[3] << 24U);
}

static void write_le16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8U);
}

static void write_le32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8U);
    data[2] = (uint8_t)(value >> 16U);
    data[3] = (uint8_t)(value >> 24U);
}

static esp_err_t validate_header(const uint8_t *header, size_t *payload_size)
{
    if ((read_le32(header + SIGNATURE_OFFSET) != DAP_TCP_SIGNATURE) ||
        (header[PACKET_TYPE_OFFSET] != DAP_TCP_PACKET_TYPE_REQUEST) ||
        (header[RESERVED_OFFSET] != 0U)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint16_t length = read_le16(header + LENGTH_OFFSET);
    if ((length == 0U) || (length > DAP_TCP_MAX_PAYLOAD_SIZE)) {
        return ESP_ERR_INVALID_SIZE;
    }
    *payload_size = length;
    return ESP_OK;
}

void dap_tcp_parser_reset(dap_tcp_parser_t *parser)
{
    if (parser != NULL) {
        parser->buffered = 0U;
    }
}

esp_err_t dap_tcp_parser_feed(dap_tcp_parser_t *parser,
                              const uint8_t *data,
                              size_t size,
                              dap_tcp_request_callback_t request_callback,
                              void *context)
{
    if ((parser == NULL) || (data == NULL) || (size == 0U) ||
        (request_callback == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    while (size > 0U) {
        size_t required = DAP_TCP_HEADER_SIZE;
        size_t payload_size = 0U;
        if (parser->buffered >= DAP_TCP_HEADER_SIZE) {
            esp_err_t err = validate_header(parser->frame, &payload_size);
            if (err != ESP_OK) {
                dap_tcp_parser_reset(parser);
                return err;
            }
            required += payload_size;
        }

        size_t copy_size = required - parser->buffered;
        if (copy_size > size) {
            copy_size = size;
        }
        memcpy(parser->frame + parser->buffered, data, copy_size);
        parser->buffered += copy_size;
        data += copy_size;
        size -= copy_size;

        if (parser->buffered < DAP_TCP_HEADER_SIZE) {
            continue;
        }
        if (payload_size == 0U) {
            esp_err_t err = validate_header(parser->frame, &payload_size);
            if (err != ESP_OK) {
                dap_tcp_parser_reset(parser);
                return err;
            }
            required = DAP_TCP_HEADER_SIZE + payload_size;
        }
        if (parser->buffered < required) {
            continue;
        }

        esp_err_t err = request_callback(parser->frame + DAP_TCP_HEADER_SIZE,
                                         payload_size, context);
        dap_tcp_parser_reset(parser);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

esp_err_t dap_tcp_encode_response(const uint8_t *payload,
                                  size_t payload_size,
                                  uint8_t *frame,
                                  size_t frame_capacity,
                                  size_t *frame_size)
{
    if ((payload == NULL) || (frame == NULL) || (frame_size == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((payload_size == 0U) || (payload_size > DAP_TCP_MAX_PAYLOAD_SIZE) ||
        (frame_capacity < (DAP_TCP_HEADER_SIZE + payload_size))) {
        return ESP_ERR_INVALID_SIZE;
    }

    write_le32(frame + SIGNATURE_OFFSET, DAP_TCP_SIGNATURE);
    write_le16(frame + LENGTH_OFFSET, (uint16_t)payload_size);
    frame[PACKET_TYPE_OFFSET] = DAP_TCP_PACKET_TYPE_RESPONSE;
    frame[RESERVED_OFFSET] = 0U;
    memcpy(frame + DAP_TCP_HEADER_SIZE, payload, payload_size);
    *frame_size = DAP_TCP_HEADER_SIZE + payload_size;
    return ESP_OK;
}
