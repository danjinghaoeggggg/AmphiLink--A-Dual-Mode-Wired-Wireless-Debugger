#include "bsp_flash.h"

#include "esp_partition.h"

static esp_err_t bsp_flash_check_range(bsp_flash_partition_t partition,
                                       size_t offset, size_t size)
{
    if (partition == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t partition_size =
        ((const esp_partition_t *)partition)->size;
    if ((offset > partition_size) || (size > (partition_size - offset))) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

bsp_flash_partition_t bsp_flash_find_data_partition(const char *label)
{
    if (label == NULL) {
        return NULL;
    }
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    ESP_PARTITION_SUBTYPE_ANY, label);
}

size_t bsp_flash_get_partition_size(bsp_flash_partition_t partition)
{
    if (partition == NULL) {
        return 0U;
    }
    return ((const esp_partition_t *)partition)->size;
}

esp_err_t bsp_flash_read(bsp_flash_partition_t partition, size_t offset,
                         void *data, size_t size)
{
    if ((data == NULL) && (size != 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = bsp_flash_check_range(partition, offset, size);
    if ((err != ESP_OK) || (size == 0U)) {
        return err;
    }
    return esp_partition_read((const esp_partition_t *)partition, offset,
                              data, size);
}

esp_err_t bsp_flash_write(bsp_flash_partition_t partition, size_t offset,
                          const void *data, size_t size)
{
    if ((data == NULL) && (size != 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = bsp_flash_check_range(partition, offset, size);
    if ((err != ESP_OK) || (size == 0U)) {
        return err;
    }
    return esp_partition_write((const esp_partition_t *)partition, offset,
                               data, size);
}

esp_err_t bsp_flash_erase(bsp_flash_partition_t partition, size_t offset,
                          size_t size)
{
    esp_err_t err = bsp_flash_check_range(partition, offset, size);
    if ((err != ESP_OK) || (size == 0U)) {
        return err;
    }
    if (((offset % BSP_FLASH_ERASE_SIZE) != 0U) ||
        ((size % BSP_FLASH_ERASE_SIZE) != 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_partition_erase_range((const esp_partition_t *)partition,
                                     offset, size);
}
