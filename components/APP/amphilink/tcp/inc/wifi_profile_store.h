#ifndef WIFI_PROFILE_STORE_H
#define WIFI_PROFILE_STORE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define WIFI_PROFILE_MAX_COUNT 10U
#define WIFI_PROFILE_SSID_SIZE 33U
#define WIFI_PROFILE_PASSWORD_SIZE 64U

typedef struct {
    uint32_t id;
    char ssid[WIFI_PROFILE_SSID_SIZE];
    char password[WIFI_PROFILE_PASSWORD_SIZE];
} wifi_profile_t;

typedef struct {
    uint32_t revision;
    size_t count;
    bool locked;
    wifi_profile_t profiles[WIFI_PROFILE_MAX_COUNT];
} wifi_profile_snapshot_t;

esp_err_t wifi_profile_store_init(const char *admin_password);
esp_err_t wifi_profile_store_get(wifi_profile_snapshot_t *snapshot);
esp_err_t wifi_profile_store_get_password(uint32_t id, char *password,
                                          size_t size);
esp_err_t wifi_profile_store_reset(wifi_profile_snapshot_t *snapshot);

esp_err_t wifi_profile_store_add(uint32_t expected_revision,
                                 const char *ssid, const char *password,
                                 wifi_profile_snapshot_t *snapshot);
esp_err_t wifi_profile_store_update(uint32_t expected_revision, uint32_t id,
                                    const char *ssid, const char *password,
                                    wifi_profile_snapshot_t *snapshot);
esp_err_t wifi_profile_store_remove(uint32_t expected_revision, uint32_t id,
                                    wifi_profile_snapshot_t *snapshot);
esp_err_t wifi_profile_store_reorder(uint32_t expected_revision,
                                     const uint32_t *ids, size_t count,
                                     wifi_profile_snapshot_t *snapshot);
esp_err_t wifi_profile_store_upsert_and_promote(
    const char *ssid, const char *password,
    wifi_profile_snapshot_t *snapshot);

esp_err_t wifi_profile_validate(const char *ssid, const char *password);

#endif
