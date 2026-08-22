#include "bsp_usb.h"

#include <stddef.h>

#include "esp_private/usb_phy.h"

static usb_phy_handle_t s_phy_handle = NULL;

esp_err_t bsp_usb_phy_init(void)
{
    if (s_phy_handle != NULL) {
        return ESP_OK;
    }

    const usb_phy_config_t phy_config = {
        .controller = USB_PHY_CTRL_OTG,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .target = USB_PHY_TARGET_INT,
        .otg_speed = USB_PHY_SPEED_UNDEFINED,
    };

    return usb_new_phy(&phy_config, &s_phy_handle);
}

esp_err_t bsp_usb_phy_deinit(void)
{
    if (s_phy_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = usb_del_phy(s_phy_handle);
    if (err == ESP_OK) {
        s_phy_handle = NULL;
    }
    return err;
}
