/*
 * SPDX-License-Identifier: MIT
 *
 * TinyUSB task and callback integration follows the upstream TinyUSB device
 * examples, adapted for ESP-IDF and this project's layered USB module.
 * Upstream: https://github.com/hathach/tinyusb
 * Local license copy: referance_project_licenses/tinyusb/LICENSE
 */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tusb.h"

#include "usb.h"
#include "bsp_usb.h"

static const char *TAG = "USB";
static usb_config_t s_config;
static SemaphoreHandle_t s_tx_event;
static bool s_initialized;

static void usb_init_cleanup(bool stack_initialized)
{
    if (stack_initialized && !tusb_deinit(BSP_USB_DEVICE_RHPORT)) {
        ESP_LOGE(TAG, "USB device stack deinit failed");
    }
    esp_err_t err = bsp_usb_phy_deinit();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        ESP_LOGE(TAG, "USB PHY deinit failed: %s", esp_err_to_name(err));
    }
    if (s_tx_event != NULL) {
        vSemaphoreDelete(s_tx_event);
        s_tx_event = NULL;
    }
    memset(&s_config, 0, sizeof(s_config));
}

static void tusb_device_task(void *arg)
{
    (void)arg;
    while (1) {
        tud_task();
    }
}

esp_err_t usb_init(const usb_config_t *config)
{
    if ((config == NULL) || (config->vendor_rx == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_tx_event = xSemaphoreCreateBinary();
    if (s_tx_event == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_config = *config;

    esp_err_t err = bsp_usb_phy_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB PHY init failed: %s", esp_err_to_name(err));
        usb_init_cleanup(false);
        return err;
    }

    const tusb_rhport_init_t dev_init = {
        .role = TUSB_ROLE_DEVICE,
#if BSP_USB_DEVICE_FULL_SPEED
        .speed = TUSB_SPEED_FULL,
#else
        .speed = TUSB_SPEED_HIGH,
#endif
    };
    if (!tusb_init(BSP_USB_DEVICE_RHPORT, &dev_init)) {
        ESP_LOGE(TAG, "USB device stack init failed");
        usb_init_cleanup(tusb_inited());
        return ESP_FAIL;
    }

    if (xTaskCreatePinnedToCore(tusb_device_task, "TinyUSB", 4096, NULL,
                                configMAX_PRIORITIES - 1, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "TinyUSB task create failed");
        usb_init_cleanup(true);
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "USB device stack initialized");
    return ESP_OK;
}

esp_err_t usb_vendor_send_packet(const uint8_t *data, size_t size)
{
    if ((data == NULL) || (size == 0U) || (size > USB_VENDOR_PACKET_SIZE)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    while (tud_mounted()) {
        if (tud_vendor_n_write_available(0) >= size) {
            uint32_t written = tud_vendor_n_write(0, data, (uint32_t)size);
            return (written == size) ? ESP_OK : ESP_FAIL;
        }
        xSemaphoreTake(s_tx_event, portMAX_DELAY);
    }

    return ESP_ERR_INVALID_STATE;
}

void tud_vendor_rx_cb(uint8_t interface, const uint8_t *buffer, uint16_t size)
{
    if ((interface != 0U) || (size == 0U)) {
        return;
    }
    if (size > USB_VENDOR_PACKET_SIZE) {
        ESP_LOGW(TAG, "Dropping oversized vendor packet: %u", size);
        return;
    }

    s_config.vendor_rx(buffer, size, s_config.context);
}

void tud_vendor_tx_cb(uint8_t interface, uint32_t sent_bytes)
{
    (void)sent_bytes;
    if ((interface == 0U) && (s_tx_event != NULL)) {
        xSemaphoreGive(s_tx_event);
    }
}

void tud_mount_cb(void)
{
    if (s_config.state_changed != NULL) {
        s_config.state_changed(true, s_config.context);
    }
}

void tud_umount_cb(void)
{
    if (s_tx_event != NULL) {
        xSemaphoreGive(s_tx_event);
    }
    if (s_config.state_changed != NULL) {
        s_config.state_changed(false, s_config.context);
    }
}
