#include "flash_user.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Layering exception: this Flash Module is the owner of persistent storage.
 * It calls the BSP flash primitives directly so partition locking, erase
 * alignment, and filesystem/NVS transaction handling stay in one module.
 */
#include "bsp_flash.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "wear_levelling.h"

#define FLASH_USER_NVS_PARTITION_LABEL "nvs"
#define FLASH_USER_VFS_PARTITION_LABEL "vfs"
#define FLASH_USER_WEBPAGE_VFS_PARTITION_LABEL "webpage_vfs"
#define FLASH_USER_STORAGE_PARTITION_LABEL "storage"

#define FLASH_USER_VFS_MOUNT_PATH "/vfs"
#define FLASH_USER_WEBPAGE_VFS_MOUNT_PATH "/webpage"
#define FLASH_USER_STORAGE_MOUNT_PATH "/storage"

#define FLASH_USER_FS_MAX_OPEN_FILES 8U
#define FLASH_USER_NVS_ENTRY_SIZE 32U
#define FLASH_USER_ERASE_CHECK_BUFFER_SIZE 4096U
#define FLASH_USER_PATH_BUFFER_SIZE 160U
#define FLASH_USER_TEMP_SUFFIX ".tmp"
#define FLASH_USER_BACKUP_SUFFIX ".bak"
#define FLASH_USER_TRANSACTION_SUFFIX_SIZE 4U

#if CONFIG_SPIFFS_OBJ_NAME_LEN < (FLASH_USER_STORAGE_MAX_RELATIVE_PATH +       \
                                  FLASH_USER_TRANSACTION_SUFFIX_SIZE + 2U)
#error "CONFIG_SPIFFS_OBJ_NAME_LEN is too small for the public path limit"
#endif

static const char *TAG = "FLASH_USER";

static bool s_nvs_ready;
static bool s_vfs_mounted;
static bool s_webpage_vfs_mounted;
static bool s_storage_mounted;
static wl_handle_t s_vfs_handle = WL_INVALID_HANDLE;
static SemaphoreHandle_t s_flash_mutex;
static uint8_t s_erase_check_buffer[FLASH_USER_ERASE_CHECK_BUFFER_SIZE];

static esp_err_t flash_user_lock(void)
{
    if (s_flash_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return (xSemaphoreTake(s_flash_mutex, portMAX_DELAY) == pdTRUE) ? ESP_OK
                                                                    : ESP_FAIL;
}

static void flash_user_unlock(void)
{
    xSemaphoreGive(s_flash_mutex);
}

static esp_err_t flash_user_errno_to_err(int error_number)
{
    switch (error_number) {
    case ENOENT:
        return ESP_ERR_NOT_FOUND;
    case EINVAL:
        return ESP_ERR_INVALID_ARG;
    case ENAMETOOLONG:
        return ESP_ERR_INVALID_SIZE;
    case ENOMEM:
    case ENOSPC:
        return ESP_ERR_NO_MEM;
    default:
        return ESP_FAIL;
    }
}

static const char *flash_user_get_mount_path(flash_user_partition_t partition)
{
    switch (partition) {
    case FLASH_USER_PARTITION_VFS:
        return FLASH_USER_VFS_MOUNT_PATH;
    case FLASH_USER_PARTITION_WEBPAGE_VFS:
        return FLASH_USER_WEBPAGE_VFS_MOUNT_PATH;
    case FLASH_USER_PARTITION_STORAGE:
        return FLASH_USER_STORAGE_MOUNT_PATH;
    default:
        return NULL;
    }
}

static bool flash_user_has_parent_segment(const char *path)
{
    const char *segment = path;
    while (*segment != '\0') {
        const char *separator = strchr(segment, '/');
        size_t segment_length = (separator == NULL)
                                    ? strlen(segment)
                                    : (size_t)(separator - segment);
        if ((segment_length == 2U) && (segment[0] == '.') &&
            (segment[1] == '.')) {
            return true;
        }
        if (separator == NULL) {
            break;
        }
        segment = separator + 1;
    }
    return false;
}

static bool flash_user_has_reserved_suffix(const char *path)
{
    size_t path_length = strlen(path);
    if (path_length < FLASH_USER_TRANSACTION_SUFFIX_SIZE) {
        return false;
    }
    const char *suffix =
        path + path_length - FLASH_USER_TRANSACTION_SUFFIX_SIZE;
    return (strcmp(suffix, FLASH_USER_TEMP_SUFFIX) == 0) ||
           (strcmp(suffix, FLASH_USER_BACKUP_SUFFIX) == 0);
}

static esp_err_t flash_user_validate_path(flash_user_partition_t partition,
                                          const char *path,
                                          size_t reserved_suffix_size)
{
    if ((path == NULL) || (path[0] == '\0') || (path[0] == '/') ||
        flash_user_has_parent_segment(path) ||
        flash_user_has_reserved_suffix(path)) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t maximum_length;
    switch (partition) {
    case FLASH_USER_PARTITION_VFS:
        maximum_length = FLASH_USER_VFS_MAX_RELATIVE_PATH +
                         FLASH_USER_TRANSACTION_SUFFIX_SIZE;
        break;
    case FLASH_USER_PARTITION_WEBPAGE_VFS:
        maximum_length = FLASH_USER_WEBPAGE_VFS_MAX_RELATIVE_PATH +
                         FLASH_USER_TRANSACTION_SUFFIX_SIZE;
        break;
    case FLASH_USER_PARTITION_STORAGE:
        maximum_length = FLASH_USER_STORAGE_MAX_RELATIVE_PATH +
                         FLASH_USER_TRANSACTION_SUFFIX_SIZE;
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }

    size_t path_length = strlen(path);
    if ((reserved_suffix_size > maximum_length) ||
        (path_length > (maximum_length - reserved_suffix_size))) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t flash_user_build_full_path(flash_user_partition_t partition,
                                            const char *path,
                                            size_t reserved_suffix_size,
                                            char *full_path,
                                            size_t full_path_size)
{
    const char *mount_path = flash_user_get_mount_path(partition);
    if (mount_path == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (((partition == FLASH_USER_PARTITION_VFS) && !s_vfs_mounted) ||
        ((partition == FLASH_USER_PARTITION_WEBPAGE_VFS) &&
         !s_webpage_vfs_mounted) ||
        ((partition == FLASH_USER_PARTITION_STORAGE) && !s_storage_mounted)) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err =
        flash_user_validate_path(partition, path, reserved_suffix_size);
    if (err != ESP_OK) {
        return err;
    }

    int written =
        snprintf(full_path, full_path_size, "%s/%s", mount_path, path);
    if ((written < 0) || ((size_t)written >= full_path_size)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t flash_user_append_suffix(const char *path, const char *suffix,
                                          char *output, size_t output_size)
{
    int written = snprintf(output, output_size, "%s%s", path, suffix);
    return ((written < 0) || ((size_t)written >= output_size))
               ? ESP_ERR_INVALID_SIZE
               : ESP_OK;
}

static esp_err_t flash_user_stat_path(const char *path, bool *exists)
{
    struct stat status;
    if (stat(path, &status) == 0) {
        *exists = true;
        return ESP_OK;
    }
    if (errno == ENOENT) {
        *exists = false;
        return ESP_OK;
    }
    return flash_user_errno_to_err(errno);
}

static esp_err_t flash_user_remove_if_exists(const char *path)
{
    if ((remove(path) == 0) || (errno == ENOENT)) {
        return ESP_OK;
    }
    return flash_user_errno_to_err(errno);
}

static esp_err_t flash_user_recover_file(const char *path)
{
    char temporary_path[FLASH_USER_PATH_BUFFER_SIZE];
    char backup_path[FLASH_USER_PATH_BUFFER_SIZE];
    esp_err_t err = flash_user_append_suffix(
        path, FLASH_USER_TEMP_SUFFIX, temporary_path, sizeof(temporary_path));
    if (err != ESP_OK) {
        return err;
    }
    err = flash_user_append_suffix(path, FLASH_USER_BACKUP_SUFFIX, backup_path,
                                   sizeof(backup_path));
    if (err != ESP_OK) {
        return err;
    }

    bool target_exists;
    bool backup_exists;
    err = flash_user_stat_path(path, &target_exists);
    if (err != ESP_OK) {
        return err;
    }
    err = flash_user_stat_path(backup_path, &backup_exists);
    if (err != ESP_OK) {
        return err;
    }

    if (!target_exists && backup_exists) {
        if (rename(backup_path, path) != 0) {
            return flash_user_errno_to_err(errno);
        }
    } else if (target_exists && backup_exists) {
        err = flash_user_remove_if_exists(backup_path);
        if (err != ESP_OK) {
            return err;
        }
    }
    return flash_user_remove_if_exists(temporary_path);
}

static esp_err_t
flash_user_recover_file_if_writable(flash_user_partition_t partition,
                                    const char *path)
{
    return (partition == FLASH_USER_PARTITION_WEBPAGE_VFS)
               ? ESP_OK
               : flash_user_recover_file(path);
}

static esp_err_t flash_user_partition_is_erased(const char *partition_label,
                                                bool *is_erased)
{
    if ((partition_label == NULL) || (is_erased == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    bsp_flash_partition_t partition =
        bsp_flash_find_data_partition(partition_label);
    if (partition == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    size_t partition_size = bsp_flash_get_partition_size(partition);
    *is_erased = true;
    for (size_t offset = 0U; offset < partition_size;) {
        size_t remaining = partition_size - offset;
        size_t read_size = (remaining < sizeof(s_erase_check_buffer))
                               ? remaining
                               : sizeof(s_erase_check_buffer);
        esp_err_t err =
            bsp_flash_read(partition, offset, s_erase_check_buffer, read_size);
        if (err != ESP_OK) {
            return err;
        }
        for (size_t index = 0U; index < read_size; ++index) {
            if (s_erase_check_buffer[index] != 0xFFU) {
                *is_erased = false;
                return ESP_OK;
            }
        }
        offset += read_size;
    }
    return ESP_OK;
}

static esp_err_t flash_user_mount_fat(void)
{
    bool partition_is_erased;
    esp_err_t err = flash_user_partition_is_erased(
        FLASH_USER_VFS_PARTITION_LABEL, &partition_is_erased);
    if (err != ESP_OK) {
        return err;
    }

    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = FLASH_USER_FS_MAX_OPEN_FILES,
        .allocation_unit_size = 0,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };
    err = esp_vfs_fat_spiflash_mount_rw_wl(FLASH_USER_VFS_MOUNT_PATH,
                                           FLASH_USER_VFS_PARTITION_LABEL,
                                           &mount_config, &s_vfs_handle);
    if ((err == ESP_OK) ||
        (!partition_is_erased && !FLASH_USER_FORCE_FORMAT_ON_INIT_ERROR)) {
        return err;
    }

    ESP_LOGW(TAG, "%s FAT partition after mount error: %s",
             partition_is_erased ? "Initializing erased" : "Formatting",
             esp_err_to_name(err));
    err = esp_vfs_fat_spiflash_format_rw_wl(FLASH_USER_VFS_MOUNT_PATH,
                                            FLASH_USER_VFS_PARTITION_LABEL);
    if (err != ESP_OK) {
        return err;
    }
    return esp_vfs_fat_spiflash_mount_rw_wl(FLASH_USER_VFS_MOUNT_PATH,
                                            FLASH_USER_VFS_PARTITION_LABEL,
                                            &mount_config, &s_vfs_handle);
}

static esp_err_t flash_user_mount_webpage_fat(void)
{
    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = FLASH_USER_FS_MAX_OPEN_FILES,
        .allocation_unit_size = 0,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };
    return esp_vfs_fat_spiflash_mount_ro(
        FLASH_USER_WEBPAGE_VFS_MOUNT_PATH,
        FLASH_USER_WEBPAGE_VFS_PARTITION_LABEL, &mount_config);
}

static esp_err_t flash_user_mount_spiffs(void)
{
    bool partition_is_erased;
    esp_err_t err = flash_user_partition_is_erased(
        FLASH_USER_STORAGE_PARTITION_LABEL, &partition_is_erased);
    if (err != ESP_OK) {
        return err;
    }

    const esp_vfs_spiffs_conf_t mount_config = {
        .base_path = FLASH_USER_STORAGE_MOUNT_PATH,
        .partition_label = FLASH_USER_STORAGE_PARTITION_LABEL,
        .max_files = FLASH_USER_FS_MAX_OPEN_FILES,
        .format_if_mount_failed = false,
    };
    err = esp_vfs_spiffs_register(&mount_config);
    if ((err == ESP_OK) ||
        (!partition_is_erased && !FLASH_USER_FORCE_FORMAT_ON_INIT_ERROR)) {
        return err;
    }

    ESP_LOGW(TAG, "%s SPIFFS partition after mount error: %s",
             partition_is_erased ? "Initializing erased" : "Formatting",
             esp_err_to_name(err));
    err = esp_spiffs_format(FLASH_USER_STORAGE_PARTITION_LABEL);
    if (err != ESP_OK) {
        return err;
    }
    return esp_vfs_spiffs_register(&mount_config);
}

esp_err_t flash_user_init(void)
{
    if (s_flash_mutex == NULL) {
        s_flash_mutex = xSemaphoreCreateMutex();
        if (s_flash_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t lock_err = flash_user_lock();
    if (lock_err != ESP_OK) {
        return lock_err;
    }
    if (s_nvs_ready && s_vfs_mounted && s_webpage_vfs_mounted &&
        s_storage_mounted) {
        flash_user_unlock();
        return ESP_OK;
    }

    esp_err_t first_error = ESP_OK;
    if (!s_nvs_ready) {
        esp_err_t err = nvs_flash_init();
#if FLASH_USER_FORCE_FORMAT_ON_INIT_ERROR
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "NVS init failed (%s); erasing and retrying",
                     esp_err_to_name(err));
            err = nvs_flash_erase();
            if (err == ESP_OK) {
                err = nvs_flash_init();
            }
        }
#endif
        if (err == ESP_OK) {
            s_nvs_ready = true;
            ESP_LOGI(TAG, "NVS partition initialized");
        } else {
            ESP_LOGE(TAG, "NVS initialization failed: %s",
                     esp_err_to_name(err));
            first_error = err;
        }
    }

    if (!s_vfs_mounted) {
        esp_err_t err = flash_user_mount_fat();
        if (err == ESP_OK) {
            s_vfs_mounted = true;
            ESP_LOGI(TAG, "FAT partition mounted at %s",
                     FLASH_USER_VFS_MOUNT_PATH);
        } else {
            ESP_LOGE(TAG, "FAT initialization failed: %s",
                     esp_err_to_name(err));
            if (first_error == ESP_OK) {
                first_error = err;
            }
        }
    }

    if (!s_webpage_vfs_mounted) {
        esp_err_t err = flash_user_mount_webpage_fat();
        if (err == ESP_OK) {
            s_webpage_vfs_mounted = true;
            ESP_LOGI(TAG, "Webpage FAT partition mounted at %s",
                     FLASH_USER_WEBPAGE_VFS_MOUNT_PATH);
        } else {
            ESP_LOGE(TAG, "Webpage FAT initialization failed: %s",
                     esp_err_to_name(err));
            if (first_error == ESP_OK) {
                first_error = err;
            }
        }
    }

    if (!s_storage_mounted) {
        esp_err_t err = flash_user_mount_spiffs();
        if (err == ESP_OK) {
            s_storage_mounted = true;
            ESP_LOGI(TAG, "SPIFFS partition mounted at %s",
                     FLASH_USER_STORAGE_MOUNT_PATH);
        } else {
            ESP_LOGE(TAG, "SPIFFS initialization failed: %s",
                     esp_err_to_name(err));
            if (first_error == ESP_OK) {
                first_error = err;
            }
        }
    }

    flash_user_unlock();
    return first_error;
}

esp_err_t flash_user_get_info(flash_user_partition_t partition,
                              flash_user_info_t *info)
{
    if ((partition >= FLASH_USER_PARTITION_COUNT) || (info == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(info, 0, sizeof(*info));

    esp_err_t err = flash_user_lock();
    if (err != ESP_OK) {
        return err;
    }

    if (partition == FLASH_USER_PARTITION_NVS) {
        if (!s_nvs_ready) {
            err = ESP_ERR_INVALID_STATE;
        } else {
            nvs_stats_t stats;
            err = nvs_get_stats(FLASH_USER_NVS_PARTITION_LABEL, &stats);
            if (err == ESP_OK) {
                info->mounted = true;
                info->total_bytes =
                    stats.total_entries * FLASH_USER_NVS_ENTRY_SIZE;
                info->used_bytes =
                    stats.used_entries * FLASH_USER_NVS_ENTRY_SIZE;
            }
        }
    } else if (partition == FLASH_USER_PARTITION_VFS) {
        if (!s_vfs_mounted) {
            err = ESP_ERR_INVALID_STATE;
        } else {
            uint64_t total_bytes = 0U;
            uint64_t free_bytes = 0U;
            err = esp_vfs_fat_info(FLASH_USER_VFS_MOUNT_PATH, &total_bytes,
                                   &free_bytes);
            if (err == ESP_OK) {
                info->mounted = true;
                info->total_bytes = (size_t)total_bytes;
                info->used_bytes = (size_t)(total_bytes - free_bytes);
            }
        }
    } else if (partition == FLASH_USER_PARTITION_WEBPAGE_VFS) {
        if (!s_webpage_vfs_mounted) {
            err = ESP_ERR_INVALID_STATE;
        } else {
            uint64_t total_bytes = 0U;
            uint64_t free_bytes = 0U;
            err = esp_vfs_fat_info(FLASH_USER_WEBPAGE_VFS_MOUNT_PATH,
                                   &total_bytes, &free_bytes);
            if (err == ESP_OK) {
                info->mounted = true;
                info->total_bytes = (size_t)total_bytes;
                info->used_bytes = (size_t)(total_bytes - free_bytes);
            }
        }
    } else if (partition == FLASH_USER_PARTITION_STORAGE) {
        if (!s_storage_mounted) {
            err = ESP_ERR_INVALID_STATE;
        } else {
            err = esp_spiffs_info(FLASH_USER_STORAGE_PARTITION_LABEL,
                                  &info->total_bytes, &info->used_bytes);
            if (err == ESP_OK) {
                info->mounted = true;
            }
        }
    } else {
        err = ESP_ERR_NOT_SUPPORTED;
    }

    flash_user_unlock();
    return err;
}

esp_err_t flash_user_format(flash_user_partition_t partition)
{
    esp_err_t err = flash_user_lock();
    if (err != ESP_OK) {
        return err;
    }

    switch (partition) {
    case FLASH_USER_PARTITION_VFS:
        if (s_vfs_mounted) {
            err = esp_vfs_fat_spiflash_unmount_rw_wl(FLASH_USER_VFS_MOUNT_PATH,
                                                     s_vfs_handle);
            if (err != ESP_OK) {
                break;
            }
            s_vfs_mounted = false;
            s_vfs_handle = WL_INVALID_HANDLE;
        }
        err = esp_vfs_fat_spiflash_format_rw_wl(FLASH_USER_VFS_MOUNT_PATH,
                                                FLASH_USER_VFS_PARTITION_LABEL);
        if (err == ESP_OK) {
            err = flash_user_mount_fat();
            s_vfs_mounted = (err == ESP_OK);
        }
        break;
    case FLASH_USER_PARTITION_STORAGE:
        if (s_storage_mounted) {
            err = esp_vfs_spiffs_unregister(FLASH_USER_STORAGE_PARTITION_LABEL);
            if (err != ESP_OK) {
                break;
            }
            s_storage_mounted = false;
        }
        err = esp_spiffs_format(FLASH_USER_STORAGE_PARTITION_LABEL);
        if (err == ESP_OK) {
            err = flash_user_mount_spiffs();
            s_storage_mounted = (err == ESP_OK);
        }
        break;
    case FLASH_USER_PARTITION_NVS:
    case FLASH_USER_PARTITION_WEBPAGE_VFS:
        err = ESP_ERR_NOT_SUPPORTED;
        break;
    default:
        err = ESP_ERR_INVALID_ARG;
        break;
    }

    flash_user_unlock();
    return err;
}

esp_err_t flash_user_file_write(flash_user_partition_t partition,
                                const char *path, const void *data, size_t size)
{
    if (partition == FLASH_USER_PARTITION_WEBPAGE_VFS) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if ((data == NULL) && (size != 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = flash_user_lock();
    if (err != ESP_OK) {
        return err;
    }

    char full_path[FLASH_USER_PATH_BUFFER_SIZE];
    char temporary_path[FLASH_USER_PATH_BUFFER_SIZE];
    char backup_path[FLASH_USER_PATH_BUFFER_SIZE];
    err = flash_user_build_full_path(partition, path,
                                     FLASH_USER_TRANSACTION_SUFFIX_SIZE,
                                     full_path, sizeof(full_path));
    if (err == ESP_OK) {
        err = flash_user_append_suffix(full_path, FLASH_USER_TEMP_SUFFIX,
                                       temporary_path, sizeof(temporary_path));
    }
    if (err == ESP_OK) {
        err = flash_user_append_suffix(full_path, FLASH_USER_BACKUP_SUFFIX,
                                       backup_path, sizeof(backup_path));
    }
    if (err == ESP_OK) {
        err = flash_user_recover_file_if_writable(partition, full_path);
    }
    if (err != ESP_OK) {
        flash_user_unlock();
        return err;
    }

    FILE *file = fopen(temporary_path, "wb");
    if (file == NULL) {
        err = flash_user_errno_to_err(errno);
    } else {
        size_t written = (size == 0U) ? 0U : fwrite(data, 1U, size, file);
        if ((written != size) || (fflush(file) != 0) ||
            (fsync(fileno(file)) != 0)) {
            err = flash_user_errno_to_err(errno);
        }
        if ((fclose(file) != 0) && (err == ESP_OK)) {
            err = flash_user_errno_to_err(errno);
        }
    }

    if (err == ESP_OK) {
        bool target_exists;
        err = flash_user_stat_path(full_path, &target_exists);
        if ((err == ESP_OK) && target_exists &&
            (rename(full_path, backup_path) != 0)) {
            err = flash_user_errno_to_err(errno);
        }
        if ((err == ESP_OK) && (rename(temporary_path, full_path) != 0)) {
            err = flash_user_errno_to_err(errno);
            if (target_exists) {
                (void)rename(backup_path, full_path);
            }
        }
        if ((err == ESP_OK) && target_exists) {
            err = flash_user_remove_if_exists(backup_path);
        }
    }
    if (err != ESP_OK) {
        (void)flash_user_remove_if_exists(temporary_path);
    }

    flash_user_unlock();
    return err;
}

esp_err_t flash_user_file_read(flash_user_partition_t partition,
                               const char *path, void *data, size_t size,
                               size_t *bytes_read)
{
    if (bytes_read != NULL) {
        *bytes_read = 0U;
    }
    if ((data == NULL) && (size != 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = flash_user_lock();
    if (err != ESP_OK) {
        return err;
    }

    char full_path[FLASH_USER_PATH_BUFFER_SIZE];
    err = flash_user_build_full_path(partition, path,
                                     FLASH_USER_TRANSACTION_SUFFIX_SIZE,
                                     full_path, sizeof(full_path));
    if (err == ESP_OK) {
        err = flash_user_recover_file_if_writable(partition, full_path);
    }
    if (err != ESP_OK) {
        flash_user_unlock();
        return err;
    }

    FILE *file = fopen(full_path, "rb");
    if (file == NULL) {
        err = flash_user_errno_to_err(errno);
    } else {
        size_t read_count = (size == 0U) ? 0U : fread(data, 1U, size, file);
        if (ferror(file) != 0) {
            err = ESP_FAIL;
        }
        if ((fclose(file) != 0) && (err == ESP_OK)) {
            err = flash_user_errno_to_err(errno);
        }
        if ((err == ESP_OK) && (bytes_read != NULL)) {
            *bytes_read = read_count;
        }
    }

    flash_user_unlock();
    return err;
}

esp_err_t flash_user_file_read_at(flash_user_partition_t partition,
                                  const char *path, size_t offset, void *data,
                                  size_t size, size_t *bytes_read)
{
    if (bytes_read != NULL) {
        *bytes_read = 0U;
    }
    if ((data == NULL) && (size != 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = flash_user_lock();
    if (err != ESP_OK) {
        return err;
    }

    char full_path[FLASH_USER_PATH_BUFFER_SIZE];
    err = flash_user_build_full_path(partition, path,
                                     FLASH_USER_TRANSACTION_SUFFIX_SIZE,
                                     full_path, sizeof(full_path));
    if (err == ESP_OK) {
        err = flash_user_recover_file_if_writable(partition, full_path);
    }
    if (err != ESP_OK) {
        flash_user_unlock();
        return err;
    }

    FILE *file = fopen(full_path, "rb");
    if (file == NULL) {
        err = flash_user_errno_to_err(errno);
    } else if ((offset > (size_t)LONG_MAX) ||
               (fseek(file, (long)offset, SEEK_SET) != 0)) {
        err = (offset > (size_t)LONG_MAX) ? ESP_ERR_INVALID_SIZE
                                          : flash_user_errno_to_err(errno);
    } else {
        size_t read_count = (size == 0U) ? 0U : fread(data, 1U, size, file);
        if (ferror(file) != 0) {
            err = ESP_FAIL;
        } else if (bytes_read != NULL) {
            *bytes_read = read_count;
        }
    }
    if ((file != NULL) && (fclose(file) != 0) && (err == ESP_OK)) {
        err = flash_user_errno_to_err(errno);
    }

    flash_user_unlock();
    return err;
}

esp_err_t flash_user_file_delete(flash_user_partition_t partition,
                                 const char *path)
{
    if (partition == FLASH_USER_PARTITION_WEBPAGE_VFS) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t err = flash_user_lock();
    if (err != ESP_OK) {
        return err;
    }

    char full_path[FLASH_USER_PATH_BUFFER_SIZE];
    err = flash_user_build_full_path(partition, path,
                                     FLASH_USER_TRANSACTION_SUFFIX_SIZE,
                                     full_path, sizeof(full_path));
    if (err == ESP_OK) {
        err = flash_user_recover_file_if_writable(partition, full_path);
    }
    if ((err == ESP_OK) && (remove(full_path) != 0)) {
        err = flash_user_errno_to_err(errno);
    }

    flash_user_unlock();
    return err;
}

bool flash_user_file_exists(flash_user_partition_t partition, const char *path)
{
    if (flash_user_lock() != ESP_OK) {
        return false;
    }

    char full_path[FLASH_USER_PATH_BUFFER_SIZE];
    bool exists = false;
    esp_err_t err = flash_user_build_full_path(
        partition, path, FLASH_USER_TRANSACTION_SUFFIX_SIZE, full_path,
        sizeof(full_path));
    if (err == ESP_OK) {
        err = flash_user_recover_file_if_writable(partition, full_path);
    }
    if (err == ESP_OK) {
        err = flash_user_stat_path(full_path, &exists);
    }

    flash_user_unlock();
    return (err == ESP_OK) && exists;
}

esp_err_t flash_user_file_get_size(flash_user_partition_t partition,
                                   const char *path, size_t *size)
{
    if (size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *size = 0U;

    esp_err_t err = flash_user_lock();
    if (err != ESP_OK) {
        return err;
    }

    char full_path[FLASH_USER_PATH_BUFFER_SIZE];
    err = flash_user_build_full_path(partition, path,
                                     FLASH_USER_TRANSACTION_SUFFIX_SIZE,
                                     full_path, sizeof(full_path));
    if (err == ESP_OK) {
        err = flash_user_recover_file_if_writable(partition, full_path);
    }
    if (err == ESP_OK) {
        struct stat status;
        if (stat(full_path, &status) != 0) {
            err = flash_user_errno_to_err(errno);
        } else if (status.st_size < 0) {
            err = ESP_FAIL;
        } else {
            *size = (size_t)status.st_size;
        }
    }

    flash_user_unlock();
    return err;
}

static esp_err_t flash_user_nvs_open(const char *namespace_name,
                                     nvs_open_mode_t mode, nvs_handle_t *handle)
{
    if (!s_nvs_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((namespace_name == NULL) || (handle == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_open(namespace_name, mode, handle);
}

esp_err_t flash_user_nvs_set_blob(const char *namespace_name, const char *key,
                                  const void *data, size_t size)
{
    if ((key == NULL) || ((data == NULL) && (size != 0U))) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = flash_user_lock();
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t handle;
    err = flash_user_nvs_open(namespace_name, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        uint8_t empty_blob = 0U;
        const void *blob_data = (data != NULL) ? data : &empty_blob;
        err = nvs_set_blob(handle, key, blob_data, size);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }

    flash_user_unlock();
    return err;
}

esp_err_t flash_user_nvs_get_blob(const char *namespace_name, const char *key,
                                  void *data, size_t size, size_t *bytes_read)
{
    if (bytes_read != NULL) {
        *bytes_read = 0U;
    }
    if ((namespace_name == NULL) || (key == NULL) ||
        ((data == NULL) && (size != 0U))) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = flash_user_lock();
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t handle;
    err = flash_user_nvs_open(namespace_name, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t length = size;
        err = nvs_get_blob(handle, key, data, &length);
        if ((err == ESP_OK) && (bytes_read != NULL)) {
            *bytes_read = length;
        }
        nvs_close(handle);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_ERR_NOT_FOUND;
    } else if ((err == ESP_ERR_NVS_TYPE_MISMATCH) ||
               (err == ESP_ERR_NVS_INVALID_LENGTH)) {
        err = ESP_ERR_INVALID_RESPONSE;
    }

    flash_user_unlock();
    return err;
}

esp_err_t flash_user_nvs_set_string(const char *namespace_name, const char *key,
                                    const char *value)
{
    if ((key == NULL) || (value == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = flash_user_lock();
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t handle;
    err = flash_user_nvs_open(namespace_name, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, key, value);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }

    flash_user_unlock();
    return err;
}

esp_err_t flash_user_nvs_get_string(const char *namespace_name, const char *key,
                                    char *value, size_t size,
                                    size_t *bytes_read)
{
    if (bytes_read != NULL) {
        *bytes_read = 0U;
    }
    if ((namespace_name == NULL) || (key == NULL) ||
        ((value == NULL) && (size != 0U))) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = flash_user_lock();
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t handle;
    err = flash_user_nvs_open(namespace_name, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t length = size;
        err = nvs_get_str(handle, key, value, &length);
        if ((err == ESP_OK) && (bytes_read != NULL)) {
            *bytes_read = length;
        }
        nvs_close(handle);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_ERR_NOT_FOUND;
    } else if ((err == ESP_ERR_NVS_TYPE_MISMATCH) ||
               (err == ESP_ERR_NVS_INVALID_LENGTH)) {
        err = ESP_ERR_INVALID_RESPONSE;
    }

    flash_user_unlock();
    return err;
}

esp_err_t flash_user_nvs_erase_key(const char *namespace_name, const char *key)
{
    if ((namespace_name == NULL) || (key == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = flash_user_lock();
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t handle;
    err = flash_user_nvs_open(namespace_name, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_erase_key(handle, key);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_ERR_NOT_FOUND;
    }

    flash_user_unlock();
    return err;
}

esp_err_t flash_user_nvs_partition_init(const char *partition_label,
                                        bool erase_on_init_error)
{
    if (partition_label == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = flash_user_lock();
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_flash_init_partition(partition_label);
    if (erase_on_init_error && ((err == ESP_ERR_NVS_NO_FREE_PAGES) ||
                                (err == ESP_ERR_NVS_NEW_VERSION_FOUND))) {
        err = nvs_flash_erase_partition(partition_label);
        if (err == ESP_OK) {
            err = nvs_flash_init_partition(partition_label);
        }
    }
    if (err == ESP_ERR_NVS_PART_NOT_FOUND) {
        err = ESP_ERR_NOT_FOUND;
    }
    flash_user_unlock();
    return err;
}

esp_err_t flash_user_nvs_partition_erase(const char *partition_label)
{
    if (partition_label == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = flash_user_lock();
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_flash_erase_partition(partition_label);
    if (err == ESP_ERR_NVS_PART_NOT_FOUND) {
        err = ESP_ERR_NOT_FOUND;
    }
    flash_user_unlock();
    return err;
}

esp_err_t flash_user_nvs_partition_set_blob(const char *partition_label,
                                            const char *namespace_name,
                                            const char *key, const void *data,
                                            size_t size)
{
    if ((partition_label == NULL) || (namespace_name == NULL) ||
        (key == NULL) || ((data == NULL) && (size != 0U))) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = flash_user_lock();
    if (err != ESP_OK) {
        return err;
    }
    nvs_handle_t handle;
    err = nvs_open_from_partition(partition_label, namespace_name,
                                  NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        uint8_t empty_blob = 0U;
        err = nvs_set_blob(handle, key, (data != NULL) ? data : &empty_blob,
                           size);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    flash_user_unlock();
    return err;
}

esp_err_t flash_user_nvs_partition_get_blob(const char *partition_label,
                                            const char *namespace_name,
                                            const char *key, void *data,
                                            size_t size, size_t *bytes_read)
{
    if (bytes_read != NULL) {
        *bytes_read = 0U;
    }
    if ((partition_label == NULL) || (namespace_name == NULL) ||
        (key == NULL) || ((data == NULL) && (size != 0U))) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = flash_user_lock();
    if (err != ESP_OK) {
        return err;
    }
    nvs_handle_t handle;
    err = nvs_open_from_partition(partition_label, namespace_name, NVS_READONLY,
                                  &handle);
    if (err == ESP_OK) {
        size_t length = size;
        err = nvs_get_blob(handle, key, data, &length);
        if ((err == ESP_OK) && (bytes_read != NULL)) {
            *bytes_read = length;
        }
        nvs_close(handle);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_ERR_NOT_FOUND;
    } else if ((err == ESP_ERR_NVS_TYPE_MISMATCH) ||
               (err == ESP_ERR_NVS_INVALID_LENGTH)) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    flash_user_unlock();
    return err;
}

esp_err_t flash_user_nvs_partition_get_string(const char *partition_label,
                                              const char *namespace_name,
                                              const char *key, char *value,
                                              size_t size,
                                              size_t *bytes_read)
{
    if (bytes_read != NULL) {
        *bytes_read = 0U;
    }
    if ((partition_label == NULL) || (namespace_name == NULL) ||
        (key == NULL) || ((value == NULL) && (size != 0U))) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = flash_user_lock();
    if (err != ESP_OK) {
        return err;
    }
    nvs_handle_t handle;
    err = nvs_open_from_partition(partition_label, namespace_name, NVS_READONLY,
                                  &handle);
    if (err == ESP_OK) {
        size_t length = size;
        err = nvs_get_str(handle, key, value, &length);
        if ((err == ESP_OK) && (bytes_read != NULL)) {
            *bytes_read = length;
        }
        nvs_close(handle);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_ERR_NOT_FOUND;
    } else if ((err == ESP_ERR_NVS_TYPE_MISMATCH) ||
               (err == ESP_ERR_NVS_INVALID_LENGTH)) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    flash_user_unlock();
    return err;
}

esp_err_t flash_user_nvs_partition_get_u8(const char *partition_label,
                                          const char *namespace_name,
                                          const char *key, uint8_t *value)
{
    if ((partition_label == NULL) || (namespace_name == NULL) ||
        (key == NULL) || (value == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = flash_user_lock();
    if (err != ESP_OK) {
        return err;
    }
    nvs_handle_t handle;
    err = nvs_open_from_partition(partition_label, namespace_name, NVS_READONLY,
                                  &handle);
    if (err == ESP_OK) {
        err = nvs_get_u8(handle, key, value);
        nvs_close(handle);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_ERR_NOT_FOUND;
    } else if (err == ESP_ERR_NVS_TYPE_MISMATCH) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    flash_user_unlock();
    return err;
}
