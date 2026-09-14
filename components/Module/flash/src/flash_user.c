#include "flash_user.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#define FLASH_USER_WEBPAGE_PARTITION_LABEL "webpage_vfs"
#define FLASH_USER_WEBPAGE_MOUNT_PATH "/webpage"
#define FLASH_USER_WEBPAGE_MAX_OPEN_FILES 8U
#define FLASH_USER_WEBPAGE_PATH_BUFFER_SIZE 128U

static const char *TAG = "FLASH_USER";

static bool s_nvs_ready;
static bool s_webpage_mounted;
static SemaphoreHandle_t s_flash_mutex;

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

static esp_err_t flash_user_build_webpage_path(
    const char *path, char *full_path, size_t full_path_size)
{
    if (!s_webpage_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((path == NULL) || (path[0] == '\0') || (path[0] == '/') ||
        flash_user_has_parent_segment(path)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(path) > FLASH_USER_WEBPAGE_MAX_RELATIVE_PATH) {
        return ESP_ERR_INVALID_SIZE;
    }

    int written = snprintf(full_path, full_path_size, "%s/%s",
                           FLASH_USER_WEBPAGE_MOUNT_PATH, path);
    if ((written < 0) || ((size_t)written >= full_path_size)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t flash_user_mount_webpage(void)
{
    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = FLASH_USER_WEBPAGE_MAX_OPEN_FILES,
        .allocation_unit_size = 0,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };
    return esp_vfs_fat_spiflash_mount_ro(FLASH_USER_WEBPAGE_MOUNT_PATH,
                                         FLASH_USER_WEBPAGE_PARTITION_LABEL,
                                         &mount_config);
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
    if (s_nvs_ready && s_webpage_mounted) {
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

    if (!s_webpage_mounted) {
        esp_err_t err = flash_user_mount_webpage();
        if (err == ESP_OK) {
            s_webpage_mounted = true;
            ESP_LOGI(TAG, "Webpage FAT partition mounted at %s",
                     FLASH_USER_WEBPAGE_MOUNT_PATH);
        } else {
            ESP_LOGE(TAG, "Webpage FAT initialization failed: %s",
                     esp_err_to_name(err));
            if (first_error == ESP_OK) {
                first_error = err;
            }
        }
    }

    flash_user_unlock();
    return first_error;
}

esp_err_t flash_user_webpage_get_size(const char *path, size_t *size)
{
    if (size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *size = 0U;

    esp_err_t err = flash_user_lock();
    if (err != ESP_OK) {
        return err;
    }

    char full_path[FLASH_USER_WEBPAGE_PATH_BUFFER_SIZE];
    err = flash_user_build_webpage_path(path, full_path, sizeof(full_path));
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

esp_err_t flash_user_webpage_read_at(const char *path, size_t offset,
                                     void *data, size_t size,
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

    char full_path[FLASH_USER_WEBPAGE_PATH_BUFFER_SIZE];
    err = flash_user_build_webpage_path(path, full_path, sizeof(full_path));
    FILE *file = NULL;
    if (err == ESP_OK) {
        file = fopen(full_path, "rb");
        if (file == NULL) {
            err = flash_user_errno_to_err(errno);
        }
    }
    if ((err == ESP_OK) &&
        ((offset > (size_t)LONG_MAX) ||
         (fseek(file, (long)offset, SEEK_SET) != 0))) {
        err = (offset > (size_t)LONG_MAX) ? ESP_ERR_INVALID_SIZE
                                          : flash_user_errno_to_err(errno);
    }
    if (err == ESP_OK) {
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
