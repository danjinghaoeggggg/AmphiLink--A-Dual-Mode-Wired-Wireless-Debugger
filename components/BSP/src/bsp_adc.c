#include "bsp_adc.h"

#include <stddef.h>

#include "bsp_pin_config.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"

#define BSP_ADC_ATTENUATION ADC_ATTEN_DB_12
#define BSP_ADC_BITWIDTH    ADC_BITWIDTH_DEFAULT

static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_calibration_handle;
static adc_unit_t s_adc_unit;
static adc_channel_t s_adc_channel;

esp_err_t bsp_adc_init(void)
{
    if (s_adc_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = adc_oneshot_io_to_channel(BSP_PIN_MODESEL, &s_adc_unit,
                                               &s_adc_channel);
    if (err != ESP_OK) {
        return err;
    }

    const adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = s_adc_unit,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    err = adc_oneshot_new_unit(&unit_config, &s_adc_handle);
    if (err != ESP_OK) {
        return err;
    }

    const adc_oneshot_chan_cfg_t channel_config = {
        .atten = BSP_ADC_ATTENUATION,
        .bitwidth = BSP_ADC_BITWIDTH,
    };
    err = adc_oneshot_config_channel(s_adc_handle, s_adc_channel,
                                     &channel_config);
    if (err != ESP_OK) {
        (void)adc_oneshot_del_unit(s_adc_handle);
        s_adc_handle = NULL;
        return err;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    const adc_cali_curve_fitting_config_t calibration_config = {
        .unit_id = s_adc_unit,
        .chan = s_adc_channel,
        .atten = BSP_ADC_ATTENUATION,
        .bitwidth = BSP_ADC_BITWIDTH,
    };
    err = adc_cali_create_scheme_curve_fitting(&calibration_config,
                                                &s_calibration_handle);
#else
    err = ESP_ERR_NOT_SUPPORTED;
#endif
    if (err != ESP_OK) {
        (void)adc_oneshot_del_unit(s_adc_handle);
        s_adc_handle = NULL;
        return err;
    }

    return ESP_OK;
}

esp_err_t bsp_adc_read_raw(uint32_t *raw_value)
{
    if (raw_value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_adc_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    int raw = 0;
    esp_err_t err = adc_oneshot_read(s_adc_handle, s_adc_channel, &raw);
    if (err == ESP_OK) {
        *raw_value = (uint32_t)raw;
    }
    return err;
}

esp_err_t bsp_adc_read_voltage_mv(uint32_t *voltage_mv)
{
    if (voltage_mv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((s_adc_handle == NULL) || (s_calibration_handle == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    int voltage = 0;
    esp_err_t err = adc_oneshot_get_calibrated_result(
        s_adc_handle, s_calibration_handle, s_adc_channel, &voltage);
    if (err == ESP_OK) {
        *voltage_mv = (uint32_t)voltage;
    }
    return err;
}

esp_err_t bsp_adc_deinit(void)
{
    if (s_adc_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t calibration_err = ESP_OK;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (s_calibration_handle != NULL) {
        calibration_err =
            adc_cali_delete_scheme_curve_fitting(s_calibration_handle);
        s_calibration_handle = NULL;
    }
#endif

    esp_err_t unit_err = adc_oneshot_del_unit(s_adc_handle);
    s_adc_handle = NULL;

    return (calibration_err != ESP_OK) ? calibration_err : unit_err;
}
