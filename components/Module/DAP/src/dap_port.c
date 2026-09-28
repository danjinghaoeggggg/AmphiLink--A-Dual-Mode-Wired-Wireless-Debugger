#include "dap_port.h"

#include "soc/io_mux_reg.h"

#define DAP_PORT_IO_MUX_REG(pin) (IO_MUX_GPIO0_REG + ((pin) * sizeof(uint32_t)))

static void dap_port_configure_gpio(uint32_t pin, bool input_enabled, bool output_enabled,
                                    bool open_drain, bool pull_up, uint32_t initial_level)
{
    const uint32_t io_mux_reg = DAP_PORT_IO_MUX_REG(pin);

    /*
     * DAP owns these pins, so configure IO_MUX and GPIO registers here instead
     * of going through BSP or GPIO LL wrappers. Set the output latch before
     * enabling the driver to avoid a transient on SWCLK, SWDIO, TDI,
     * nTRST, or nRESET.
     */
    PIN_FUNC_SELECT(io_mux_reg, PIN_FUNC_GPIO);
    dap_port_gpio_write(pin, initial_level);

    if (input_enabled) {
        PIN_INPUT_ENABLE(io_mux_reg);
    } else {
        PIN_INPUT_DISABLE(io_mux_reg);
    }

    GPIO.pin[pin].pad_driver = open_drain ? 1U : 0U;

    if (pull_up) {
        PIN_PULLUP_EN(io_mux_reg);
    } else {
        PIN_PULLUP_DIS(io_mux_reg);
    }
    PIN_PULLDWN_DIS(io_mux_reg);

    dap_port_gpio_set_output(pin, output_enabled);
}

void dap_port_setup(void)
{
    dap_port_configure_gpio(BSP_PIN_STATUS_CONNECTED_LED, false, true,
                            false, false, 0U);
    dap_port_configure_gpio(BSP_PIN_STATUS_RUNNING_LED, false, true,
                            false, false, 0U);
    PIN_SET_DRV(DAP_PORT_IO_MUX_REG(BSP_PIN_DEBUG_CLOCK), 0U);
    PIN_SET_DRV(DAP_PORT_IO_MUX_REG(BSP_PIN_DEBUG_DATA), 0U);
    PIN_SET_DRV(DAP_PORT_IO_MUX_REG(BSP_PIN_JTAG_TDI), 0U);
    dap_port_off();
}

void dap_port_swd_setup(void)
{
    dap_port_configure_gpio(BSP_PIN_JTAG_TDI, true, false, false, false, 1U);
    dap_port_configure_gpio(BSP_PIN_JTAG_TDO, true, false, false, false, 1U);
    dap_port_configure_gpio(BSP_PIN_JTAG_TRST, true, true, true, true, 1U);
    dap_port_configure_gpio(BSP_PIN_DEBUG_CLOCK, true, true, false, false, 1U);
    dap_port_configure_gpio(BSP_PIN_DEBUG_DATA, true, true, false, false, 1U);
    dap_port_configure_gpio(BSP_PIN_TARGET_RESET, true, true, true, true, 1U);
}

void dap_port_jtag_setup(void)
{
    dap_port_configure_gpio(BSP_PIN_DEBUG_CLOCK, true, true, false, false, 1U);
    dap_port_configure_gpio(BSP_PIN_DEBUG_DATA, true, true, false, false, 1U);
    dap_port_configure_gpio(BSP_PIN_JTAG_TDI, true, true, false, false, 1U);
    dap_port_configure_gpio(BSP_PIN_JTAG_TDO, true, false, false, false, 1U);
    dap_port_configure_gpio(BSP_PIN_JTAG_TRST, true, true, true, true, 1U);
    dap_port_configure_gpio(BSP_PIN_TARGET_RESET, true, true, true, true, 1U);
}

void dap_port_off(void)
{
    dap_port_configure_gpio(BSP_PIN_DEBUG_CLOCK, true, false, false, false, 1U);
    dap_port_configure_gpio(BSP_PIN_DEBUG_DATA, true, false, false, false, 1U);
    dap_port_configure_gpio(BSP_PIN_JTAG_TDI, true, false, false, false, 1U);
    dap_port_configure_gpio(BSP_PIN_JTAG_TDO, true, false, false, false, 1U);
    dap_port_configure_gpio(BSP_PIN_JTAG_TRST, true, true, true, true, 1U);
    dap_port_configure_gpio(BSP_PIN_TARGET_RESET, true, true, true, true, 1U);
}

void dap_port_target_reset_write(uint32_t level)
{
    dap_port_gpio_write(BSP_PIN_TARGET_RESET, level);
}

void dap_port_connected_led_write(uint32_t level)
{
    dap_port_gpio_write(BSP_PIN_STATUS_CONNECTED_LED, level);
}

void dap_port_running_led_write(uint32_t level)
{
    dap_port_gpio_write(BSP_PIN_STATUS_RUNNING_LED, level);
}

uint8_t dap_port_reset_target(void)
{
    dap_port_target_reset_write(0U);
    bsp_system_delay_us(2000U);
    dap_port_target_reset_write(1U);
    bsp_system_delay_us(2000U);
    return 1U;
}
