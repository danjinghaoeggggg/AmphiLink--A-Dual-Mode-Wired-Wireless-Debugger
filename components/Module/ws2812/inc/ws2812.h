#ifndef WS2812_H
#define WS2812_H

#include <stdint.h>

#include "esp_err.h"

typedef enum {
    WS2812_COLOR_RED = 1,
    WS2812_COLOR_ORANGE = 2,
    WS2812_COLOR_YELLOW = 3,
    WS2812_COLOR_GREEN = 4,
    WS2812_COLOR_CYAN = 5,
    WS2812_COLOR_BLUE = 6,
    WS2812_COLOR_PURPLE = 7,
    WS2812_COLOR_MAGENTA = 8,
    WS2812_COLOR_PINK = 9,
    WS2812_COLOR_WHITE = 10,
} ws2812_color_t;

esp_err_t ws2812_init(void);
esp_err_t ws2812_set_rgb(uint8_t red, uint8_t green, uint8_t blue);
esp_err_t ws2812_set_color(ws2812_color_t color);
esp_err_t ws2812_breathe(ws2812_color_t color, uint32_t period_ms,
                         uint32_t repeat_count);
esp_err_t ws2812_stop_breathe(void);
esp_err_t ws2812_off(void);
esp_err_t ws2812_deinit(void);

#endif
