#include "bsp_rmt_tx.h"

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"

static rmt_channel_handle_t s_channel;
static rmt_encoder_handle_t s_encoder;

esp_err_t bsp_rmt_tx_init(uint32_t pin, uint32_t resolution_hz)
{
    if (resolution_hz == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_channel != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const rmt_tx_channel_config_t channel_config = {
        .gpio_num = (gpio_num_t)pin,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = resolution_hz,
        .mem_block_symbols = BSP_RMT_TX_MAX_SYMBOLS,
        .trans_queue_depth = 1,
    };
    esp_err_t err = rmt_new_tx_channel(&channel_config, &s_channel);
    if (err != ESP_OK) {
        return err;
    }

    const rmt_copy_encoder_config_t encoder_config = {};
    err = rmt_new_copy_encoder(&encoder_config, &s_encoder);
    if (err != ESP_OK) {
        (void)rmt_del_channel(s_channel);
        s_channel = NULL;
        return err;
    }

    err = rmt_enable(s_channel);
    if (err != ESP_OK) {
        (void)rmt_del_encoder(s_encoder);
        (void)rmt_del_channel(s_channel);
        s_encoder = NULL;
        s_channel = NULL;
    }
    return err;
}

esp_err_t bsp_rmt_tx_write(const bsp_rmt_tx_symbol_t *symbols,
                           size_t symbol_count)
{
    if ((symbols == NULL) || (symbol_count == 0U) ||
        (symbol_count > BSP_RMT_TX_MAX_SYMBOLS)) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((s_channel == NULL) || (s_encoder == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    rmt_symbol_word_t rmt_symbols[BSP_RMT_TX_MAX_SYMBOLS];
    for (size_t i = 0U; i < symbol_count; ++i) {
        if ((symbols[i].level0 > 1U) || (symbols[i].level1 > 1U) ||
            (symbols[i].duration0 == 0U) || (symbols[i].duration1 == 0U)) {
            return ESP_ERR_INVALID_ARG;
        }
        rmt_symbols[i] = (rmt_symbol_word_t) {
            .level0 = symbols[i].level0,
            .duration0 = symbols[i].duration0,
            .level1 = symbols[i].level1,
            .duration1 = symbols[i].duration1,
        };
    }

    const rmt_transmit_config_t transmit_config = {
        .loop_count = 0,
    };
    esp_err_t err = rmt_transmit(s_channel, s_encoder, rmt_symbols,
                                 symbol_count * sizeof(rmt_symbols[0]),
                                 &transmit_config);
    if (err != ESP_OK) {
        return err;
    }
    return rmt_tx_wait_all_done(s_channel, -1);
}

esp_err_t bsp_rmt_tx_deinit(void)
{
    if ((s_channel == NULL) || (s_encoder == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = rmt_disable(s_channel);
    esp_err_t encoder_err = rmt_del_encoder(s_encoder);
    esp_err_t channel_err = rmt_del_channel(s_channel);
    s_encoder = NULL;
    s_channel = NULL;

    if (err != ESP_OK) {
        return err;
    }
    return (encoder_err != ESP_OK) ? encoder_err : channel_err;
}
