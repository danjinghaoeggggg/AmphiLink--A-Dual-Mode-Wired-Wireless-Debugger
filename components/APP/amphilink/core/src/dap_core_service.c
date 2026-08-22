/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The CMSIS-DAP v2 command queue behavior implemented here follows the ARM
 * CMSIS-DAP firmware contract and is adapted to ESP-IDF/FreeRTOS.
 * Upstream: https://github.com/ARM-software/CMSIS-DAP
 * Local license copy: referance_project_licenses/CMSIS-DAP/LICENSE
 */

#include "dap_core_service.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "DAP.h"
#include "DAP_config.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

_Static_assert(DAP_PACKET_COUNT > 0U, "Request and response queues must not be empty");
_Static_assert(DAP_PACKET_SIZE == DEBUG_PACKET_SIZE,
               "Core and transport packet sizes must match");

typedef enum {
    CORE_EVENT_REQUEST,
    CORE_EVENT_RESET,
} core_event_type_t;

typedef struct {
    core_event_type_t type;
    debug_packet_t packet;
} core_event_t;

static const char *TAG = "DAP_CORE";
static QueueHandle_t s_request_queue;
static QueueHandle_t s_response_queue;
static SemaphoreHandle_t s_reset_mutex;
static SemaphoreHandle_t s_reset_complete;
static TaskHandle_t s_core_task;

#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
#define DAP_CORE_TASK_CORE 1
#else
#define DAP_CORE_TASK_CORE 0
#endif

static void dap_core_execute_request(debug_packet_t *request)
{
    debug_packet_t response = {
        .size = 0U,
        .data = {0},
    };
    uint32_t result = DAP_ExecuteCommand(request->data, response.data);
    response.size = (uint16_t)result;

    if ((response.size == 0U) || (response.size > sizeof(response.data))) {
        ESP_LOGE(TAG, "Invalid response size: %u", (unsigned int)response.size);
        return;
    }

    if (xQueueSend(s_response_queue, &response, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to queue response");
    }
}

static void dap_core_apply_reset(void)
{
    DAP_Setup();
    DAP_TransferAbort = 0U;
    xSemaphoreGive(s_reset_complete);
}

static void dap_core_task(void *argument)
{
    (void)argument;
    core_event_t batch[DAP_PACKET_COUNT];

    while (true) {
        core_event_t event;
        if (xQueueReceive(s_request_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (event.type == CORE_EVENT_RESET) {
            dap_core_apply_reset();
            continue;
        }

        if (event.packet.data[0] != ID_DAP_QueueCommands) {
            dap_core_execute_request(&event.packet);
            continue;
        }

        size_t batch_count = 0U;
        bool reset_requested = false;
        do {
            batch[batch_count++] = event;
            if (event.packet.data[0] != ID_DAP_QueueCommands) {
                break;
            }
            if (batch_count == DAP_PACKET_COUNT) {
                ESP_LOGE(TAG, "Queued command sequence has no execute packet");
                break;
            }
            if (xQueueReceive(s_request_queue, &event, portMAX_DELAY) != pdTRUE) {
                continue;
            }
            if (event.type == CORE_EVENT_RESET) {
                reset_requested = true;
                break;
            }
        } while (true);

        if (reset_requested) {
            dap_core_apply_reset();
            continue;
        }

        for (size_t index = 0U; index < batch_count; ++index) {
            if (batch[index].packet.data[0] == ID_DAP_QueueCommands) {
                batch[index].packet.data[0] = ID_DAP_ExecuteCommands;
            }
            dap_core_execute_request(&batch[index].packet);
        }
    }
}

static void dap_core_service_cleanup(void)
{
    if (s_reset_complete != NULL) {
        vSemaphoreDelete(s_reset_complete);
        s_reset_complete = NULL;
    }
    if (s_reset_mutex != NULL) {
        vSemaphoreDelete(s_reset_mutex);
        s_reset_mutex = NULL;
    }
    if (s_response_queue != NULL) {
        vQueueDelete(s_response_queue);
        s_response_queue = NULL;
    }
    if (s_request_queue != NULL) {
        vQueueDelete(s_request_queue);
        s_request_queue = NULL;
    }
}

esp_err_t dap_core_service_init(void)
{
    if (s_request_queue != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_request_queue = xQueueCreate(DAP_PACKET_COUNT, sizeof(core_event_t));
    s_response_queue = xQueueCreate(DAP_PACKET_COUNT, sizeof(debug_packet_t));
    s_reset_mutex = xSemaphoreCreateMutex();
    s_reset_complete = xSemaphoreCreateBinary();
    if ((s_request_queue == NULL) || (s_response_queue == NULL) ||
        (s_reset_mutex == NULL) || (s_reset_complete == NULL)) {
        dap_core_service_cleanup();
        return ESP_ERR_NO_MEM;
    }

    DAP_Setup();
    DAP_TransferAbort = 0U;

    if (xTaskCreatePinnedToCore(dap_core_task, "DAP-Core", 4096, NULL,
                                configMAX_PRIORITIES - 2, &s_core_task,
                                DAP_CORE_TASK_CORE) != pdPASS) {
        dap_core_service_cleanup();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Core task and packet queues initialized");
    return ESP_OK;
}

esp_err_t dap_core_service_reset(void)
{
    if ((s_core_task == NULL) || (s_reset_mutex == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_reset_mutex, portMAX_DELAY);
    DAP_TransferAbort = 1U;

    xQueueReset(s_request_queue);
    xQueueReset(s_response_queue);
    while (xSemaphoreTake(s_reset_complete, 0) == pdTRUE) {
    }

    const core_event_t reset_event = {
        .type = CORE_EVENT_RESET,
        .packet = {
            .size = 0U,
            .data = {0},
        },
    };

    esp_err_t err = ESP_OK;
    if (xQueueSendToFront(s_request_queue, &reset_event, portMAX_DELAY) != pdTRUE) {
        err = ESP_FAIL;
    } else if (xSemaphoreTake(s_reset_complete, portMAX_DELAY) != pdTRUE) {
        err = ESP_FAIL;
    }

    /* Discard a response produced by a request that was executing during reset. */
    xQueueReset(s_response_queue);
    xSemaphoreGive(s_reset_mutex);
    return err;
}

esp_err_t dap_core_service_submit(const uint8_t *data, size_t size)
{
    if ((data == NULL) || (size == 0U) || (size > DEBUG_PACKET_SIZE)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_request_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (data[0] == ID_DAP_TransferAbort) {
        DAP_TransferAbort = 1U;
        return ESP_OK;
    }

    core_event_t event = {
        .type = CORE_EVENT_REQUEST,
        .packet = {
            .size = size,
            .data = {0},
        },
    };
    memcpy(event.packet.data, data, size);

    return (xQueueSend(s_request_queue, &event, 0) == pdTRUE)
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

esp_err_t dap_core_service_receive(debug_packet_t *packet, TickType_t wait_ticks)
{
    if (packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_response_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return (xQueueReceive(s_response_queue, packet, wait_ticks) == pdTRUE)
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}
