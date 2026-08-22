#ifndef __USB_H__
#define __USB_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define USB_VENDOR_PACKET_SIZE 64U

typedef void (*usb_vendor_rx_callback_t)(const uint8_t *data, size_t size, void *context);
typedef void (*usb_state_callback_t)(bool mounted, void *context);

typedef struct {
    usb_vendor_rx_callback_t vendor_rx;
    usb_state_callback_t state_changed;
    void *context;
} usb_config_t;

/* Callbacks run in the TinyUSB task and must not block. */
esp_err_t usb_init(const usb_config_t *config);

/* Sends one vendor bulk packet, waiting until the IN endpoint is available. */
esp_err_t usb_vendor_send_packet(const uint8_t *data, size_t size);

#endif
