#ifndef SW3_H
#define SW3_H

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    SW3_STATE_UNKNOWN = 0,
    SW3_STATE_1,
    SW3_STATE_2,
    SW3_STATE_3,
} sw3_state_t;

sw3_state_t sw3_get_state(void);
esp_err_t sw3_read_binary_level(bool *high_level);

#endif
