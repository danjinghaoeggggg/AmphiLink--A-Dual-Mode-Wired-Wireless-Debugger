#ifndef BSP_SYSTEM_H
#define BSP_SYSTEM_H

#include <stdint.h>

#define BSP_SYSTEM_CPU_CLOCK_HZ       240000000U
#define BSP_SYSTEM_TIMESTAMP_CLOCK_HZ 1000000U

void bsp_system_get_unique_id(uint8_t id[6]);
uint32_t bsp_system_get_timestamp_us(void);
void bsp_system_delay_us(uint32_t delay_us);

#endif
