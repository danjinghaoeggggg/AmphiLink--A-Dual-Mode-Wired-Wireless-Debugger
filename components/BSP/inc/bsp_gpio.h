#ifndef BSP_GPIO_H
#define BSP_GPIO_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"

typedef uint32_t bsp_gpio_pin_t;

typedef enum {
    BSP_GPIO_MODE_INPUT = 0,
    BSP_GPIO_MODE_OUTPUT,
    BSP_GPIO_MODE_INPUT_OUTPUT,
    BSP_GPIO_MODE_INPUT_OUTPUT_OPEN_DRAIN,
} bsp_gpio_mode_t;

typedef enum {
    BSP_GPIO_PULL_NONE = 0,
    BSP_GPIO_PULL_UP,
    BSP_GPIO_PULL_DOWN,
} bsp_gpio_pull_t;

typedef enum {
    BSP_GPIO_DRIVE_STRENGTH_0 = 0,
    BSP_GPIO_DRIVE_STRENGTH_1,
    BSP_GPIO_DRIVE_STRENGTH_2,
    BSP_GPIO_DRIVE_STRENGTH_3,
} bsp_gpio_drive_strength_t;

#define BSP_GPIO_WRITE_CYCLES 2U

void bsp_gpio_configure(bsp_gpio_pin_t pin, bsp_gpio_mode_t mode,
                        bsp_gpio_pull_t pull, uint32_t initial_level);
void bsp_gpio_set_drive_strength(bsp_gpio_pin_t pin, bsp_gpio_drive_strength_t strength);

static inline __attribute__((always_inline)) uint32_t bsp_gpio_read(bsp_gpio_pin_t pin)
{
    return (uint32_t)gpio_get_level((gpio_num_t)pin);
}

static inline __attribute__((always_inline)) void bsp_gpio_write(bsp_gpio_pin_t pin,
                                                                  uint32_t level)
{
    gpio_set_level((gpio_num_t)pin, (uint32_t)(level & 1U));
}

static inline __attribute__((always_inline)) void bsp_gpio_set_output_enabled(
    bsp_gpio_pin_t pin, bool enabled)
{
    gpio_set_direction((gpio_num_t)pin, enabled ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT);
}

#endif
