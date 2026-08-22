#include "bsp_gpio.h"

#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"

static gpio_mode_t to_gpio_mode(bsp_gpio_mode_t mode)
{
    switch (mode) {
    case BSP_GPIO_MODE_OUTPUT:
        return GPIO_MODE_OUTPUT;
    case BSP_GPIO_MODE_INPUT_OUTPUT:
        return GPIO_MODE_INPUT_OUTPUT;
    case BSP_GPIO_MODE_INPUT_OUTPUT_OPEN_DRAIN:
        return GPIO_MODE_INPUT_OUTPUT_OD;
    case BSP_GPIO_MODE_INPUT:
    default:
        return GPIO_MODE_INPUT;
    }
}

void bsp_gpio_configure(bsp_gpio_pin_t pin, bsp_gpio_mode_t mode,
                        bsp_gpio_pull_t pull, uint32_t initial_level)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << pin,
        .mode = to_gpio_mode(mode),
        .pull_up_en = (pull == BSP_GPIO_PULL_UP) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (pull == BSP_GPIO_PULL_DOWN) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    gpio_config(&config);
    gpio_set_level((gpio_num_t)pin, (uint32_t)(initial_level & 1U));
}

void bsp_gpio_set_drive_strength(bsp_gpio_pin_t pin, bsp_gpio_drive_strength_t strength)
{
    gpio_ll_set_drive_capability(&GPIO, pin, (gpio_drive_cap_t)strength);
}
