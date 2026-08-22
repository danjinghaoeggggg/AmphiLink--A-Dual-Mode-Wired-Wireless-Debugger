#include "amphilink.h"

#include "esp_err.h"

#include "dap_core_service.h"
#include "flash_user.h"
#include "transport_manager.h"

void amphilink_start(void)
{
    ESP_ERROR_CHECK(flash_user_init());
    ESP_ERROR_CHECK(dap_core_service_init());
    ESP_ERROR_CHECK(transport_manager_start());
}
