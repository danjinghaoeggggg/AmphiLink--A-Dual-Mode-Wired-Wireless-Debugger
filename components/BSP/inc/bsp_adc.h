#ifndef BSP_ADC_H
#define BSP_ADC_H

#include <stdint.h>

#include "esp_err.h"

esp_err_t bsp_adc_init(void);
esp_err_t bsp_adc_read_raw(uint32_t *raw_value);
esp_err_t bsp_adc_read_voltage_mv(uint32_t *voltage_mv);
esp_err_t bsp_adc_deinit(void);

#endif
