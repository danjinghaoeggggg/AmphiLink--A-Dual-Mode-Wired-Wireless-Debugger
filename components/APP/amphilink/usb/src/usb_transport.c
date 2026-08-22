#include "usb_transport.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dap_core_service.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb.h"

_Static_assert(DEBUG_PACKET_SIZE == USB_VENDOR_PACKET_SIZE,
               "Debug and USB packet sizes must match");

static const char *TAG = "USB_TRANSPORT";
static TaskHandle_t s_transport_task;

#define USB_TRANSPORT_EVENT_DISCONNECTED (1UL << 0)

#define USB_TRANSPORT_TASK_CORE 0

static void usb_transport_rx(const uint8_t *data, size_t size, void *context)
{
    (void)context;
    if ((size == 0U) || (size > DEBUG_PACKET_SIZE)) {
        return;
    }

    esp_err_t err = dap_core_service_submit(data, size);
    if (err == ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "Request queue overflow");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Request submit failed: %s", esp_err_to_name(err));
    }
}

static void usb_transport_state_changed(bool mounted, void *context)
{
    (void)context;
    if (!mounted && (s_transport_task != NULL)) {
        xTaskNotify(s_transport_task, USB_TRANSPORT_EVENT_DISCONNECTED, eSetBits);
    }
}

static void usb_transport_task(void *argument)
{
    (void)argument;

    while (true) {
        uint32_t events = 0U;
        xTaskNotifyWait(0U, UINT32_MAX, &events, 0U);
        if ((events & USB_TRANSPORT_EVENT_DISCONNECTED) != 0U) {
            esp_err_t err = dap_core_service_reset();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Core reset failed: %s", esp_err_to_name(err));
            }
        }

        debug_packet_t response;
        esp_err_t err = dap_core_service_receive(&response, pdMS_TO_TICKS(20));
        if (err == ESP_ERR_TIMEOUT) {
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Response receive failed: %s", esp_err_to_name(err));
            continue;
        }

        err = usb_vendor_send_packet(response.data, response.size);
        if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
            ESP_LOGE(TAG, "Response send failed: %s", esp_err_to_name(err));
        }
    }
}

esp_err_t usb_transport_start(void)
{
    if (s_transport_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xTaskCreatePinnedToCore(usb_transport_task, "USB-Transport", 3072, NULL,
                                configMAX_PRIORITIES - 2, &s_transport_task,
                                USB_TRANSPORT_TASK_CORE) != pdPASS) {
        s_transport_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    const usb_config_t usb_config = {
        .vendor_rx = usb_transport_rx,
        .state_changed = usb_transport_state_changed,
        .context = NULL,
    };
    esp_err_t err = usb_init(&usb_config);
    if (err != ESP_OK) {
        vTaskDelete(s_transport_task);
        s_transport_task = NULL;
        return err;
    }

    ESP_LOGI(TAG, "USB transport started");
    return ESP_OK;
}
