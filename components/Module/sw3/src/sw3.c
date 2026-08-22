#include "sw3.h"

#include <stdbool.h>
#include <stdint.h>

#include "bsp_adc.h"
#include "bsp_gpio.h"
#include "bsp_pin_config.h"

#define SW3_STATE_1_2_BOUNDARY_MV 825U
#define SW3_STATE_2_3_BOUNDARY_MV 2475U

static bool s_initialized;
static bool s_binary_initialized;

sw3_state_t sw3_get_state(void)
{
    if (!s_initialized) {
        if (bsp_adc_init() != ESP_OK) {
            return SW3_STATE_UNKNOWN;
        }
        s_initialized = true;
    }

    uint32_t voltage_mv = 0U;
    if (bsp_adc_read_voltage_mv(&voltage_mv) != ESP_OK) {
        return SW3_STATE_UNKNOWN;
    }

    if (voltage_mv < SW3_STATE_1_2_BOUNDARY_MV) {
        return SW3_STATE_1;
    }
    if (voltage_mv < SW3_STATE_2_3_BOUNDARY_MV) {
        return SW3_STATE_2;
    }
    return SW3_STATE_3;
}

esp_err_t sw3_read_binary_level(bool *high_level)
{
    if (high_level == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_binary_initialized) {
        bsp_gpio_configure(BSP_PIN_MODESEL, BSP_GPIO_MODE_INPUT,
                           BSP_GPIO_PULL_NONE, 0U);
        s_binary_initialized = true;
    }

    *high_level = (bsp_gpio_read(BSP_PIN_MODESEL) != 0U);
    return ESP_OK;
}
