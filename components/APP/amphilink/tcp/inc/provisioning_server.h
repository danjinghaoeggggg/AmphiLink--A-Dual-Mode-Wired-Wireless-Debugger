#ifndef PROVISIONING_SERVER_H
#define PROVISIONING_SERVER_H

#include "device_identity_store.h"
#include "esp_err.h"

typedef void (*provisioning_identity_changed_callback_t)(
    const device_identity_t *identity, void *context);

typedef struct {
    provisioning_identity_changed_callback_t identity_changed;
    void *context;
} provisioning_server_config_t;

esp_err_t provisioning_server_start(const provisioning_server_config_t *config);
esp_err_t provisioning_server_stop(void);

#endif
