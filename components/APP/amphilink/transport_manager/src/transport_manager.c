#include "transport_manager.h"

#include <stddef.h>

#include "esp_err.h"
#include "esp_log.h"
#include "sw3.h"
#include "tcp_transport.h"
#include "transport_path_config.h"
#include "usb_transport.h"

typedef enum {
    TRANSPORT_PATH_NONE = 0,
    TRANSPORT_PATH_WIRED,
    TRANSPORT_PATH_WIRELESS,
} transport_path_t;

static const char *TAG = "TRANSPORT_MANAGER";
static transport_path_t s_active_path = TRANSPORT_PATH_NONE;

static const char *transport_path_name(transport_path_t path)
{
    switch (path) {
    case TRANSPORT_PATH_WIRED:
        return "wired USB";
    case TRANSPORT_PATH_WIRELESS:
        return "wireless TCP";
    case TRANSPORT_PATH_NONE:
    default:
        return "none";
    }
}

static esp_err_t transport_path_start(transport_path_t path)
{
    switch (path) {
    case TRANSPORT_PATH_WIRED:
        return usb_transport_start();
    case TRANSPORT_PATH_WIRELESS:
        return tcp_transport_start();
    case TRANSPORT_PATH_NONE:
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

static esp_err_t transport_manager_select_and_start(sw3_state_t switch_state)
{
    transport_path_t attempts[2];
    size_t attempt_count = 1U;
    if (switch_state == DAP_PATH_SW3_WIRED) {
        attempts[0] = TRANSPORT_PATH_WIRED;
    } else if (switch_state == DAP_PATH_SW3_WIRELESS) {
        attempts[0] = TRANSPORT_PATH_WIRELESS;
    } else {
        if (switch_state == SW3_STATE_UNKNOWN) {
            ESP_LOGW(TAG, "SW3 state unavailable; using auto mode");
        } else if (switch_state != DAP_PATH_SW3_AUTO) {
            ESP_LOGW(TAG, "Unexpected SW3 state %d; using auto mode",
                     (int)switch_state);
        }
#if DAP_PATH_AUTO_PREFER_WIRELESS
        attempts[0] = TRANSPORT_PATH_WIRELESS;
        attempts[1] = TRANSPORT_PATH_WIRED;
#else
        attempts[0] = TRANSPORT_PATH_WIRED;
        attempts[1] = TRANSPORT_PATH_WIRELESS;
#endif
        attempt_count = 2U;
    }

    esp_err_t err = ESP_FAIL;
    for (size_t index = 0U; index < attempt_count; ++index) {
        const transport_path_t path = attempts[index];
        ESP_LOGI(TAG, "Starting %s path", transport_path_name(path));
        err = transport_path_start(path);
        if (err == ESP_OK) {
            s_active_path = path;
            ESP_LOGI(TAG, "%s path selected", transport_path_name(path));
            return ESP_OK;
        }

        if ((index + 1U) < attempt_count) {
            ESP_LOGW(TAG, "%s path failed: %s; trying %s path",
                     transport_path_name(path), esp_err_to_name(err),
                     transport_path_name(attempts[index + 1U]));
        } else {
            ESP_LOGE(TAG, "%s path failed: %s", transport_path_name(path),
                     esp_err_to_name(err));
        }
    }
    return err;
}

esp_err_t transport_manager_start(void)
{
    if (s_active_path != TRANSPORT_PATH_NONE) {
        return ESP_ERR_INVALID_STATE;
    }

    sw3_state_t switch_state = DAP_PATH_SW3_AUTO;
#if DAP_PATH_SELECTION_MODE == DAP_PATH_MODE_THREE_STATE
    switch_state = sw3_get_state();
    ESP_LOGI(TAG, "MODESEL three-state startup state: %d",
             (int)switch_state);
#elif DAP_PATH_SELECTION_MODE == DAP_PATH_MODE_TWO_STATE
    bool high_level = false;
    esp_err_t read_err = sw3_read_binary_level(&high_level);
    if (read_err != ESP_OK) {
        ESP_LOGW(TAG, "MODESEL digital read failed: %s; using auto mode",
                 esp_err_to_name(read_err));
        switch_state = SW3_STATE_UNKNOWN;
    } else {
        switch_state = high_level ? DAP_PATH_SW3_WIRELESS
                                  : DAP_PATH_SW3_WIRED;
        ESP_LOGI(TAG, "MODESEL two-state level: %s",
                 high_level ? "high/wireless" : "low/wired");
    }
#elif DAP_PATH_SELECTION_MODE == DAP_PATH_MODE_FORCE_WIRED
    switch_state = DAP_PATH_SW3_WIRED;
    ESP_LOGI(TAG, "Path selection ignores MODESEL: forced wired");
#elif DAP_PATH_SELECTION_MODE == DAP_PATH_MODE_FORCE_WIRELESS
    switch_state = DAP_PATH_SW3_WIRELESS;
    ESP_LOGI(TAG, "Path selection ignores MODESEL: forced wireless");
#elif DAP_PATH_SELECTION_MODE == DAP_PATH_MODE_FORCE_AUTO
    switch_state = DAP_PATH_SW3_AUTO;
    ESP_LOGI(TAG, "Path selection ignores MODESEL: forced auto");
#endif
    return transport_manager_select_and_start(switch_state);
}
