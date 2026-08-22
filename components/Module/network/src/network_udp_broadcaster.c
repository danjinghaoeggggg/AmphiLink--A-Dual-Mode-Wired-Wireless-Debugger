#include "network_udp_broadcaster.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define BROADCASTER_TASK_STACK_SIZE 3072U
#define BROADCASTER_TASK_PRIORITY (tskIDLE_PRIORITY + 1U)

typedef struct {
    uint8_t payload[NETWORK_UDP_BROADCASTER_MAX_PAYLOAD_SIZE];
    size_t payload_size;
    uint16_t port;
    TickType_t interval_ticks;
    bool enabled;
    bool stop_requested;
} broadcaster_state_t;

static const char *TAG = "UDP_BROADCAST";
static SemaphoreHandle_t s_mutex;
static volatile TaskHandle_t s_task;
static broadcaster_state_t s_state;

static int create_broadcast_socket(void)
{
    int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd < 0) {
        return -1;
    }

    int enabled = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_BROADCAST, &enabled,
                   sizeof(enabled)) != 0) {
        close(socket_fd);
        return -1;
    }
    return socket_fd;
}

static void broadcaster_task(void *argument)
{
    (void)argument;
    int socket_fd = -1;

    while (true) {
        broadcaster_state_t state;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        state = s_state;
        xSemaphoreGive(s_mutex);

        if (state.stop_requested) {
            break;
        }
        if (!state.enabled) {
            if (socket_fd >= 0) {
                close(socket_fd);
                socket_fd = -1;
            }
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        if (socket_fd < 0) {
            socket_fd = create_broadcast_socket();
            if (socket_fd < 0) {
                ESP_LOGW(TAG, "Unable to create socket: errno=%d", errno);
            }
        }
        if ((socket_fd >= 0) && (state.payload_size > 0U)) {
            const struct sockaddr_in destination = {
                .sin_family = AF_INET,
                .sin_port = htons(state.port),
                .sin_addr.s_addr = htonl(INADDR_BROADCAST),
            };
            int sent = sendto(socket_fd, state.payload, state.payload_size, 0,
                              (const struct sockaddr *)&destination,
                              sizeof(destination));
            if (sent != (int)state.payload_size) {
                ESP_LOGW(TAG, "Broadcast failed: errno=%d", errno);
                close(socket_fd);
                socket_fd = -1;
            }
        }
        (void)ulTaskNotifyTake(pdTRUE, state.interval_ticks);
    }

    if (socket_fd >= 0) {
        close(socket_fd);
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t
network_udp_broadcaster_start(const network_udp_broadcaster_config_t *config)
{
    if ((config == NULL) || (config->port == 0U) ||
        (config->interval_ms == 0U) ||
        ((config->payload == NULL) && (config->payload_size != 0U)) ||
        (config->payload_size > NETWORK_UDP_BROADCASTER_MAX_PAYLOAD_SIZE) ||
        (pdMS_TO_TICKS(config->interval_ms) == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(&s_state, 0, sizeof(s_state));
    s_state.port = config->port;
    s_state.interval_ticks = pdMS_TO_TICKS(config->interval_ms);
    s_state.payload_size = config->payload_size;
    if (config->payload_size > 0U) {
        memcpy(s_state.payload, config->payload, config->payload_size);
    }

    TaskHandle_t task = NULL;
    if (xTaskCreatePinnedToCore(broadcaster_task, "UDP-Broadcast",
                                BROADCASTER_TASK_STACK_SIZE, NULL,
                                BROADCASTER_TASK_PRIORITY, &task, 0) != pdPASS) {
        s_task = NULL;
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_task = task;
    return ESP_OK;
}

esp_err_t network_udp_broadcaster_update_payload(const void *payload,
                                                 size_t payload_size)
{
    if ((payload == NULL) || (payload_size == 0U) ||
        (payload_size > NETWORK_UDP_BROADCASTER_MAX_PAYLOAD_SIZE)) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((s_task == NULL) || (s_mutex == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(s_state.payload, payload, payload_size);
    s_state.payload_size = payload_size;
    xSemaphoreGive(s_mutex);
    xTaskNotifyGive((TaskHandle_t)s_task);
    return ESP_OK;
}

esp_err_t network_udp_broadcaster_set_enabled(bool enabled)
{
    if ((s_task == NULL) || (s_mutex == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state.enabled = enabled;
    xSemaphoreGive(s_mutex);
    xTaskNotifyGive((TaskHandle_t)s_task);
    return ESP_OK;
}

esp_err_t network_udp_broadcaster_stop(void)
{
    if ((s_task == NULL) || (s_mutex == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state.stop_requested = true;
    xSemaphoreGive(s_mutex);
    xTaskNotifyGive((TaskHandle_t)s_task);
    while (s_task != NULL) {
        vTaskDelay(1);
    }
    vSemaphoreDelete(s_mutex);
    s_mutex = NULL;
    memset(&s_state, 0, sizeof(s_state));
    return ESP_OK;
}
