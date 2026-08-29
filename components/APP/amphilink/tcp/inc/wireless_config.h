#ifndef WIRELESS_CONFIG_H
#define WIRELESS_CONFIG_H

#define DAP_TCP_SERVER_PORT 4441U
#define DAP_DISCOVERY_UDP_PORT 4442U
#define SERIAL_TCP_SERVER_PORT 4443U
#define DAP_DISCOVERY_INTERVAL_MS 2000U

#define SERIAL_DEFAULT_ENABLED true
#define SERIAL_DEFAULT_BAUDRATE 115200U
#define SERIAL_DEFAULT_DATA_BITS 8U
#define SERIAL_DEFAULT_STOP_BITS 1U

#if __has_include("wireless_secrets.h")
#include "wireless_secrets.h"
#else
#include "wireless_secrets_example.h"
#endif

#define ADMIN_PASSWORD_LENGTH (sizeof(ADMIN_PASSWORD) - 1U)
_Static_assert(ADMIN_PASSWORD_LENGTH >= 8U,
               "SoftAP password must contain at least 8 bytes");
_Static_assert(ADMIN_PASSWORD_LENGTH <= 63U,
               "SoftAP password must contain at most 63 bytes");

#endif
