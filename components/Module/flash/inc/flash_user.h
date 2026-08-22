#ifndef FLASH_USER_H
#define FLASH_USER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define FLASH_USER_VFS_MAX_RELATIVE_PATH 115U
#define FLASH_USER_WEBPAGE_VFS_MAX_RELATIVE_PATH 115U
#define FLASH_USER_STORAGE_MAX_RELATIVE_PATH 26U

#ifndef FLASH_USER_FORCE_FORMAT_ON_INIT_ERROR
#define FLASH_USER_FORCE_FORMAT_ON_INIT_ERROR 1
#endif

#if (FLASH_USER_FORCE_FORMAT_ON_INIT_ERROR != 0) &&                            \
    (FLASH_USER_FORCE_FORMAT_ON_INIT_ERROR != 1)
#error "FLASH_USER_FORCE_FORMAT_ON_INIT_ERROR must be 0 or 1"
#endif

typedef enum {
    FLASH_USER_PARTITION_NVS = 0,
    FLASH_USER_PARTITION_VFS,
    FLASH_USER_PARTITION_STORAGE,
    FLASH_USER_PARTITION_WEBPAGE_VFS,
    FLASH_USER_PARTITION_COUNT
} flash_user_partition_t;

typedef struct {
    bool mounted;
    /* NVS values describe 32-byte entry storage, not raw partition capacity. */
    size_t total_bytes;
    size_t used_bytes;
} flash_user_info_t;

/*
 * Initializes NVS and mounts the writable FAT, read-only webpage FAT, and
 * SPIFFS partitions. When FLASH_USER_FORCE_FORMAT_ON_INIT_ERROR is 1, failed
 * initialization erases or formats affected writable partitions once and
 * retries. The webpage partition is never formatted at runtime.
 */
esp_err_t flash_user_init(void);

esp_err_t flash_user_get_info(flash_user_partition_t partition,
                              flash_user_info_t *info);

/* NVS cannot be formatted through this API. */
esp_err_t flash_user_format(flash_user_partition_t partition);

/*
 * path is relative to the partition mount point, for example "data.bin".
 * VFS paths are limited to FLASH_USER_VFS_MAX_RELATIVE_PATH characters;
 * webpage paths are limited to FLASH_USER_WEBPAGE_VFS_MAX_RELATIVE_PATH;
 * storage paths are limited to FLASH_USER_STORAGE_MAX_RELATIVE_PATH.
 * Names ending in ".tmp" or ".bak" are reserved for transactional writes.
 * The webpage partition supports read operations only.
 */
esp_err_t flash_user_file_write(flash_user_partition_t partition,
                                const char *path, const void *data,
                                size_t size);
esp_err_t flash_user_file_read(flash_user_partition_t partition,
                               const char *path, void *data, size_t size,
                               size_t *bytes_read);
esp_err_t flash_user_file_read_at(flash_user_partition_t partition,
                                  const char *path, size_t offset, void *data,
                                  size_t size, size_t *bytes_read);
esp_err_t flash_user_file_delete(flash_user_partition_t partition,
                                 const char *path);
bool flash_user_file_exists(flash_user_partition_t partition, const char *path);
esp_err_t flash_user_file_get_size(flash_user_partition_t partition,
                                   const char *path, size_t *size);

esp_err_t flash_user_nvs_set_blob(const char *namespace_name, const char *key,
                                  const void *data, size_t size);
/* Pass data=NULL and size=0 to query the required blob length. */
esp_err_t flash_user_nvs_get_blob(const char *namespace_name, const char *key,
                                  void *data, size_t size, size_t *bytes_read);
esp_err_t flash_user_nvs_set_string(const char *namespace_name, const char *key,
                                    const char *value);
/* Pass value=NULL and size=0 to query the required string length. */
esp_err_t flash_user_nvs_get_string(const char *namespace_name, const char *key,
                                    char *value, size_t size,
                                    size_t *bytes_read);

esp_err_t flash_user_nvs_partition_init(const char *partition_label,
                                        bool erase_on_init_error);
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
