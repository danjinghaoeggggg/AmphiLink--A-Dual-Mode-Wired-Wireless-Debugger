#ifndef BSP_FLASH_H
#define BSP_FLASH_H

#include <stddef.h>

#include "esp_err.h"

#define BSP_FLASH_ERASE_SIZE 4096U

typedef const void *bsp_flash_partition_t;

bsp_flash_partition_t bsp_flash_find_data_partition(const char *label);
size_t bsp_flash_get_partition_size(bsp_flash_partition_t partition);
esp_err_t bsp_flash_read(bsp_flash_partition_t partition, size_t offset,
                         void *data, size_t size);
esp_err_t bsp_flash_write(bsp_flash_partition_t partition, size_t offset,
                          const void *data, size_t size);
esp_err_t bsp_flash_erase(bsp_flash_partition_t partition, size_t offset,
                          size_t size);

#endif
