#include "bsp_system.h"

#include "esp_mac.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

void bsp_system_get_unique_id(uint8_t id[6])
{
    esp_read_mac(id, ESP_MAC_WIFI_STA);
}

uint32_t bsp_system_get_timestamp_us(void)
{
    return (uint32_t)esp_timer_get_time();
}

void bsp_system_delay_us(uint32_t delay_us)
{
    esp_rom_delay_us(delay_us);
}
