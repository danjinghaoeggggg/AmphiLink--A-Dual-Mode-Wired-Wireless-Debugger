#ifndef PROVISIONING_SERVER_H
#define PROVISIONING_SERVER_H

#include <stdbool.h>

#include "device_identity_store.h"
#include "esp_err.h"

typedef void (*provisioning_identity_changed_callback_t)(
    const device_identity_t *identity, void *context);
typedef bool (*provisioning_client_status_callback_t)(void *context);
typedef void (*provisioning_disconnect_callback_t)(void *context);

typedef struct {
    provisioning_identity_changed_callback_t identity_changed;
    provisioning_client_status_callback_t dap_client_active;
    provisioning_client_status_callback_t serial_client_active;
    provisioning_disconnect_callback_t disconnect_clients;
    void *context;
} provisioning_server_config_t;

esp_err_t provisioning_server_start(const provisioning_server_config_t *config);
esp_err_t provisioning_server_stop(void);

#endif
