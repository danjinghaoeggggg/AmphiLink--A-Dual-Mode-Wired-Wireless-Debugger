#ifndef DEVICE_IDENTITY_STORE_H
#define DEVICE_IDENTITY_STORE_H

#include <stdint.h>

#include "esp_err.h"

#define DEVICE_IDENTITY_NAME_SIZE 33U
#define DEVICE_IDENTITY_ID_MIN 1U
#define DEVICE_IDENTITY_ID_MAX 10U
#define DEVICE_IDENTITY_DEFAULT_NAME "AmphiLink"
#define DEVICE_IDENTITY_DEFAULT_ID 1U

typedef struct {
    char name[DEVICE_IDENTITY_NAME_SIZE];
    uint8_t id;
} device_identity_t;

esp_err_t device_identity_validate(const char *name, uint32_t id);
esp_err_t device_identity_store_init(void);
esp_err_t device_identity_store_get(device_identity_t *identity);
esp_err_t device_identity_store_set(const char *name, uint32_t id,
                                    device_identity_t *identity);

#endif
