#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types.h"

#define WIFI_MANAGER_MAX_CREDENTIALS 10U
#define WIFI_MANAGER_MAX_SCAN_RESULTS 20U
#define WIFI_MANAGER_SSID_SIZE 33U
#define WIFI_MANAGER_PASSWORD_SIZE 64U

typedef struct {
    uint32_t id;
    char ssid[WIFI_MANAGER_SSID_SIZE];
    char password[WIFI_MANAGER_PASSWORD_SIZE];
} wifi_manager_credential_t;

typedef enum {
    WIFI_MANAGER_MODE_STARTING = 0,
    WIFI_MANAGER_MODE_CONNECTING,
    WIFI_MANAGER_MODE_STA,
    WIFI_MANAGER_MODE_AP,
} wifi_manager_mode_t;

typedef enum {
    WIFI_MANAGER_OPERATION_IDLE = 0,
    WIFI_MANAGER_OPERATION_RUNNING,
    WIFI_MANAGER_OPERATION_SUCCEEDED,
    WIFI_MANAGER_OPERATION_FAILED,
} wifi_manager_operation_state_t;

typedef struct {
    wifi_manager_mode_t mode;
    bool scanning;
    char ssid[WIFI_MANAGER_SSID_SIZE];
    char ip[16];
    char ap_ssid[WIFI_MANAGER_SSID_SIZE];
} wifi_manager_status_t;

typedef struct {
    char ssid[WIFI_MANAGER_SSID_SIZE];
    int8_t rssi;
    uint8_t channel;
    wifi_auth_mode_t authmode;
} wifi_manager_scan_record_t;

typedef struct {
    uint32_t operation_id;
    wifi_manager_operation_state_t state;
    esp_err_t error;
    size_t count;
    wifi_manager_scan_record_t records[WIFI_MANAGER_MAX_SCAN_RESULTS];
} wifi_manager_scan_result_t;

typedef struct {
    uint32_t operation_id;
    wifi_manager_operation_state_t state;
    esp_err_t error;
    char ssid[WIFI_MANAGER_SSID_SIZE];
} wifi_manager_connect_result_t;

typedef esp_err_t (*wifi_manager_save_callback_t)(
    const wifi_manager_credential_t *credential, void *context);
typedef void (*wifi_manager_connection_callback_t)(bool connected,
                                                    void *context);

typedef struct {
    const wifi_manager_credential_t *credentials;
    size_t credential_count;
    const char *softap_password;
    wifi_manager_save_callback_t save_candidate;
    wifi_manager_connection_callback_t connection_changed;
    void *context;
} wifi_manager_config_t;

esp_err_t wifi_manager_start(const wifi_manager_config_t *config);
esp_err_t wifi_manager_stop(void);
esp_err_t wifi_manager_set_credentials(
    const wifi_manager_credential_t *credentials, size_t count);
esp_err_t wifi_manager_get_status(wifi_manager_status_t *status);

esp_err_t wifi_manager_scan_start(uint32_t *operation_id);
esp_err_t wifi_manager_scan_get(uint32_t operation_id,
                                wifi_manager_scan_result_t *result);
esp_err_t wifi_manager_connect_start(
    const wifi_manager_credential_t *credential, uint32_t *operation_id);
esp_err_t wifi_manager_connect_get(uint32_t operation_id,
                                   wifi_manager_connect_result_t *result);

#endif
