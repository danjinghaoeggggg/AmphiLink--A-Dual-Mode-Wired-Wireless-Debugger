#include "network_tcp_server.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

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

struct network_tcp_server {
    network_tcp_server_config_t config;
    EventGroupHandle_t events;
    SemaphoreHandle_t client_mutex;
    TaskHandle_t server_task;
    int listen_socket;
    int client_socket;
    volatile bool started;
};

static const char *TAG = "NETWORK_TCP";

static bool server_enabled(network_tcp_server_t *server)
{
    return (xEventGroupGetBits(server->events) & SERVER_ENABLED_BIT) != 0U;
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

static int create_listening_socket(network_tcp_server_t *server)
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
        .sin_port = htons(server->config.port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if ((bind(socket_fd, (const struct sockaddr *)&address, sizeof(address)) !=
         0) ||
        (listen(socket_fd, 1) != 0)) {
        ESP_LOGE(TAG, "Unable to listen on port %u: errno=%d",
                 (unsigned int)server->config.port, errno);
        close(socket_fd);
        return -1;
    }
    ESP_LOGI(TAG, "Listening on TCP port %u", (unsigned int)server->config.port);
    return socket_fd;
}

static void serve_client(network_tcp_server_t *server, int socket_fd)
{
    uint8_t receive_buffer[SERVER_RX_BUFFER_SIZE];
    xSemaphoreTake(server->client_mutex, portMAX_DELAY);
    server->client_socket = socket_fd;
    xSemaphoreGive(server->client_mutex);
    server->config.client_changed(true, server->config.context);

    while (server->started && server_enabled(server)) {
        int received = recv(socket_fd, receive_buffer, sizeof(receive_buffer), 0);
        if (received > 0) {
            if (server->config.received(receive_buffer, (size_t)received,
                                        server->config.context) != ESP_OK) {
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

    xSemaphoreTake(server->client_mutex, portMAX_DELAY);
    if (server->client_socket == socket_fd) {
        server->client_socket = -1;
    }
    xSemaphoreGive(server->client_mutex);
    shutdown(socket_fd, SHUT_RDWR);
    close(socket_fd);
    server->config.client_changed(false, server->config.context);
}

static void server_task(void *argument)
{
    network_tcp_server_t *server = argument;
    while (server->started) {
        EventBits_t bits = xEventGroupWaitBits(
            server->events, SERVER_ENABLED_BIT | SERVER_STOP_REQUEST_BIT,
            pdFALSE, pdFALSE, portMAX_DELAY);
        if (((bits & SERVER_STOP_REQUEST_BIT) != 0U) || !server->started) {
            break;
        }
        int listen_socket = create_listening_socket(server);
        if (listen_socket < 0) {
            vTaskDelay(pdMS_TO_TICKS(SERVER_RECREATE_DELAY_MS));
            continue;
        }
        xSemaphoreTake(server->client_mutex, portMAX_DELAY);
        server->listen_socket = listen_socket;
        xSemaphoreGive(server->client_mutex);

        while (server->started && server_enabled(server)) {
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
                serve_client(server, client);
            }
        }
        xSemaphoreTake(server->client_mutex, portMAX_DELAY);
        if (server->listen_socket == listen_socket) {
            server->listen_socket = -1;
        }
        xSemaphoreGive(server->client_mutex);
        close(listen_socket);
    }
    xEventGroupSetBits(server->events, SERVER_STOPPED_BIT);
    vTaskSuspend(NULL);
}

esp_err_t network_tcp_server_start(network_tcp_server_t **output,
                                   const network_tcp_server_config_t *config)
{
    if ((output == NULL) || (config == NULL) || (config->port == 0U) ||
        (config->received == NULL) || (config->client_changed == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*output != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    network_tcp_server_t *server = calloc(1U, sizeof(*server));
    if (server == NULL) {
        return ESP_ERR_NO_MEM;
    }
    server->config = *config;
    server->listen_socket = -1;
    server->client_socket = -1;
    server->events = xEventGroupCreate();
    server->client_mutex = xSemaphoreCreateMutex();
    if ((server->events == NULL) || (server->client_mutex == NULL)) {
        if (server->events != NULL) {
            vEventGroupDelete(server->events);
        }
        if (server->client_mutex != NULL) {
            vSemaphoreDelete(server->client_mutex);
        }
        free(server);
        return ESP_ERR_NO_MEM;
    }
    server->started = true;
    if (xTaskCreatePinnedToCore(server_task, "Network-TCP", 4096, server,
                                configMAX_PRIORITIES - 4, &server->server_task,
                                0) != pdPASS) {
        vEventGroupDelete(server->events);
        vSemaphoreDelete(server->client_mutex);
        free(server);
        return ESP_ERR_NO_MEM;
    }
    *output = server;
    return ESP_OK;
}

esp_err_t network_tcp_server_stop(network_tcp_server_t *server)
{
    if ((server == NULL) || !server->started) {
        return ESP_ERR_INVALID_STATE;
    }
    server->started = false;
    xEventGroupClearBits(server->events, SERVER_ENABLED_BIT);
    xEventGroupSetBits(server->events, SERVER_STOP_REQUEST_BIT);
    xSemaphoreTake(server->client_mutex, portMAX_DELAY);
    if (server->client_socket >= 0) {
        shutdown(server->client_socket, SHUT_RDWR);
    }
    if (server->listen_socket >= 0) {
        shutdown(server->listen_socket, SHUT_RDWR);
    }
    xSemaphoreGive(server->client_mutex);

    EventBits_t bits = xEventGroupWaitBits(
        server->events, SERVER_STOPPED_BIT, pdFALSE, pdTRUE,
        pdMS_TO_TICKS(SERVER_STOP_TIMEOUT_MS));
    esp_err_t result = ESP_OK;
    if ((bits & SERVER_STOPPED_BIT) == 0U) {
        result = ESP_ERR_TIMEOUT;
    }
    if (server->server_task != NULL) {
        vTaskDelete(server->server_task);
    }
    if ((bits & SERVER_STOPPED_BIT) == 0U) {
        if (server->client_socket >= 0) {
            close(server->client_socket);
        }
        if (server->listen_socket >= 0) {
            close(server->listen_socket);
        }
    }
    vEventGroupDelete(server->events);
    vSemaphoreDelete(server->client_mutex);
    free(server);
    return result;
}

void network_tcp_server_set_enabled(network_tcp_server_t *server, bool enabled)
{
    if ((server == NULL) || !server->started) {
        return;
    }
    if (enabled) {
        xEventGroupSetBits(server->events, SERVER_ENABLED_BIT);
    } else {
        xEventGroupClearBits(server->events, SERVER_ENABLED_BIT);
        network_tcp_server_disconnect_client(server);
    }
}

bool network_tcp_server_has_client(network_tcp_server_t *server)
{
    if ((server == NULL) || (server->client_mutex == NULL)) {
        return false;
    }
    xSemaphoreTake(server->client_mutex, portMAX_DELAY);
    bool connected = server->client_socket >= 0;
    xSemaphoreGive(server->client_mutex);
    return connected;
}

esp_err_t network_tcp_server_send(network_tcp_server_t *server,
                                  const uint8_t *data, size_t size)
{
    if ((server == NULL) || (data == NULL) || (size == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((server->client_mutex == NULL) || !server_enabled(server)) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(server->client_mutex, portMAX_DELAY);
    if (server->client_socket < 0) {
        xSemaphoreGive(server->client_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    size_t sent = 0U;
    while (sent < size) {
        int result = send(server->client_socket, data + sent, size - sent, 0);
        if (result > 0) {
            sent += (size_t)result;
        } else if ((result < 0) && (errno == EINTR)) {
            continue;
        } else {
            xSemaphoreGive(server->client_mutex);
            return ESP_FAIL;
        }
    }
    xSemaphoreGive(server->client_mutex);
    return ESP_OK;
}

void network_tcp_server_disconnect_client(network_tcp_server_t *server)
{
    if ((server == NULL) || (server->client_mutex == NULL)) {
        return;
    }
    xSemaphoreTake(server->client_mutex, portMAX_DELAY);
    if (server->client_socket >= 0) {
        shutdown(server->client_socket, SHUT_RDWR);
    }
    xSemaphoreGive(server->client_mutex);
}
