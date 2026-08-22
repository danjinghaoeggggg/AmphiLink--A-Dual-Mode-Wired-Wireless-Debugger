#ifndef __DAP_USB_DESCRIPTORS_H__
#define __DAP_USB_DESCRIPTORS_H__

#include <stddef.h>

#define USB_VID 0x303A
#define USB_PID 0x83B3
#define USB_BCD 0x0100

#define USB_MS_OS_VENDOR_CODE 0x20
#define USB_MS_OS_20_DESC_LEN 0xB2

// Interface layout
#define ITF_NUM_DAP 0
#define ITF_NUM_TOTAL 1

// Endpoint layout (EP1 OUT + EP1 IN)
#define EP_DAP_OUT 0x01
#define EP_DAP_IN  0x81

static const char *const string_desc_arr[] = {
    NULL,
    "danjinghaoeggggg",
    "AmphiLink (CMSIS-DAP V2)",
    NULL,
    "AmphiLink (CMSIS-DAP V2)",
};

#define USB_STRING_SERIAL_INDEX 3U

#endif
