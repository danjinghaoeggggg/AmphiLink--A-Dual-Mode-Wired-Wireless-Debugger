#include "network_tcp_server.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define SERVER_ENABLED_BIT BIT0
#define SERVER_STOP_REQUEST_BIT BIT1
#define SERVER_STOPPED_BIT BIT2
#define SERVER_IO_TIMEOUT_MS 500
#define SERVER_SEND_TIMEOUT_MS 1000
#define SERVER_RECREATE_DELAY_MS 500
#define SERVER_STOP_TIMEOUT_MS 1500
#define SERVER_RX_BUFFER_SIZE 256

static const char *TAG = "NETWORK_TCP";
static network_tcp_server_config_t s_config;
static EventGroupHandle_t s_events;
static SemaphoreHandle_t s_client_mutex;
static TaskHandle_t s_server_task;
static int s_listen_socket = -1;
static int s_client_socket = -1;
static bool s_started;

static bool server_enabled(void)
{
    return (xEventGroupGetBits(s_events) & SERVER_ENABLED_BIT) != 0U;
}

static void set_socket_timeouts(int socket_fd)
{
    const struct timeval receive_timeout = {
        .tv_sec = 0,
        .tv_usec = SERVER_IO_TIMEOUT_MS * 1000,
    };
    const struct timeval send_timeout = {
        .tv_sec = SERVER_SEND_TIMEOUT_MS / 1000,
        .tv_usec = (SERVER_SEND_TIMEOUT_MS % 1000) * 1000,
    };
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
               sizeof(receive_timeout));
    setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout,
               sizeof(send_timeout));
}

static void configure_client_socket(int socket_fd)
{
    const int enabled = 1;
    const int keepalive_idle = 10;
    const int keepalive_interval = 5;
    const int keepalive_count = 3;
    setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
    setsockopt(socket_fd, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled));
    setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepalive_idle,
               sizeof(keepalive_idle));
    setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepalive_interval,
               sizeof(keepalive_interval));
    setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPCNT, &keepalive_count,
               sizeof(keepalive_count));
    set_socket_timeouts(socket_fd);
}

static int create_listening_socket(void)
{
    int socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (socket_fd < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno=%d", errno);
        return -1;
    }
    const int reuse_address = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_address,
               sizeof(reuse_address));
    const struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(s_config.port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if ((bind(socket_fd, (const struct sockaddr *)&address, sizeof(address)) !=
         0) ||
        (listen(socket_fd, 1) != 0)) {
        ESP_LOGE(TAG, "Unable to listen on port %u: errno=%d",
                 (unsigned int)s_config.port, errno);
        close(socket_fd);
        return -1;
    }
    ESP_LOGI(TAG, "Listening on TCP port %u", (unsigned int)s_config.port);
    return socket_fd;
}

static void serve_client(int socket_fd)
{
    uint8_t receive_buffer[SERVER_RX_BUFFER_SIZE];
    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    s_client_socket = socket_fd;
    xSemaphoreGive(s_client_mutex);
    s_config.client_changed(true, s_config.context);

    while (server_enabled()) {
        int received = recv(socket_fd, receive_buffer, sizeof(receive_buffer), 0);
        if (received > 0) {
            if (s_config.received(receive_buffer, (size_t)received,
                                  s_config.context) != ESP_OK) {
                break;
            }
        } else if (received == 0) {
            break;
        } else if ((errno != EINTR) && (errno != EAGAIN) &&
                   (errno != EWOULDBLOCK) && (errno != ETIMEDOUT)) {
            ESP_LOGW(TAG, "TCP receive failed: errno=%d", errno);
            break;
        }
    }

    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    if (s_client_socket == socket_fd) {
        s_client_socket = -1;
    }
    xSemaphoreGive(s_client_mutex);
    shutdown(socket_fd, SHUT_RDWR);
    close(socket_fd);
    s_config.client_changed(false, s_config.context);
}

static void server_task(void *argument)
{
    (void)argument;
    while (s_started) {
        EventBits_t bits = xEventGroupWaitBits(
            s_events, SERVER_ENABLED_BIT | SERVER_STOP_REQUEST_BIT, pdFALSE,
            pdFALSE, portMAX_DELAY);
        if (((bits & SERVER_STOP_REQUEST_BIT) != 0U) || !s_started) {
            break;
        }
        int listen_socket = create_listening_socket();
        if (listen_socket < 0) {
            vTaskDelay(pdMS_TO_TICKS(SERVER_RECREATE_DELAY_MS));
            continue;
        }
        xSemaphoreTake(s_client_mutex, portMAX_DELAY);
        s_listen_socket = listen_socket;
        xSemaphoreGive(s_client_mutex);

        while (s_started && server_enabled()) {
            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(listen_socket, &read_set);
            struct timeval timeout = {.tv_sec = 0, .tv_usec = 500000};
            int ready = select(listen_socket + 1, &read_set, NULL, NULL,
                               &timeout);
            if (ready < 0) {
                if (errno != EINTR) {
                    ESP_LOGW(TAG, "TCP select failed: errno=%d", errno);
                    break;
                }
                continue;
            }
            if (ready == 0) {
                continue;
            }
            struct sockaddr_in peer;
            socklen_t peer_size = sizeof(peer);
            int client = accept(listen_socket, (struct sockaddr *)&peer,
                                &peer_size);
            if (client >= 0) {
                configure_client_socket(client);
                serve_client(client);
            }
        }
        xSemaphoreTake(s_client_mutex, portMAX_DELAY);
        if (s_listen_socket == listen_socket) {
            s_listen_socket = -1;
        }
        xSemaphoreGive(s_client_mutex);
        close(listen_socket);
    }
    xEventGroupSetBits(s_events, SERVER_STOPPED_BIT);
    vTaskSuspend(NULL);
}

esp_err_t network_tcp_server_start(const network_tcp_server_config_t *config)
{
    if ((config == NULL) || (config->port == 0U) ||
        (config->received == NULL) || (config->client_changed == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    s_events = xEventGroupCreate();
    s_client_mutex = xSemaphoreCreateMutex();
    if ((s_events == NULL) || (s_client_mutex == NULL)) {
        if (s_events != NULL) {
            vEventGroupDelete(s_events);
        }
        if (s_client_mutex != NULL) {
            vSemaphoreDelete(s_client_mutex);
        }
        return ESP_ERR_NO_MEM;
    }
    s_config = *config;
    s_started = true;
    if (xTaskCreatePinnedToCore(server_task, "Network-TCP", 4096, NULL,
                                configMAX_PRIORITIES - 4, &s_server_task,
                                0) != pdPASS) {
        s_started = false;
        vEventGroupDelete(s_events);
        vSemaphoreDelete(s_client_mutex);
        s_events = NULL;
        s_client_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t network_tcp_server_stop(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    s_started = false;
    xEventGroupClearBits(s_events, SERVER_ENABLED_BIT);
    xEventGroupSetBits(s_events, SERVER_STOP_REQUEST_BIT);

    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    if (s_client_socket >= 0) {
        shutdown(s_client_socket, SHUT_RDWR);
    }
    if (s_listen_socket >= 0) {
        shutdown(s_listen_socket, SHUT_RDWR);
    }
    xSemaphoreGive(s_client_mutex);

    EventBits_t bits = xEventGroupWaitBits(
        s_events, SERVER_STOPPED_BIT, pdFALSE, pdTRUE,
        pdMS_TO_TICKS(SERVER_STOP_TIMEOUT_MS));
    esp_err_t result = ESP_OK;
    if ((bits & SERVER_STOPPED_BIT) == 0U) {
        result = ESP_ERR_TIMEOUT;
    }
    if (s_server_task != NULL) {
        vTaskDelete(s_server_task);
    }
    if ((bits & SERVER_STOPPED_BIT) == 0U) {
        if (s_client_socket >= 0) {
            close(s_client_socket);
        }
        if (s_listen_socket >= 0) {
            close(s_listen_socket);
        }
    }

    s_server_task = NULL;
    s_client_socket = -1;
    s_listen_socket = -1;
    vEventGroupDelete(s_events);
    vSemaphoreDelete(s_client_mutex);
    s_events = NULL;
    s_client_mutex = NULL;
    memset(&s_config, 0, sizeof(s_config));
    return result;
}

void network_tcp_server_set_enabled(bool enabled)
{
    if (!s_started) {
        return;
    }
    if (enabled) {
        xEventGroupSetBits(s_events, SERVER_ENABLED_BIT);
    } else {
        xEventGroupClearBits(s_events, SERVER_ENABLED_BIT);
        network_tcp_server_disconnect_client();
    }
}

bool network_tcp_server_has_client(void)
{
    if (s_client_mutex == NULL) {
        return false;
    }
    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    bool connected = s_client_socket >= 0;
    xSemaphoreGive(s_client_mutex);
    return connected;
}

esp_err_t network_tcp_server_send(const uint8_t *data, size_t size)
{
    if ((data == NULL) || (size == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((s_client_mutex == NULL) || !server_enabled()) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    if (s_client_socket < 0) {
        xSemaphoreGive(s_client_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    size_t sent = 0U;
    while (sent < size) {
        int result = send(s_client_socket, data + sent, size - sent, 0);
        if (result > 0) {
            sent += (size_t)result;
        } else if ((result < 0) && (errno == EINTR)) {
            continue;
        } else {
            xSemaphoreGive(s_client_mutex);
            return ESP_FAIL;
        }
    }
    xSemaphoreGive(s_client_mutex);
    return ESP_OK;
}

void network_tcp_server_disconnect_client(void)
{
    if (s_client_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    if (s_client_socket >= 0) {
        shutdown(s_client_socket, SHUT_RDWR);
    }
    xSemaphoreGive(s_client_mutex);
}
