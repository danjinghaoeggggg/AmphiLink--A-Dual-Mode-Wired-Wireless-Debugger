#include "wifi_manager.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"

#define WIFI_READY_BIT BIT0
#define WIFI_CONNECTED_BIT BIT1
#define WIFI_DISCONNECTED_BIT BIT2
#define WIFI_STOP_REQUEST_BIT BIT3
#define WIFI_STOPPED_BIT BIT4

#define WIFI_MANAGER_TASK_CORE 0
#define WIFI_MANAGER_COMMAND_QUEUE_SIZE 4U
#define WIFI_MANAGER_CONNECT_TIMEOUT_MS 8000U
#define WIFI_MANAGER_CANDIDATE_TIMEOUT_MS 12000U
#define WIFI_MANAGER_AP_HANDOFF_MS 3000U
#define WIFI_MANAGER_COMMAND_SETTLE_MS 250U
#define WIFI_MANAGER_AP_STOP_RETRIES 3U
#define WIFI_MANAGER_AP_STOP_RETRY_MS 100U
#define WIFI_MANAGER_STOP_TIMEOUT_MS 2000U

typedef enum {
    WIFI_COMMAND_SCAN,
    WIFI_COMMAND_CONNECT,
} wifi_command_type_t;

typedef struct {
    wifi_command_type_t type;
    wifi_manager_credential_t credential;
} wifi_command_t;

static const char *TAG = "WIFI_MANAGER";
static wifi_manager_config_t s_config;
static wifi_manager_credential_t s_credentials[WIFI_MANAGER_MAX_CREDENTIALS];
static size_t s_credential_count;
static wifi_manager_credential_t s_current;
static bool s_has_current;
static SemaphoreHandle_t s_mutex;
static EventGroupHandle_t s_wifi_events;
static QueueHandle_t s_commands;
static TaskHandle_t s_manager_task;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static wifi_manager_status_t s_status;
static wifi_manager_scan_result_t s_scan_result;
static wifi_manager_connect_result_t s_connect_result;
static uint32_t s_next_operation_id;
static uint16_t s_disconnect_reason;
static bool s_started;
static bool s_stopping;
static bool s_netif_initialized;
static bool s_event_loop_created;
static bool s_wifi_initialized;
static bool s_wifi_started;
static bool s_wifi_handler_registered;
static bool s_ip_handler_registered;

static bool stop_requested(void)
{
    return s_stopping ||
           ((s_wifi_events != NULL) &&
            ((xEventGroupGetBits(s_wifi_events) & WIFI_STOP_REQUEST_BIT) !=
             0U));
}

static bool wait_for_stop(TickType_t wait_ticks)
{
    return (xEventGroupWaitBits(s_wifi_events, WIFI_STOP_REQUEST_BIT, pdFALSE,
                                pdTRUE, wait_ticks) &
            WIFI_STOP_REQUEST_BIT) != 0U;
}

static void set_status(wifi_manager_mode_t mode, const char *ssid,
                       const char *ip)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.mode = mode;
    if (ssid != NULL) {
        snprintf(s_status.ssid, sizeof(s_status.ssid), "%s", ssid);
    } else {
        s_status.ssid[0] = '\0';
    }
    if (ip != NULL) {
        snprintf(s_status.ip, sizeof(s_status.ip), "%s", ip);
    } else {
        s_status.ip[0] = '\0';
    }
    xSemaphoreGive(s_mutex);
}

static void wifi_event_handler(void *argument, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)argument;
    if ((event_base == WIFI_EVENT) && (event_id == WIFI_EVENT_STA_START)) {
        xEventGroupSetBits(s_wifi_events, WIFI_READY_BIT);
    } else if ((event_base == WIFI_EVENT) &&
               (event_id == WIFI_EVENT_STA_DISCONNECTED)) {
        wifi_event_sta_disconnected_t *event = event_data;
        s_disconnect_reason = event->reason;
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        xEventGroupSetBits(s_wifi_events, WIFI_DISCONNECTED_BIT);
    } else if ((event_base == IP_EVENT) &&
               (event_id == IP_EVENT_STA_GOT_IP)) {
        ip_event_got_ip_t *event = event_data;
        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        snprintf(s_status.ip, sizeof(s_status.ip), "%s", ip);
        xSemaphoreGive(s_mutex);
        xEventGroupClearBits(s_wifi_events, WIFI_DISCONNECTED_BIT);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t set_station_config(
    const wifi_manager_credential_t *credential)
{
    wifi_config_t config = {0};
    size_t ssid_length = strlen(credential->ssid);
    size_t password_length = strlen(credential->password);
    memcpy(config.sta.ssid, credential->ssid, ssid_length);
    memcpy(config.sta.password, credential->password, password_length);
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.threshold.authmode =
        (password_length == 0U) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_PSK;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    return esp_wifi_set_config(WIFI_IF_STA, &config);
}

static esp_err_t connect_credential(
    const wifi_manager_credential_t *credential, bool keep_ap,
    TickType_t timeout)
{
    if (stop_requested()) {
        return ESP_ERR_INVALID_STATE;
    }
    (void)esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    xEventGroupClearBits(s_wifi_events,
                         WIFI_CONNECTED_BIT | WIFI_DISCONNECTED_BIT);

    esp_err_t err = esp_wifi_set_mode(keep_ap ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }
    err = set_station_config(credential);
    if (err != ESP_OK) {
        return err;
    }
    set_status(WIFI_MANAGER_MODE_CONNECTING, credential->ssid, NULL);
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events,
        WIFI_CONNECTED_BIT | WIFI_DISCONNECTED_BIT | WIFI_STOP_REQUEST_BIT,
        pdFALSE, pdFALSE, timeout);
    if ((bits & WIFI_STOP_REQUEST_BIT) != 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((bits & WIFI_CONNECTED_BIT) != 0U) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status.mode = WIFI_MANAGER_MODE_STA;
        snprintf(s_status.ssid, sizeof(s_status.ssid), "%s", credential->ssid);
        xSemaphoreGive(s_mutex);
        ESP_LOGI(TAG, "Connected to '%s'", credential->ssid);
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Connection to '%s' failed, reason=%u", credential->ssid,
             (unsigned int)s_disconnect_reason);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t start_softap(void)
{
    if (stop_requested()) {
        return ESP_ERR_INVALID_STATE;
    }
    (void)esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    wifi_config_t config = {0};
    char ap_ssid[WIFI_MANAGER_SSID_SIZE];
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    snprintf(ap_ssid, sizeof(ap_ssid), "%s", s_status.ap_ssid);
    xSemaphoreGive(s_mutex);
    size_t ssid_length = strlen(ap_ssid);
    size_t password_length = strlen(s_config.softap_password);
    memcpy(config.ap.ssid, ap_ssid, ssid_length);
    memcpy(config.ap.password, s_config.softap_password, password_length);
    config.ap.ssid_len = (uint8_t)ssid_length;
    config.ap.channel = 1U;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    config.ap.max_connection = 4U;
    config.ap.pmf_cfg.capable = true;
    config.ap.pmf_cfg.required = false;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &config);
    }
    if (err != ESP_OK) {
        return err;
    }
    s_has_current = false;
    xEventGroupClearBits(s_wifi_events,
                         WIFI_CONNECTED_BIT | WIFI_DISCONNECTED_BIT);
    set_status(WIFI_MANAGER_MODE_AP, ap_ssid, "192.168.4.1");
    s_config.connection_changed(false, s_config.context);
    ESP_LOGI(TAG, "Provisioning AP '%s' available at 192.168.4.1",
             ap_ssid);
    return ESP_OK;
}

static void copy_credentials(wifi_manager_credential_t *credentials,
                             size_t *count)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *count = s_credential_count;
    memcpy(credentials, s_credentials,
           s_credential_count * sizeof(s_credentials[0]));
    xSemaphoreGive(s_mutex);
}

static esp_err_t try_saved_credentials(const char *skip_ssid)
{
    wifi_manager_credential_t credentials[WIFI_MANAGER_MAX_CREDENTIALS];
    size_t count;
    copy_credentials(credentials, &count);
    for (size_t index = 0U; index < count; ++index) {
        if (stop_requested()) {
            return ESP_ERR_INVALID_STATE;
        }
        if ((skip_ssid != NULL) &&
            (strcmp(skip_ssid, credentials[index].ssid) == 0)) {
            continue;
        }
        esp_err_t err = connect_credential(
            &credentials[index], false,
            pdMS_TO_TICKS(WIFI_MANAGER_CONNECT_TIMEOUT_MS));
        if (err == ESP_OK) {
            if (stop_requested()) {
                return ESP_ERR_INVALID_STATE;
            }
            s_current = credentials[index];
            s_has_current = true;
            s_config.connection_changed(true, s_config.context);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t recover_connection(void)
{
    wifi_manager_credential_t lost = s_current;
    wifi_manager_credential_t credentials[WIFI_MANAGER_MAX_CREDENTIALS];
    size_t count;
    copy_credentials(credentials, &count);
    bool remembered = false;
    for (size_t index = 0U; index < count; ++index) {
        if (strcmp(credentials[index].ssid, lost.ssid) == 0) {
            lost = credentials[index];
            remembered = true;
            break;
        }
    }
    s_config.connection_changed(false, s_config.context);
    for (unsigned int retry = 0U; remembered && (retry < 3U); ++retry) {
        if (wait_for_stop(pdMS_TO_TICKS(1000U << retry))) {
            return ESP_ERR_INVALID_STATE;
        }
        if (connect_credential(&lost, false,
                               pdMS_TO_TICKS(WIFI_MANAGER_CONNECT_TIMEOUT_MS)) ==
            ESP_OK) {
            if (stop_requested()) {
                return ESP_ERR_INVALID_STATE;
            }
            s_current = lost;
            s_has_current = true;
            s_config.connection_changed(true, s_config.context);
            return ESP_OK;
        }
    }
    if (try_saved_credentials(remembered ? lost.ssid : NULL) == ESP_OK) {
        return ESP_OK;
    }
    return start_softap();
}

static int compare_scan_records(const void *left, const void *right)
{
    const wifi_manager_scan_record_t *a = left;
    const wifi_manager_scan_record_t *b = right;
    return (int)b->rssi - (int)a->rssi;
}

static void handle_scan(void)
{
    if (stop_requested()) {
        return;
    }
    wifi_manager_status_t status;
    wifi_manager_get_status(&status);
    bool from_ap = status.mode == WIFI_MANAGER_MODE_AP;
    if (from_ap) {
        (void)esp_wifi_set_mode(WIFI_MODE_APSTA);
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.scanning = true;
    xSemaphoreGive(s_mutex);

    wifi_scan_config_t scan_config = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    wifi_ap_record_t raw[WIFI_MANAGER_MAX_SCAN_RESULTS * 2U];
    uint16_t raw_count = WIFI_MANAGER_MAX_SCAN_RESULTS * 2U;
    if (err == ESP_OK) {
        err = esp_wifi_scan_get_ap_records(&raw_count, raw);
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_scan_result.count = 0U;
    s_scan_result.error = err;
    if (err == ESP_OK) {
        for (uint16_t index = 0U; index < raw_count; ++index) {
            if (raw[index].ssid[0] == '\0') {
                continue;
            }
            int duplicate = -1;
            for (size_t existing = 0U; existing < s_scan_result.count;
                 ++existing) {
                if (strcmp(s_scan_result.records[existing].ssid,
                           (const char *)raw[index].ssid) == 0) {
                    duplicate = (int)existing;
                    break;
                }
            }
            wifi_manager_scan_record_t record = {
                .rssi = raw[index].rssi,
                .channel = raw[index].primary,
                .authmode = raw[index].authmode,
            };
            snprintf(record.ssid, sizeof(record.ssid), "%s", raw[index].ssid);
            if ((duplicate >= 0) &&
                (record.rssi > s_scan_result.records[duplicate].rssi)) {
                s_scan_result.records[duplicate] = record;
            } else if ((duplicate < 0) &&
                       (s_scan_result.count < WIFI_MANAGER_MAX_SCAN_RESULTS)) {
                s_scan_result.records[s_scan_result.count++] = record;
            }
        }
        qsort(s_scan_result.records, s_scan_result.count,
              sizeof(s_scan_result.records[0]), compare_scan_records);
        s_scan_result.state = WIFI_MANAGER_OPERATION_SUCCEEDED;
    } else {
        s_scan_result.state = WIFI_MANAGER_OPERATION_FAILED;
    }
    s_status.scanning = false;
    xSemaphoreGive(s_mutex);
    if (from_ap) {
        (void)esp_wifi_set_mode(WIFI_MODE_AP);
    }
}

static void set_connect_result(wifi_manager_operation_state_t state,
                               esp_err_t error)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_connect_result.state = state;
    s_connect_result.error = error;
    xSemaphoreGive(s_mutex);
}

static esp_err_t stop_softap_keep_station(void)
{
    esp_err_t err = ESP_FAIL;
    for (unsigned int attempt = 0U; attempt < WIFI_MANAGER_AP_STOP_RETRIES;
         ++attempt) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Provisioning AP disabled after successful setup");
            return ESP_OK;
        }
        ESP_LOGW(TAG, "Unable to disable provisioning AP (attempt %u): %s",
                 attempt + 1U, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(WIFI_MANAGER_AP_STOP_RETRY_MS));
    }
    ESP_LOGE(TAG, "Provisioning AP remains enabled: %s",
             esp_err_to_name(err));
    return err;
}

static void handle_candidate(const wifi_manager_credential_t *candidate)
{
    /* Let the HTTP worker flush its 202 response before changing interfaces. */
    if (wait_for_stop(pdMS_TO_TICKS(WIFI_MANAGER_COMMAND_SETTLE_MS))) {
        return;
    }
    wifi_manager_status_t status;
    wifi_manager_get_status(&status);
    bool from_ap = status.mode == WIFI_MANAGER_MODE_AP;
    wifi_manager_credential_t previous = s_current;
    bool had_previous = s_has_current && !from_ap;
    s_config.connection_changed(false, s_config.context);

    esp_err_t err = connect_credential(
        candidate, from_ap,
        pdMS_TO_TICKS(WIFI_MANAGER_CANDIDATE_TIMEOUT_MS));
    if (err == ESP_OK) {
        err = s_config.save_candidate(candidate, s_config.context);
    }
    if (err == ESP_OK) {
        s_current = *candidate;
        s_has_current = true;
        set_connect_result(WIFI_MANAGER_OPERATION_SUCCEEDED, ESP_OK);
        char ip[16];
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        snprintf(ip, sizeof(ip), "%s", s_status.ip);
        xSemaphoreGive(s_mutex);
        if (from_ap) {
            if (wait_for_stop(pdMS_TO_TICKS(WIFI_MANAGER_AP_HANDOFF_MS))) {
                return;
            }
            (void)stop_softap_keep_station();
        }
        set_status(WIFI_MANAGER_MODE_STA, candidate->ssid, ip);
        if (stop_requested()) {
            return;
        }
        s_config.connection_changed(true, s_config.context);
        return;
    }

    set_connect_result(WIFI_MANAGER_OPERATION_FAILED, err);
    if (from_ap) {
        (void)start_softap();
    } else if (had_previous &&
               (connect_credential(&previous, false,
                                   pdMS_TO_TICKS(
                                       WIFI_MANAGER_CONNECT_TIMEOUT_MS)) ==
                ESP_OK)) {
        if (stop_requested()) {
            return;
        }
        s_current = previous;
        s_has_current = true;
        s_config.connection_changed(true, s_config.context);
    } else if (try_saved_credentials(candidate->ssid) != ESP_OK) {
        (void)start_softap();
    }
}

static void manager_task(void *argument)
{
    (void)argument;
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events, WIFI_READY_BIT | WIFI_STOP_REQUEST_BIT, pdFALSE,
        pdFALSE, portMAX_DELAY);
    if ((bits & WIFI_STOP_REQUEST_BIT) != 0U) {
        goto stopped;
    }
    if (try_saved_credentials(NULL) != ESP_OK) {
        (void)start_softap();
    }

    while (!stop_requested()) {
        wifi_command_t command;
        if (xQueueReceive(s_commands, &command, pdMS_TO_TICKS(250)) == pdTRUE) {
            if (command.type == WIFI_COMMAND_SCAN) {
                handle_scan();
            } else if (command.type == WIFI_COMMAND_CONNECT) {
                handle_candidate(&command.credential);
            }
            continue;
        }
        if (stop_requested()) {
            break;
        }
        wifi_manager_status_t status;
        wifi_manager_get_status(&status);
        if ((status.mode == WIFI_MANAGER_MODE_STA) &&
            ((xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) == 0U)) {
            (void)recover_connection();
        }
    }

stopped:
    xEventGroupSetBits(s_wifi_events, WIFI_STOPPED_BIT);
    vTaskSuspend(NULL);
}

static esp_err_t initialize_wifi(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    s_netif_initialized = true;
    err = esp_event_loop_create_default();
    if (err != ESP_OK) {
        return err;
    }
    s_event_loop_created = true;
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_config);
    if (err == ESP_OK) {
        s_wifi_initialized = true;
        err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                         wifi_event_handler, NULL);
        if (err == ESP_OK) {
            s_wifi_handler_registered = true;
        }
    }
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                         wifi_event_handler, NULL);
        if (err == ESP_OK) {
            s_ip_handler_registered = true;
        }
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
        if (err == ESP_OK) {
            s_wifi_started = true;
        }
    }
    return err;
}

static void save_first_error(esp_err_t err, esp_err_t *result)
{
    if ((err != ESP_OK) && (*result == ESP_OK)) {
        *result = err;
    }
}

static esp_err_t deinitialize_wifi(void)
{
    esp_err_t result = ESP_OK;
    if (s_wifi_started) {
        save_first_error(esp_wifi_stop(), &result);
        s_wifi_started = false;
    }
    if (s_ip_handler_registered) {
        save_first_error(esp_event_handler_unregister(
                             IP_EVENT, IP_EVENT_STA_GOT_IP,
                             wifi_event_handler),
                         &result);
        s_ip_handler_registered = false;
    }
    if (s_wifi_handler_registered) {
        save_first_error(esp_event_handler_unregister(
                             WIFI_EVENT, ESP_EVENT_ANY_ID,
                             wifi_event_handler),
                         &result);
        s_wifi_handler_registered = false;
    }
    if (s_ap_netif != NULL) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
    if (s_sta_netif != NULL) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }
    if (s_wifi_initialized) {
        save_first_error(esp_wifi_deinit(), &result);
        s_wifi_initialized = false;
    }
    if (s_event_loop_created) {
        save_first_error(esp_event_loop_delete_default(), &result);
        s_event_loop_created = false;
    }
    if (s_netif_initialized) {
        save_first_error(esp_netif_deinit(), &result);
        s_netif_initialized = false;
    }
    return result;
}

static void cleanup_manager_objects(void)
{
    if (s_commands != NULL) {
        vQueueDelete(s_commands);
        s_commands = NULL;
    }
    if (s_wifi_events != NULL) {
        vEventGroupDelete(s_wifi_events);
        s_wifi_events = NULL;
    }
    if (s_mutex != NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    memset(&s_config, 0, sizeof(s_config));
    memset(&s_status, 0, sizeof(s_status));
    memset(&s_scan_result, 0, sizeof(s_scan_result));
    memset(&s_connect_result, 0, sizeof(s_connect_result));
    memset(s_credentials, 0, sizeof(s_credentials));
    memset(&s_current, 0, sizeof(s_current));
    s_credential_count = 0U;
    s_has_current = false;
    s_next_operation_id = 0U;
    s_disconnect_reason = 0U;
}

esp_err_t wifi_manager_start(const wifi_manager_config_t *config)
{
    if ((config == NULL) || (config->softap_password == NULL) ||
        (config->save_candidate == NULL) ||
        (config->connection_changed == NULL) ||
        (config->credential_count > WIFI_MANAGER_MAX_CREDENTIALS) ||
        ((config->credentials == NULL) && (config->credential_count != 0U))) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t password_length = strlen(config->softap_password);
    if ((password_length < 8U) || (password_length > 63U)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    s_mutex = xSemaphoreCreateMutex();
    s_wifi_events = xEventGroupCreate();
    s_commands = xQueueCreate(WIFI_MANAGER_COMMAND_QUEUE_SIZE,
                              sizeof(wifi_command_t));
    if ((s_mutex == NULL) || (s_wifi_events == NULL) ||
        (s_commands == NULL)) {
        cleanup_manager_objects();
        return ESP_ERR_NO_MEM;
    }
    s_config = *config;
    s_next_operation_id = 1U;
    s_status.mode = WIFI_MANAGER_MODE_STARTING;
    uint8_t mac[6];
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    if (err != ESP_OK) {
        goto failed;
    }
    snprintf(s_status.ap_ssid, sizeof(s_status.ap_ssid), "AmphiLink-%02X%02X%02X",
             mac[3], mac[4], mac[5]);
    err = wifi_manager_set_credentials(config->credentials,
                                       config->credential_count);
    if (err != ESP_OK) {
        goto failed;
    }
    s_config.credentials = NULL;
    s_config.credential_count = 0U;
    err = initialize_wifi();
    if (err != ESP_OK) {
        goto failed;
    }
    if (xTaskCreatePinnedToCore(manager_task, "WiFi-Manager", 8192, NULL,
                                configMAX_PRIORITIES - 4, &s_manager_task,
                                WIFI_MANAGER_TASK_CORE) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto failed;
    }
    s_started = true;
    return ESP_OK;

failed:
    {
        esp_err_t cleanup_err = deinitialize_wifi();
        if (cleanup_err != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi startup cleanup failed: %s",
                     esp_err_to_name(cleanup_err));
        }
    }
    cleanup_manager_objects();
    return err;
}

esp_err_t wifi_manager_stop(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    s_stopping = true;
    s_config.connection_changed(false, s_config.context);
    xEventGroupSetBits(s_wifi_events, WIFI_STOP_REQUEST_BIT |
                                          WIFI_READY_BIT |
                                          WIFI_DISCONNECTED_BIT);
    (void)esp_wifi_scan_stop();
    (void)esp_wifi_disconnect();

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events, WIFI_STOPPED_BIT, pdFALSE, pdTRUE,
        pdMS_TO_TICKS(WIFI_MANAGER_STOP_TIMEOUT_MS));
    esp_err_t result = ESP_OK;
    if ((bits & WIFI_STOPPED_BIT) == 0U) {
        result = ESP_ERR_TIMEOUT;
    }
    if (s_manager_task != NULL) {
        vTaskDelete(s_manager_task);
    }
    s_manager_task = NULL;
    s_config.connection_changed(false, s_config.context);

    esp_err_t cleanup_err = deinitialize_wifi();
    save_first_error(cleanup_err, &result);
    cleanup_manager_objects();
    s_started = false;
    s_stopping = false;
    return result;
}

esp_err_t wifi_manager_set_credentials(
    const wifi_manager_credential_t *credentials, size_t count)
{
    if ((count > WIFI_MANAGER_MAX_CREDENTIALS) ||
        ((credentials == NULL) && (count != 0U)) || (s_mutex == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t index = 0U; index < count; ++index) {
        size_t ssid_length = strnlen(credentials[index].ssid,
                                    WIFI_MANAGER_SSID_SIZE);
        size_t password_length = strnlen(credentials[index].password,
                                        WIFI_MANAGER_PASSWORD_SIZE);
        if ((ssid_length == 0U) || (ssid_length >= WIFI_MANAGER_SSID_SIZE) ||
            (password_length >= WIFI_MANAGER_PASSWORD_SIZE) ||
            ((password_length != 0U) && (password_length < 8U))) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_credential_count = count;
    memset(s_credentials, 0, sizeof(s_credentials));
    memcpy(s_credentials, credentials, count * sizeof(s_credentials[0]));
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t wifi_manager_get_status(wifi_manager_status_t *status)
{
    if ((status == NULL) || (s_mutex == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *status = s_status;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

static uint32_t allocate_operation_id(void)
{
    uint32_t id = s_next_operation_id++;
    if (s_next_operation_id == 0U) {
        s_next_operation_id = 1U;
    }
    return id;
}

esp_err_t wifi_manager_scan_start(uint32_t *operation_id)
{
    if ((operation_id == NULL) || !s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if ((s_scan_result.state == WIFI_MANAGER_OPERATION_RUNNING) ||
        (s_connect_result.state == WIFI_MANAGER_OPERATION_RUNNING)) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_scan_result, 0, sizeof(s_scan_result));
    s_scan_result.operation_id = allocate_operation_id();
    s_scan_result.state = WIFI_MANAGER_OPERATION_RUNNING;
    *operation_id = s_scan_result.operation_id;
    xSemaphoreGive(s_mutex);
    wifi_command_t command = {.type = WIFI_COMMAND_SCAN};
    if (xQueueSend(s_commands, &command, 0) != pdTRUE) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_scan_result.state = WIFI_MANAGER_OPERATION_FAILED;
        s_scan_result.error = ESP_ERR_TIMEOUT;
        xSemaphoreGive(s_mutex);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t wifi_manager_scan_get(uint32_t operation_id,
                                wifi_manager_scan_result_t *result)
{
    if ((result == NULL) || !s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (operation_id != s_scan_result.operation_id) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    *result = s_scan_result;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t wifi_manager_connect_start(
    const wifi_manager_credential_t *credential, uint32_t *operation_id)
{
    if ((credential == NULL) || (operation_id == NULL) || !s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t ssid_length = strnlen(credential->ssid, WIFI_MANAGER_SSID_SIZE);
    size_t password_length =
        strnlen(credential->password, WIFI_MANAGER_PASSWORD_SIZE);
    if ((ssid_length == 0U) || (ssid_length >= WIFI_MANAGER_SSID_SIZE) ||
        (password_length >= WIFI_MANAGER_PASSWORD_SIZE) ||
        ((password_length != 0U) && (password_length < 8U))) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if ((s_scan_result.state == WIFI_MANAGER_OPERATION_RUNNING) ||
        (s_connect_result.state == WIFI_MANAGER_OPERATION_RUNNING)) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_connect_result, 0, sizeof(s_connect_result));
    s_connect_result.operation_id = allocate_operation_id();
    s_connect_result.state = WIFI_MANAGER_OPERATION_RUNNING;
    snprintf(s_connect_result.ssid, sizeof(s_connect_result.ssid), "%s",
             credential->ssid);
    *operation_id = s_connect_result.operation_id;
    xSemaphoreGive(s_mutex);
    wifi_command_t command = {
        .type = WIFI_COMMAND_CONNECT,
        .credential = *credential,
    };
    if (xQueueSend(s_commands, &command, 0) != pdTRUE) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_connect_result.state = WIFI_MANAGER_OPERATION_FAILED;
        s_connect_result.error = ESP_ERR_TIMEOUT;
        xSemaphoreGive(s_mutex);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t wifi_manager_connect_get(uint32_t operation_id,
                                   wifi_manager_connect_result_t *result)
{
    if ((result == NULL) || !s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (operation_id != s_connect_result.operation_id) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    *result = s_connect_result;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}
