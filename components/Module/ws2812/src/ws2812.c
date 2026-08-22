#include "ws2812.h"

#include <stdbool.h>
#include <stddef.h>

#include "bsp_pin_config.h"
#include "bsp_rmt_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define WS2812_RMT_RESOLUTION_HZ 10000000U
#define WS2812_COLOR_BYTES       3U
#define WS2812_BRIGHTNESS_PERCENT 15U
#define WS2812_DATA_BITS         (WS2812_COLOR_BYTES * 8U)
#define WS2812_SYMBOL_COUNT      (WS2812_DATA_BITS + 1U)
#define WS2812_BREATHE_STEPS     32U
#define WS2812_BREATHE_FRAMES    (WS2812_BREATHE_STEPS * 2U)
#define WS2812_TASK_STACK_SIZE   3072U
#define WS2812_TASK_PRIORITY     (tskIDLE_PRIORITY + 1U)

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} ws2812_rgb_t;

typedef struct {
    ws2812_rgb_t color;
    TickType_t step_ticks;
    uint32_t repeat_count;
} ws2812_breathe_config_t;

static bool s_initialized;
static volatile bool s_stop_requested;
static TaskHandle_t s_breathe_task;
static ws2812_breathe_config_t s_breathe_config;

static const ws2812_rgb_t s_colors[] = {
    {255U, 0U, 0U},
    {255U, 80U, 0U},
    {255U, 220U, 0U},
    {0U, 255U, 0U},
    {0U, 255U, 255U},
    {0U, 0U, 255U},
    {128U, 0U, 255U},
    {255U, 0U, 255U},
    {255U, 64U, 128U},
    {255U, 255U, 255U},
};

static const bsp_rmt_tx_symbol_t s_zero = {
    .duration0 = 3,
    .duration1 = 9,
    .level0 = 1,
    .level1 = 0,
};

static const bsp_rmt_tx_symbol_t s_one = {
    .duration0 = 9,
    .duration1 = 3,
    .level0 = 1,
    .level1 = 0,
};

static const bsp_rmt_tx_symbol_t s_reset = {
    .duration0 = 400,
    .duration1 = 400,
    .level0 = 0,
    .level1 = 0,
};

static uint8_t apply_brightness(uint8_t value)
{
    return (uint8_t)(((uint32_t)value * WS2812_BRIGHTNESS_PERCENT) / 100U);
}

static esp_err_t set_scaled_rgb(uint8_t red, uint8_t green, uint8_t blue,
                                uint32_t step)
{
    const uint32_t scale = step * step;
    const uint32_t scale_max =
        WS2812_BREATHE_STEPS * WS2812_BREATHE_STEPS;
    return ws2812_set_rgb((uint8_t)((red * scale) / scale_max),
                          (uint8_t)((green * scale) / scale_max),
                          (uint8_t)((blue * scale) / scale_max));
}

static esp_err_t get_color(ws2812_color_t color, ws2812_rgb_t *rgb)
{
    if ((color < WS2812_COLOR_RED) || (color > WS2812_COLOR_WHITE)) {
        return ESP_ERR_INVALID_ARG;
    }

    *rgb = s_colors[(uint32_t)color - (uint32_t)WS2812_COLOR_RED];
    return ESP_OK;
}

static void breathe_task(void *argument)
{
    const ws2812_breathe_config_t config =
        *(const ws2812_breathe_config_t *)argument;

    for (uint32_t repeat = 0U;
         (repeat < config.repeat_count) && !s_stop_requested; ++repeat) {
        for (uint32_t step = 0U;
             (step <= WS2812_BREATHE_STEPS) && !s_stop_requested; ++step) {
            if (set_scaled_rgb(config.color.red, config.color.green,
                               config.color.blue, step) != ESP_OK) {
                goto finished;
            }
            vTaskDelay(config.step_ticks);
        }
        for (uint32_t step = WS2812_BREATHE_STEPS - 1U;
             (step > 0U) && !s_stop_requested; --step) {
            if (set_scaled_rgb(config.color.red, config.color.green,
                               config.color.blue, step) != ESP_OK) {
                goto finished;
            }
            vTaskDelay(config.step_ticks);
        }
    }

finished:
    (void)ws2812_off();
    s_breathe_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t ws2812_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err =
        bsp_rmt_tx_init(BSP_PIN_RMT_TX, WS2812_RMT_RESOLUTION_HZ);
    if (err == ESP_OK) {
        s_initialized = true;
    }
    return err;
}

esp_err_t ws2812_set_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    const uint8_t grb[WS2812_COLOR_BYTES] = {
        apply_brightness(green),
        apply_brightness(red),
        apply_brightness(blue),
    };
    bsp_rmt_tx_symbol_t symbols[WS2812_SYMBOL_COUNT];
    size_t symbol_index = 0U;

    for (size_t byte_index = 0U; byte_index < WS2812_COLOR_BYTES;
         ++byte_index) {
        for (uint8_t mask = 0x80U; mask != 0U; mask >>= 1U) {
            symbols[symbol_index++] =
                ((grb[byte_index] & mask) != 0U) ? s_one : s_zero;
        }
    }
    symbols[symbol_index] = s_reset;
    return bsp_rmt_tx_write(symbols, WS2812_SYMBOL_COUNT);
}

esp_err_t ws2812_set_color(ws2812_color_t color)
{
    ws2812_rgb_t rgb;
    esp_err_t err = get_color(color, &rgb);
    if (err != ESP_OK) {
        return err;
    }
    return ws2812_set_rgb(rgb.red, rgb.green, rgb.blue);
}

esp_err_t ws2812_breathe(ws2812_color_t color, uint32_t period_ms,
                         uint32_t repeat_count)
{
    const TickType_t period_ticks = pdMS_TO_TICKS(period_ms);
    if ((repeat_count == 0U) || (period_ticks < WS2812_BREATHE_FRAMES)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || (s_breathe_task != NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    ws2812_rgb_t rgb;
    esp_err_t err = get_color(color, &rgb);
    if (err != ESP_OK) {
        return err;
    }

    s_breathe_config = (ws2812_breathe_config_t) {
        .color = rgb,
        .step_ticks = period_ticks / WS2812_BREATHE_FRAMES,
        .repeat_count = repeat_count,
    };
    s_stop_requested = false;
    if (xTaskCreatePinnedToCore(breathe_task, "WS2812-Breathe",
                                WS2812_TASK_STACK_SIZE, &s_breathe_config,
                                WS2812_TASK_PRIORITY, &s_breathe_task,
                                0) != pdPASS) {
        s_breathe_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ws2812_stop_breathe(void)
{
    if (s_breathe_task == NULL) {
        return ESP_OK;
    }

    s_stop_requested = true;
    while (s_breathe_task != NULL) {
        vTaskDelay(1);
    }
    return ESP_OK;
}

esp_err_t ws2812_off(void)
{
    return ws2812_set_rgb(0U, 0U, 0U);
}

esp_err_t ws2812_deinit(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    (void)ws2812_stop_breathe();
    esp_err_t err = bsp_rmt_tx_deinit();
    s_initialized = false;
    return err;
}
