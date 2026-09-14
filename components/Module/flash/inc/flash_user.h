#ifndef FLASH_USER_H
#define FLASH_USER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define FLASH_USER_WEBPAGE_MAX_RELATIVE_PATH 115U

#ifndef FLASH_USER_FORCE_FORMAT_ON_INIT_ERROR
#define FLASH_USER_FORCE_FORMAT_ON_INIT_ERROR 1
#endif

#if (FLASH_USER_FORCE_FORMAT_ON_INIT_ERROR != 0) &&                            \
    (FLASH_USER_FORCE_FORMAT_ON_INIT_ERROR != 1)
#error "FLASH_USER_FORCE_FORMAT_ON_INIT_ERROR must be 0 or 1"
#endif

/* Initializes the default NVS partition and mounts webpage_vfs read-only. */
esp_err_t flash_user_init(void);

esp_err_t flash_user_webpage_get_size(const char *path, size_t *size);
esp_err_t flash_user_webpage_read_at(const char *path, size_t offset,
                                     void *data, size_t size,
                                     size_t *bytes_read);

esp_err_t flash_user_nvs_set_blob(const char *namespace_name, const char *key,
                                  const void *data, size_t size);
/* Pass data=NULL and size=0 to query the required blob length. */
esp_err_t flash_user_nvs_get_blob(const char *namespace_name, const char *key,
                                  void *data, size_t size, size_t *bytes_read);
esp_err_t flash_user_nvs_erase_key(const char *namespace_name,
                                   const char *key);

esp_err_t flash_user_nvs_partition_init(const char *partition_label,
                                        bool erase_on_init_error);
esp_err_t flash_user_nvs_partition_erase(const char *partition_label);
esp_err_t flash_user_nvs_partition_set_blob(const char *partition_label,
                                            const char *namespace_name,
                                            const char *key, const void *data,
                                            size_t size);
esp_err_t flash_user_nvs_partition_get_blob(const char *partition_label,
                                            const char *namespace_name,
                                            const char *key, void *data,
                                            size_t size, size_t *bytes_read);
esp_err_t flash_user_nvs_partition_get_string(const char *partition_label,
                                              const char *namespace_name,
                                              const char *key, char *value,
                                              size_t size,
                                              size_t *bytes_read);
esp_err_t flash_user_nvs_partition_get_u8(const char *partition_label,
                                          const char *namespace_name,
                                          const char *key, uint8_t *value);

#endif
