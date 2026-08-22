#include "tcp_transport.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "DAP.h"
#include "cJSON.h"
#include "dap_core_service.h"
#include "dap_tcp_protocol.h"
#include "device_identity_store.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "network_tcp_server.h"
#include "network_udp_broadcaster.h"
#include "provisioning_server.h"
#include "sdkconfig.h"
#include "wifi_manager.h"
#include "wifi_profile_store.h"
#include "wireless_config.h"
#include "ws2812.h"

#define WS2812_BREATHE_PERIOD_MS 2000U

_Static_assert(DEBUG_PACKET_SIZE == DAP_TCP_MAX_PAYLOAD_SIZE,
               "Debug and TCP payload sizes must match");

static const char *TAG = "TCP_TRANSPORT";
static TaskHandle_t s_response_task;
static dap_tcp_parser_t s_parser;
static SemaphoreHandle_t s_state_mutex;
static device_identity_t s_identity;
static bool s_wifi_connected;

typedef struct {
    wifi_profile_snapshot_t snapshot;
    wifi_manager_credential_t credentials[WIFI_MANAGER_MAX_CREDENTIALS];
} wifi_transport_profile_buffer_t;

#define TCP_TRANSPORT_TASK_CORE 0

static esp_err_t submit_request(const uint8_t *payload, size_t size,
                                void *context)
{
    (void)context;
    esp_err_t err = dap_core_service_submit(payload, size);
    if (err == ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "DAP request queue overflow");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "DAP request rejected: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t tcp_received(const uint8_t *data, size_t size, void *context)
{
    (void)context;
    esp_err_t err =
        dap_tcp_parser_feed(&s_parser, data, size, submit_request, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Invalid CMSIS-DAP TCP stream: %s", esp_err_to_name(err));
    }
    return err;
}

static void tcp_client_changed(bool connected, void *context)
{
    (void)context;
    dap_tcp_parser_reset(&s_parser);
    if (connected) {
        ESP_LOGI(TAG, "CMSIS-DAP TCP session started");
        return;
    }

    esp_err_t err = dap_core_service_reset();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DAP core reset failed: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "CMSIS-DAP TCP session ended");
}

static void response_task(void *argument)
{
    (void)argument;
    uint8_t frame[DAP_TCP_MAX_FRAME_SIZE];

    while (true) {
        debug_packet_t response;
        esp_err_t err = dap_core_service_receive(&response, pdMS_TO_TICKS(20));
        if (err == ESP_ERR_TIMEOUT) {
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Response receive failed: %s", esp_err_to_name(err));
            continue;
        }

        size_t frame_size = 0U;
        err = dap_tcp_encode_response(response.data, response.size, frame,
                                      sizeof(frame), &frame_size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Response frame encoding failed: %s",
                     esp_err_to_name(err));
            network_tcp_server_disconnect_client();
            continue;
        }

        err = network_tcp_server_send(frame, frame_size);
        if (err == ESP_ERR_INVALID_STATE) {
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Response send failed: %s", esp_err_to_name(err));
            network_tcp_server_disconnect_client();
        }
    }
}

static void fill_manager_credentials(const wifi_profile_snapshot_t *snapshot,
                                     wifi_manager_credential_t *credentials)
{
    memset(credentials, 0,
           WIFI_MANAGER_MAX_CREDENTIALS * sizeof(credentials[0]));
    for (size_t index = 0U; index < snapshot->count; ++index) {
        credentials[index].id = snapshot->profiles[index].id;
        snprintf(credentials[index].ssid, sizeof(credentials[index].ssid), "%s",
                 snapshot->profiles[index].ssid);
        snprintf(credentials[index].password,
                 sizeof(credentials[index].password), "%s",
                 snapshot->profiles[index].password);
    }
}

static esp_err_t save_candidate(const wifi_manager_credential_t *credential,
                                void *context)
{
    (void)context;
    wifi_transport_profile_buffer_t *buffer = calloc(1U, sizeof(*buffer));
    if (buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = wifi_profile_store_upsert_and_promote(
        credential->ssid, credential->password, &buffer->snapshot);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Unable to save Wi-Fi profile: %s", esp_err_to_name(err));
        free(buffer);
        return err;
    }
    fill_manager_credentials(&buffer->snapshot, buffer->credentials);
    err = wifi_manager_set_credentials(buffer->credentials,
                                       buffer->snapshot.count);
    free(buffer);
    return err;
}

static void log_runtime_error(const char *operation, esp_err_t err)
{
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s failed: %s", operation, esp_err_to_name(err));
    }
}

static void update_discovery_locked(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "Unable to allocate discovery payload");
        return;
    }
    cJSON_AddStringToObject(root, "name", s_identity.name);
    cJSON_AddNumberToObject(root, "id", s_identity.id);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        ESP_LOGE(TAG, "Unable to encode discovery payload");
        return;
    }
    log_runtime_error(
        "Discovery payload update",
        network_udp_broadcaster_update_payload(payload, strlen(payload)));
    cJSON_free(payload);
}

static void update_indicator_locked(void)
{
    log_runtime_error("WS2812 breathing stop", ws2812_stop_breathe());
    esp_err_t err = s_wifi_connected
                        ? ws2812_set_color((ws2812_color_t)s_identity.id)
                        : ws2812_breathe((ws2812_color_t)s_identity.id,
                                         WS2812_BREATHE_PERIOD_MS, UINT32_MAX);
    log_runtime_error("WS2812 state update", err);
}

static void identity_changed(const device_identity_t *identity, void *context)
{
    (void)context;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_identity = *identity;
    update_indicator_locked();
    if (s_wifi_connected) {
        update_discovery_locked();
    }
    xSemaphoreGive(s_state_mutex);
}

static void wifi_connection_changed(bool connected, void *context)
{
    (void)context;
    network_tcp_server_set_enabled(connected);

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_wifi_connected = connected;
    update_indicator_locked();
    if (connected) {
        update_discovery_locked();
    }
    log_runtime_error("Discovery state update",
                      network_udp_broadcaster_set_enabled(connected));
    xSemaphoreGive(s_state_mutex);
}

static void log_cleanup_error(const char *resource, esp_err_t err)
{
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        ESP_LOGE(TAG, "%s cleanup failed: %s", resource, esp_err_to_name(err));
    }
}

static void tcp_transport_start_cleanup(bool response_task_started,
                                        bool provisioning_started,
                                        bool wifi_manager_started,
                                        bool broadcaster_started,
                                        bool network_server_started,
                                        bool ws2812_started)
{
    if (response_task_started && (s_response_task != NULL)) {
        vTaskDelete(s_response_task);
        s_response_task = NULL;
    }
    if (provisioning_started) {
        log_cleanup_error("Provisioning server", provisioning_server_stop());
    }
    if (wifi_manager_started) {
        log_cleanup_error("Wi-Fi manager", wifi_manager_stop());
    }
    if (broadcaster_started) {
        log_cleanup_error("UDP broadcaster", network_udp_broadcaster_stop());
    }
    if (network_server_started) {
        log_cleanup_error("TCP server", network_tcp_server_stop());
    }
    if (ws2812_started) {
        log_cleanup_error("WS2812", ws2812_deinit());
    }
    if (s_state_mutex != NULL) {
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = NULL;
    }
    s_wifi_connected = false;
    memset(&s_identity, 0, sizeof(s_identity));
    dap_tcp_parser_reset(&s_parser);
}

esp_err_t tcp_transport_start(void)
{
    if ((s_response_task != NULL) || (s_state_mutex != NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = device_identity_store_init();
    if (err != ESP_OK) {
        return err;
    }
    err = device_identity_store_get(&s_identity);
    if (err != ESP_OK) {
        return err;
    }
    s_state_mutex = xSemaphoreCreateMutex();
    if (s_state_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    bool ws2812_started = false;
    err = ws2812_init();
    if (err != ESP_OK) {
        goto failed_early;
    }
    ws2812_started = true;
    s_wifi_connected = false;
    update_indicator_locked();

    err = wifi_profile_store_init();
    if (err != ESP_OK) {
        goto failed_early;
    }

    wifi_transport_profile_buffer_t *buffer = calloc(1U, sizeof(*buffer));
    if (buffer == NULL) {
        err = ESP_ERR_NO_MEM;
        goto failed_early;
    }
    err = wifi_profile_store_get(&buffer->snapshot);
    if (err != ESP_OK) {
        free(buffer);
        goto failed_early;
    }

    bool provisioning_started = false;
    bool wifi_manager_started = false;
    bool broadcaster_started = false;
    bool network_server_started = false;
    bool response_task_started = false;

    /* Keep the DAP server disabled until Wi-Fi has initialized lwIP. */
    const network_tcp_server_config_t server_config = {
        .port = DAP_TCP_SERVER_PORT,
        .received = tcp_received,
        .client_changed = tcp_client_changed,
        .context = NULL,
    };
    err = network_tcp_server_start(&server_config);
    if (err != ESP_OK) {
        goto failed;
    }
    network_server_started = true;

    const char initial_payload[] = "{}";
    const network_udp_broadcaster_config_t broadcaster_config = {
        .port = DAP_DISCOVERY_UDP_PORT,
        .interval_ms = DAP_DISCOVERY_INTERVAL_MS,
        .payload = initial_payload,
        .payload_size = sizeof(initial_payload) - 1U,
    };
    err = network_udp_broadcaster_start(&broadcaster_config);
    if (err != ESP_OK) {
        goto failed;
    }
    broadcaster_started = true;

    fill_manager_credentials(&buffer->snapshot, buffer->credentials);
    const wifi_manager_config_t wifi_config = {
        .credentials = buffer->credentials,
        .credential_count = buffer->snapshot.count,
        .softap_password = ADMIN_PASSWORD,
        .save_candidate = save_candidate,
        .connection_changed = wifi_connection_changed,
        .context = NULL,
    };
    err = wifi_manager_start(&wifi_config);
    if (err != ESP_OK) {
        goto failed;
    }
    wifi_manager_started = true;
    free(buffer);
    buffer = NULL;

    /* The HTTP server creates sockets, so lwIP must be ready first. */
    const provisioning_server_config_t provisioning_config = {
        .identity_changed = identity_changed,
        .context = NULL,
    };
    err = provisioning_server_start(&provisioning_config);
    if (err != ESP_OK) {
        goto failed;
    }
    provisioning_started = true;

    dap_tcp_parser_reset(&s_parser);
    if (xTaskCreatePinnedToCore(response_task, "TCP-Response", 3072, NULL,
                                configMAX_PRIORITIES - 3, &s_response_task,
                                TCP_TRANSPORT_TASK_CORE) != pdPASS) {
        s_response_task = NULL;
        err = ESP_ERR_NO_MEM;
        goto failed;
    }
    response_task_started = true;

    ESP_LOGI(TAG, "Wireless transport and provisioning portal started");
    return ESP_OK;

failed:
    free(buffer);
    tcp_transport_start_cleanup(response_task_started, provisioning_started,
                                wifi_manager_started, broadcaster_started,
                                network_server_started, ws2812_started);
    return err;

failed_early:
    tcp_transport_start_cleanup(false, false, false, false, false,
                                ws2812_started);
    return err;
}
