#ifndef DAP_PORT_H
#define DAP_PORT_H

#include <stdbool.h>
#include <stdint.h>

#include "bsp_pin_config.h"
#include "bsp_system.h"
#include "sdkconfig.h"
#include "soc/gpio_struct.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "The DAP GPIO register fast path currently supports ESP32-S3 only"
#endif

#if (BSP_PIN_DEBUG_CLOCK >= 32U) || (BSP_PIN_DEBUG_DATA >= 32U) || \
    (BSP_PIN_TARGET_RESET >= 32U) || (BSP_PIN_JTAG_TDI >= 32U) || \
    (BSP_PIN_JTAG_TDO >= 32U) || (BSP_PIN_JTAG_TRST >= 32U)
#error "The DAP SWD/JTAG GPIO fast path requires debug and reset pins below GPIO32"
#endif

/*
 * DAP owns these signals and accesses the ESP32-S3 GPIO peripheral directly.
 * This deliberately bypasses the general BSP GPIO API so the per-bit SWD path
 * has a short, predictable instruction sequence. SWDIO input remains enabled;
 * turnaround only changes its output-enable bit. JTAG uses the same fast GPIO
 * bank for TDI/TDO/nTRST. BSP remains responsible for
 * board pin assignment and system services, not DAP signal manipulation.
 */
#define DAP_PORT_DEBUG_CLOCK_MASK (1U << BSP_PIN_DEBUG_CLOCK)
#define DAP_PORT_DEBUG_DATA_MASK  (1U << BSP_PIN_DEBUG_DATA)
#define DAP_PORT_TARGET_RESET_MASK (1U << BSP_PIN_TARGET_RESET)
#define DAP_PORT_JTAG_TDI_MASK     (1U << BSP_PIN_JTAG_TDI)
#define DAP_PORT_JTAG_TRST_MASK    (1U << BSP_PIN_JTAG_TRST)
#define DAP_PORT_CPU_CLOCK_HZ       BSP_SYSTEM_CPU_CLOCK_HZ
#define DAP_PORT_GPIO_WRITE_CYCLES  2U
#define DAP_PORT_TIMESTAMP_CLOCK_HZ BSP_SYSTEM_TIMESTAMP_CLOCK_HZ

void dap_port_setup(void);
void dap_port_swd_setup(void);
void dap_port_jtag_setup(void);
void dap_port_off(void);
uint8_t dap_port_reset_target(void);

static inline void dap_port_get_unique_id(uint8_t id[6])
{
    bsp_system_get_unique_id(id);
}

static inline uint32_t dap_port_get_timestamp(void)
{
    return bsp_system_get_timestamp_us();
}

static inline __attribute__((always_inline)) uint32_t dap_port_debug_clock_read(void)
{
    return (GPIO.in >> BSP_PIN_DEBUG_CLOCK) & 1U;
}

static inline __attribute__((always_inline)) void dap_port_gpio_write(uint32_t pin,
                                                                       uint32_t level)
{
    const uint32_t mask = 1U << (pin & 31U);

    if (pin < 32U) {
        if ((level & 1U) != 0U) {
            GPIO.out_w1ts = mask;
        } else {
            GPIO.out_w1tc = mask;
        }
    } else if ((level & 1U) != 0U) {
        GPIO.out1_w1ts.val = mask;
    } else {
        GPIO.out1_w1tc.val = mask;
    }
}

static inline __attribute__((always_inline)) void dap_port_gpio_set_output(uint32_t pin,
                                                                             bool enabled)
{
    const uint32_t mask = 1U << (pin & 31U);

    if (pin < 32U) {
        if (enabled) {
            GPIO.enable_w1ts = mask;
        } else {
            GPIO.enable_w1tc = mask;
        }
    } else if (enabled) {
        GPIO.enable1_w1ts.data = mask;
    } else {
        GPIO.enable1_w1tc.data = mask;
    }
}

static inline __attribute__((always_inline)) void dap_port_debug_clock_write(uint32_t level)
{
    if ((level & 1U) != 0U) {
        GPIO.out_w1ts = DAP_PORT_DEBUG_CLOCK_MASK;
    } else {
        GPIO.out_w1tc = DAP_PORT_DEBUG_CLOCK_MASK;
    }
}

static inline __attribute__((always_inline)) uint32_t dap_port_debug_data_read(void)
{
    return (GPIO.in >> BSP_PIN_DEBUG_DATA) & 1U;
}

static inline __attribute__((always_inline)) void dap_port_debug_data_write(uint32_t level)
{
    if ((level & 1U) != 0U) {
        GPIO.out_w1ts = DAP_PORT_DEBUG_DATA_MASK;
    } else {
        GPIO.out_w1tc = DAP_PORT_DEBUG_DATA_MASK;
    }
}

static inline __attribute__((always_inline)) void dap_port_debug_data_set_output(bool enabled)
{
    if (enabled) {
        GPIO.enable_w1ts = DAP_PORT_DEBUG_DATA_MASK;
    } else {
        GPIO.enable_w1tc = DAP_PORT_DEBUG_DATA_MASK;
    }
}

static inline __attribute__((always_inline)) uint32_t dap_port_jtag_tdi_read(void)
{
    return (GPIO.in >> BSP_PIN_JTAG_TDI) & 1U;
}

static inline __attribute__((always_inline)) void dap_port_jtag_tdi_write(uint32_t level)
{
    if ((level & 1U) != 0U) {
        GPIO.out_w1ts = DAP_PORT_JTAG_TDI_MASK;
    } else {
        GPIO.out_w1tc = DAP_PORT_JTAG_TDI_MASK;
    }
}

static inline __attribute__((always_inline)) uint32_t dap_port_jtag_tdo_read(void)
{
    return (GPIO.in >> BSP_PIN_JTAG_TDO) & 1U;
}

static inline __attribute__((always_inline)) uint32_t dap_port_jtag_trst_read(void)
{
    return (GPIO.in >> BSP_PIN_JTAG_TRST) & 1U;
}

static inline __attribute__((always_inline)) void dap_port_jtag_trst_write(uint32_t level)
{
    /* The nTRST pad is open-drain: high releases the line. */
    if ((level & 1U) != 0U) {
        GPIO.out_w1ts = DAP_PORT_JTAG_TRST_MASK;
    } else {
        GPIO.out_w1tc = DAP_PORT_JTAG_TRST_MASK;
    }
}

static inline __attribute__((always_inline)) uint32_t dap_port_target_reset_read(void)
{
    return (GPIO.in >> BSP_PIN_TARGET_RESET) & 1U;
}

void dap_port_target_reset_write(uint32_t level);
void dap_port_connected_led_write(uint32_t level);
void dap_port_running_led_write(uint32_t level);

#endif
