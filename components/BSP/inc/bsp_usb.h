#ifndef BSP_USB_H
#define BSP_USB_H

#include "esp_err.h"

#define BSP_USB_DEVICE_RHPORT     0U
#define BSP_USB_DEVICE_FULL_SPEED 1

esp_err_t bsp_usb_phy_init(void);
esp_err_t bsp_usb_phy_deinit(void);

#endif
