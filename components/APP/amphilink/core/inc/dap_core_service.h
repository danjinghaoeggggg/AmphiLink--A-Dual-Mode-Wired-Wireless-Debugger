#ifndef DAP_CORE_SERVICE_H
#define DAP_CORE_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#define DEBUG_PACKET_SIZE 64U

typedef struct {
    size_t size;
    uint8_t data[DEBUG_PACKET_SIZE];
} debug_packet_t;

esp_err_t dap_core_service_init(void);
esp_err_t dap_core_service_reset(void);

/* Called by the active transport receive path. This function never blocks. */
esp_err_t dap_core_service_submit(const uint8_t *data, size_t size);

/* Called only by the active transport task. */
esp_err_t dap_core_service_receive(debug_packet_t *packet, TickType_t wait_ticks);

#endif
