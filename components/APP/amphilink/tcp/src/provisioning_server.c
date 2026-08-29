#include "provisioning_server.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "device_identity_store.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "flash_user.h"
#include "network_tcp_server.h"
#include "serial.h"
#include "serial_config_store.h"
#include "bsp_pin_config.h"
#include "wifi_manager.h"
#include "wifi_profile_store.h"
#include "wireless_config.h"

#define HTTP_BODY_MAX_SIZE 512U
#define STATIC_CHUNK_SIZE 2048U
#define AUTH_COOKIE_NAME "amphilink_auth"
#define AUTH_TOKEN_BYTES 16U
#define AUTH_TOKEN_HEX_SIZE (AUTH_TOKEN_BYTES * 2U)

typedef struct {
    const char *uri;
    const char *path;
    const char *content_type;
    const char *cache_control;
} static_resource_t;

static const char *TAG = "PROVISIONING_HTTP";
static httpd_handle_t s_server;
static provisioning_server_config_t s_config;
static char s_auth_token[AUTH_TOKEN_HEX_SIZE + 1U];

static void secure_zero(void *data, size_t size)
{
    volatile uint8_t *bytes = data;
    while (size-- > 0U) {
        *bytes++ = 0U;
    }
}

static void clear_json_strings(cJSON *root)
{
    for (cJSON *item = root; item != NULL; item = item->next) {
        if (item->child != NULL) {
            clear_json_strings(item->child);
        }
        if (cJSON_IsString(item) && (item->valuestring != NULL)) {
            secure_zero(item->valuestring, strlen(item->valuestring));
        }
    }
}

static void delete_sensitive_json(cJSON *root)
{
    clear_json_strings(root);
    cJSON_Delete(root);
}

static void clear_snapshot(wifi_profile_snapshot_t *snapshot)
{
    secure_zero(snapshot, sizeof(*snapshot));
}

static const static_resource_t s_static_resources[] = {
    {"/", "index.html", "text/html; charset=utf-8", "no-store"},
    {"/app.v6.css", "app.v2.css", "text/css; charset=utf-8", "no-store"},
    {"/app.v6.js", "app.v3.js", "application/javascript; charset=utf-8",
     "no-store"},
    {"/icon.svg", "icon.svg", "image/svg+xml", "no-store"},
};

static const char *serial_parity_name(serial_parity_t parity)
{
    switch (parity) {
    case SERIAL_PARITY_EVEN:
        return "even";
    case SERIAL_PARITY_ODD:
        return "odd";
    case SERIAL_PARITY_NONE:
    default:
        return "none";
    }
}

static bool serial_parity_from_json(cJSON *item, serial_parity_t *parity)
{
    if (!cJSON_IsString(item) || (item->valuestring == NULL)) {
        return false;
    }
    if (strcmp(item->valuestring, "none") == 0) {
        *parity = SERIAL_PARITY_NONE;
    } else if (strcmp(item->valuestring, "even") == 0) {
        *parity = SERIAL_PARITY_EVEN;
    } else if (strcmp(item->valuestring, "odd") == 0) {
        *parity = SERIAL_PARITY_ODD;
    } else {
        return false;
    }
    return true;
}

static cJSON *serial_json(const serial_config_t *config, uint32_t revision)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "revision", revision);
    cJSON_AddBoolToObject(root, "enabled", config->enabled);
    cJSON_AddNumberToObject(root, "port", SERIAL_TCP_SERVER_PORT);
    cJSON_AddNumberToObject(root, "rx_pin", BSP_PIN_UART_RX);
    cJSON_AddNumberToObject(root, "tx_pin", BSP_PIN_UART_TX);
    cJSON_AddNumberToObject(root, "baudrate", config->baudrate);
    cJSON_AddNumberToObject(root, "data_bits", config->data_bits);
    cJSON_AddStringToObject(root, "parity", serial_parity_name(config->parity));
    cJSON_AddNumberToObject(root, "stop_bits", config->stop_bits);
    cJSON_AddStringToObject(root, "flow_control", "none");
    cJSON_AddBoolToObject(root, "client", serial_has_client());
    return root;
}

static const char *mode_name(wifi_manager_mode_t mode)
{
    switch (mode) {
    case WIFI_MANAGER_MODE_CONNECTING:
        return "connecting";
    case WIFI_MANAGER_MODE_STA:
        return "sta";
    case WIFI_MANAGER_MODE_AP:
        return "ap";
    case WIFI_MANAGER_MODE_STARTING:
    default:
        return "starting";
    }
}

static const char *operation_name(wifi_manager_operation_state_t state)
{
    switch (state) {
    case WIFI_MANAGER_OPERATION_RUNNING:
        return "running";
    case WIFI_MANAGER_OPERATION_SUCCEEDED:
        return "succeeded";
    case WIFI_MANAGER_OPERATION_FAILED:
        return "failed";
    case WIFI_MANAGER_OPERATION_IDLE:
    default:
        return "idle";
    }
}

static const char *auth_name(wifi_auth_mode_t authmode)
{
    switch (authmode) {
    case WIFI_AUTH_OPEN:
        return "open";
    case WIFI_AUTH_WPA2_PSK:
        return "wpa2";
    case WIFI_AUTH_WPA3_PSK:
        return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "wpa2_wpa3";
    case WIFI_AUTH_WPA_PSK:
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "legacy_wpa";
    default:
        return "unsupported";
    }
}

static bool auth_supported(wifi_auth_mode_t authmode)
{
    return (authmode == WIFI_AUTH_OPEN) || (authmode == WIFI_AUTH_WPA_PSK) ||
           (authmode == WIFI_AUTH_WPA_WPA2_PSK) ||
           (authmode == WIFI_AUTH_WPA2_PSK) ||
           (authmode == WIFI_AUTH_WPA3_PSK) ||
           (authmode == WIFI_AUTH_WPA2_WPA3_PSK);
}

static esp_err_t send_json(httpd_req_t *request, const char *status,
                           cJSON *root)
{
    char *body = cJSON_PrintUnformatted(root);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return err;
}

static esp_err_t send_sensitive_json(httpd_req_t *request, const char *status,
                                     cJSON *root)
{
    char *body = cJSON_PrintUnformatted(root);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t body_size = strlen(body);
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(request, body, body_size);
    secure_zero(body, body_size);
    cJSON_free(body);
    return err;
}

static esp_err_t send_error(httpd_req_t *request, const char *status,
                            const char *code)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *error = cJSON_AddObjectToObject(root, "error");
    cJSON_AddStringToObject(error, "code", code);
    esp_err_t result = send_json(request, status, root);
    cJSON_Delete(root);
    return result;
}

static esp_err_t unauthorized(httpd_req_t *request)
{
    return send_error(request, "401 Unauthorized", "unauthorized");
}

static bool request_authenticated(httpd_req_t *request)
{
    size_t length = httpd_req_get_hdr_value_len(request, "Cookie");
    if ((length == 0U) || (length >= 128U)) {
        return false;
    }

    char cookie[128];
    if (httpd_req_get_hdr_value_str(request, "Cookie", cookie,
                                    sizeof(cookie)) != ESP_OK) {
        return false;
    }
    char marker[sizeof(AUTH_COOKIE_NAME) + 2U];
    snprintf(marker, sizeof(marker), "%s=", AUTH_COOKIE_NAME);
    const char *value = cookie;
    while ((value = strstr(value, marker)) != NULL) {
        if ((value == cookie) || (value[-1] == ';') || (value[-1] == ' ')) {
            value += strlen(marker);
            size_t token_length = strcspn(value, ";");
            return (token_length == AUTH_TOKEN_HEX_SIZE) &&
                   (memcmp(value, s_auth_token, AUTH_TOKEN_HEX_SIZE) == 0);
        }
        value += strlen(marker);
    }
    return false;
}

#define REQUIRE_AUTH(request) \
    do { \
        if (!request_authenticated(request)) { \
            return unauthorized(request); \
        } \
    } while (0)

static bool dap_client_active(void)
{
    return (s_config.dap_client_active != NULL) &&
           s_config.dap_client_active(s_config.context);
}

static bool serial_client_active(void)
{
    return (s_config.serial_client_active != NULL) &&
           s_config.serial_client_active(s_config.context);
}

static bool any_client_active(void)
{
    return dap_client_active() || serial_client_active();
}

static void disconnect_clients(void)
{
    if (s_config.disconnect_clients != NULL) {
        s_config.disconnect_clients(s_config.context);
    }
}

static void generate_auth_token(void)
{
    static const char hex[] = "0123456789abcdef";
    uint8_t random_bytes[AUTH_TOKEN_BYTES];
    esp_fill_random(random_bytes, sizeof(random_bytes));
    for (size_t index = 0U; index < sizeof(random_bytes); ++index) {
        s_auth_token[index * 2U] = hex[random_bytes[index] >> 4U];
        s_auth_token[index * 2U + 1U] =
            hex[random_bytes[index] & 0x0FU];
    }
    s_auth_token[AUTH_TOKEN_HEX_SIZE] = '\0';
}

static esp_err_t send_store_error(httpd_req_t *request, esp_err_t error)
{
    if (error == ESP_ERR_INVALID_CRC) {
        return send_error(request, "423 Locked", "credentials_locked");
    }
    if (error == ESP_ERR_NOT_FOUND) {
        return send_error(request, "404 Not Found", "not_found");
    }
    if ((error == ESP_ERR_INVALID_STATE) || (error == ESP_ERR_NO_MEM)) {
        return send_error(request, "409 Conflict",
                          (error == ESP_ERR_NO_MEM) ? "profile_limit"
                                                    : "revision_conflict");
    }
    if (error == ESP_ERR_INVALID_SIZE) {
        return send_error(request, "400 Bad Request", "invalid_size");
    }
    if (error == ESP_ERR_INVALID_ARG) {
        return send_error(request, "400 Bad Request", "invalid_profile");
    }
    return send_error(request, "500 Internal Server Error", "storage_error");
}

static cJSON *receive_json(httpd_req_t *request)
{
    if ((request->content_len <= 0) ||
        ((size_t)request->content_len > HTTP_BODY_MAX_SIZE)) {
        return NULL;
    }
    char body[HTTP_BODY_MAX_SIZE + 1U];
    size_t received = 0U;
    while (received < (size_t)request->content_len) {
        int result = httpd_req_recv(request, body + received,
                                    (size_t)request->content_len - received);
        if (result <= 0) {
            secure_zero(body, sizeof(body));
            return NULL;
        }
        received += (size_t)result;
    }
    body[received] = '\0';
    cJSON *root = cJSON_ParseWithLength(body, received);
    secure_zero(body, sizeof(body));
    return root;
}

static bool json_uint32(cJSON *root, const char *name, uint32_t *value)
{
    if ((root == NULL) || (name == NULL) || (value == NULL)) {
        return false;
    }
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsNumber(item) || (item->valuedouble < 0.0) ||
        (item->valuedouble > 4294967295.0)) {
        return false;
    }
    uint32_t converted = (uint32_t)item->valuedouble;
    if ((double)converted != item->valuedouble) {
        return false;
    }
    *value = converted;
    return true;
}

static esp_err_t login_post(httpd_req_t *request)
{
    cJSON *body = receive_json(request);
    cJSON *password =
        (body == NULL)
            ? NULL
            : cJSON_GetObjectItemCaseSensitive(body, "password");
    if (!cJSON_IsString(password) ||
        (strcmp(password->valuestring, ADMIN_PASSWORD) != 0)) {
        delete_sensitive_json(body);
        return send_error(request, "401 Unauthorized", "invalid_password");
    }
    delete_sensitive_json(body);

    char cookie[96];
    snprintf(cookie, sizeof(cookie), "%s=%s; HttpOnly; SameSite=Strict; Path=/",
             AUTH_COOKIE_NAME, s_auth_token);
    httpd_resp_set_hdr(request, "Set-Cookie", cookie);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "authenticated", true);
    esp_err_t err = send_json(request, "200 OK", root);
    cJSON_Delete(root);
    return err;
}

static bool json_profile(cJSON *root, const char **ssid, const char **password)
{
    cJSON *ssid_item = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    cJSON *password_item = cJSON_GetObjectItemCaseSensitive(root, "password");
    if (!cJSON_IsString(ssid_item) || !cJSON_IsString(password_item)) {
        return false;
    }
    *ssid = ssid_item->valuestring;
    *password = password_item->valuestring;
    return true;
}

static void sync_manager(const wifi_profile_snapshot_t *snapshot)
{
    wifi_manager_credential_t credentials[WIFI_PROFILE_MAX_COUNT] = {0};
    for (size_t index = 0U; index < snapshot->count; ++index) {
        credentials[index].id = snapshot->profiles[index].id;
        snprintf(credentials[index].ssid, sizeof(credentials[index].ssid), "%s",
                 snapshot->profiles[index].ssid);
        snprintf(credentials[index].password,
                 sizeof(credentials[index].password), "%s",
                 snapshot->profiles[index].password);
    }
    (void)wifi_manager_set_credentials(credentials, snapshot->count);
    secure_zero(credentials, sizeof(credentials));
}

static cJSON *snapshot_json(const wifi_profile_snapshot_t *snapshot)
{
    wifi_manager_status_t status = {0};
    (void)wifi_manager_get_status(&status);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "revision", snapshot->revision);
    cJSON_AddBoolToObject(root, "locked", snapshot->locked);
    cJSON *items = cJSON_AddArrayToObject(root, "items");
    for (size_t index = 0U; index < snapshot->count; ++index) {
        const wifi_profile_t *profile = &snapshot->profiles[index];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", profile->id);
        cJSON_AddStringToObject(item, "ssid", profile->ssid);
        cJSON_AddBoolToObject(item, "has_password",
                              profile->password[0] != '\0');
        cJSON_AddNumberToObject(item, "priority", index);
        cJSON_AddBoolToObject(item, "connected",
                              (status.mode == WIFI_MANAGER_MODE_STA) &&
                                  (strcmp(status.ssid, profile->ssid) == 0));
        cJSON_AddItemToArray(items, item);
    }
    return root;
}

static esp_err_t send_snapshot_json(httpd_req_t *request, const char *status,
                                    wifi_profile_snapshot_t *snapshot)
{
    cJSON *root = snapshot_json(snapshot);
    clear_snapshot(snapshot);
    esp_err_t err = send_json(request, status, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t status_get(httpd_req_t *request)
{
    REQUIRE_AUTH(request);
    wifi_manager_status_t status;
    if (wifi_manager_get_status(&status) != ESP_OK) {
        return send_error(request, "503 Service Unavailable", "not_ready");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "mode", mode_name(status.mode));
    cJSON_AddStringToObject(root, "ssid", status.ssid);
    cJSON_AddStringToObject(root, "ip", status.ip);
    cJSON_AddStringToObject(root, "ap_ssid", status.ap_ssid);
    cJSON_AddBoolToObject(root, "scanning", status.scanning);
    cJSON_AddBoolToObject(root, "dap_client", dap_client_active());
    serial_config_t serial_config = {0};
    uint32_t serial_revision = 0U;
    bool serial_enabled =
        (serial_config_store_get(&serial_config, &serial_revision) == ESP_OK) &&
        serial_config.enabled;
    (void)serial_revision;
    cJSON_AddBoolToObject(root, "serial_enabled", serial_enabled);
    cJSON_AddBoolToObject(root, "serial_client", serial_has_client());
    esp_err_t err = send_json(request, "200 OK", root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t serial_get(httpd_req_t *request)
{
    REQUIRE_AUTH(request);
    serial_config_t config;
    uint32_t revision;
    esp_err_t err = serial_config_store_get(&config, &revision);
    if (err != ESP_OK) {
        return send_error(request, "503 Service Unavailable", "not_ready");
    }
    cJSON *root = serial_json(&config, revision);
    err = send_json(request, "200 OK", root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t serial_put(httpd_req_t *request)
{
    REQUIRE_AUTH(request);
    cJSON *body = receive_json(request);
    uint32_t revision;
    uint32_t baudrate;
    uint32_t data_bits;
    uint32_t stop_bits;
    cJSON *enabled_item = (body == NULL) ? NULL
                                         : cJSON_GetObjectItemCaseSensitive(
                                               body, "enabled");
    if (!cJSON_IsBool(enabled_item) || !json_uint32(body, "revision", &revision) ||
        !json_uint32(body, "baudrate", &baudrate) ||
        !json_uint32(body, "data_bits", &data_bits) ||
        !json_uint32(body, "stop_bits", &stop_bits)) {
        delete_sensitive_json(body);
        return send_error(request, "400 Bad Request", "invalid_serial");
    }
    serial_parity_t parity;
    if (!serial_parity_from_json(
            cJSON_GetObjectItemCaseSensitive(body, "parity"), &parity)) {
        delete_sensitive_json(body);
        return send_error(request, "400 Bad Request", "invalid_serial");
    }
    serial_config_t candidate = {
        .enabled = cJSON_IsTrue(enabled_item),
        .baudrate = baudrate,
        .data_bits = (uint8_t)data_bits,
        .parity = parity,
        .stop_bits = (uint8_t)stop_bits,
    };
    if (!serial_config_store_baudrate_allowed(candidate.baudrate) ||
        (candidate.data_bits < 5U) || (candidate.data_bits > 8U) ||
        (candidate.stop_bits < 1U) || (candidate.stop_bits > 2U)) {
        delete_sensitive_json(body);
        return send_error(request, "400 Bad Request", "invalid_serial");
    }

    serial_config_t previous;
    uint32_t current_revision;
    esp_err_t err = serial_config_store_get(&previous, &current_revision);
    if (err != ESP_OK) {
        delete_sensitive_json(body);
        return send_error(request, "503 Service Unavailable", "not_ready");
    }
    if (revision != current_revision) {
        delete_sensitive_json(body);
        return send_error(request, "409 Conflict", "revision_conflict");
    }
    err = serial_apply_config(&candidate);
    if (err != ESP_OK) {
        delete_sensitive_json(body);
        return send_error(request, "409 Conflict", "invalid_serial");
    }
    uint32_t next_revision;
    err = serial_config_store_set(&candidate, revision, &next_revision);
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_STATE) {
            serial_config_t latest;
            uint32_t latest_revision;
            if (serial_config_store_get(&latest, &latest_revision) == ESP_OK) {
                (void)serial_apply_config(&latest);
            }
            delete_sensitive_json(body);
            return send_error(request, "409 Conflict", "revision_conflict");
        }
        (void)serial_apply_config(&previous);
        delete_sensitive_json(body);
        return send_error(request, "500 Internal Server Error", "storage_error");
    }
    delete_sensitive_json(body);
    cJSON *root = serial_json(&candidate, next_revision);
    err = send_json(request, "200 OK", root);
    cJSON_Delete(root);
    return err;
}

static cJSON *device_identity_json(const device_identity_t *identity)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", identity->name);
    cJSON_AddNumberToObject(root, "id", identity->id);
    return root;
}

static esp_err_t device_get(httpd_req_t *request)
{
    REQUIRE_AUTH(request);
    device_identity_t identity;
    esp_err_t err = device_identity_store_get(&identity);
    if (err != ESP_OK) {
        return send_error(request, "503 Service Unavailable", "not_ready");
    }
    cJSON *root = device_identity_json(&identity);
    err = send_json(request, "200 OK", root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t device_put(httpd_req_t *request)
{
    REQUIRE_AUTH(request);
    cJSON *body = receive_json(request);
    cJSON *name_item =
        (body == NULL) ? NULL : cJSON_GetObjectItemCaseSensitive(body, "name");
    uint32_t id;
    if (!cJSON_IsString(name_item) || !json_uint32(body, "id", &id)) {
        delete_sensitive_json(body);
        return send_error(request, "400 Bad Request", "invalid_json");
    }

    device_identity_t identity;
    esp_err_t err =
        device_identity_store_set(name_item->valuestring, id, &identity);
    delete_sensitive_json(body);
    if (err == ESP_ERR_INVALID_ARG) {
        return send_error(request, "400 Bad Request", "invalid_identity");
    }
    if (err != ESP_OK) {
        return send_error(request, "500 Internal Server Error",
                          "storage_error");
    }

    if (s_config.identity_changed != NULL) {
        s_config.identity_changed(&identity, s_config.context);
    }
    cJSON *root = device_identity_json(&identity);
    err = send_json(request, "200 OK", root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t networks_get(httpd_req_t *request)
{
    REQUIRE_AUTH(request);
    wifi_profile_snapshot_t snapshot = {0};
    esp_err_t err = wifi_profile_store_get(&snapshot);
    if (err != ESP_OK) {
        return send_store_error(request, err);
    }
    return send_snapshot_json(request, "200 OK", &snapshot);
}

static bool password_uri_id(const char *uri, uint32_t *id)
{
    static const char prefix[] = "/api/v1/networks/";
    static const char suffix[] = "/password";
    if ((uri == NULL) || (strncmp(uri, prefix, sizeof(prefix) - 1U) != 0)) {
        return false;
    }
    const char *value = uri + sizeof(prefix) - 1U;
    char *end;
    unsigned long converted = strtoul(value, &end, 10);
    if ((converted == 0UL) || (converted > UINT32_MAX) ||
        (strcmp(end, suffix) != 0)) {
        return false;
    }
    *id = (uint32_t)converted;
    return true;
}

static esp_err_t network_password_get(httpd_req_t *request)
{
    REQUIRE_AUTH(request);
    uint32_t id;
    if (!password_uri_id(request->uri, &id)) {
        return send_error(request, "400 Bad Request", "invalid_id");
    }
    char password[WIFI_PROFILE_PASSWORD_SIZE] = {0};
    esp_err_t err = wifi_profile_store_get_password(id, password,
                                                     sizeof(password));
    if (err != ESP_OK) {
        secure_zero(password, sizeof(password));
        return send_store_error(request, err);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *item = (root == NULL)
                      ? NULL
                      : cJSON_AddStringToObject(root, "password", password);
    secure_zero(password, sizeof(password));
    if (item == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    err = send_sensitive_json(request, "200 OK", root);
    delete_sensitive_json(root);
    return err;
}

static esp_err_t networks_reset_post(httpd_req_t *request)
{
    REQUIRE_AUTH(request);
    cJSON *body = receive_json(request);
    cJSON *confirm = (body == NULL)
                         ? NULL
                         : cJSON_GetObjectItemCaseSensitive(body, "confirm");
    if (!cJSON_IsTrue(confirm)) {
        delete_sensitive_json(body);
        return send_error(request, "400 Bad Request", "confirmation_required");
    }
    delete_sensitive_json(body);
    wifi_profile_snapshot_t snapshot = {0};
    esp_err_t err = wifi_profile_store_reset(&snapshot);
    if (err != ESP_OK) {
        clear_snapshot(&snapshot);
        return send_store_error(request, err);
    }
    sync_manager(&snapshot);
    return send_snapshot_json(request, "200 OK", &snapshot);
}

static esp_err_t networks_post(httpd_req_t *request)
{
    REQUIRE_AUTH(request);
    cJSON *body = receive_json(request);
    const char *ssid;
    const char *password;
    uint32_t revision;
    if ((body == NULL) || !json_uint32(body, "revision", &revision) ||
        !json_profile(body, &ssid, &password)) {
        delete_sensitive_json(body);
        return send_error(request, "400 Bad Request", "invalid_json");
    }
    wifi_profile_snapshot_t snapshot = {0};
    esp_err_t err = wifi_profile_store_add(revision, ssid, password, &snapshot);
    delete_sensitive_json(body);
    if (err != ESP_OK) {
        clear_snapshot(&snapshot);
        return send_store_error(request, err);
    }
    sync_manager(&snapshot);
    return send_snapshot_json(request, "201 Created", &snapshot);
}

static bool uri_id(const char *uri, uint32_t *id)
{
    const char *separator = strrchr(uri, '/');
    if ((separator == NULL) || (separator[1] == '\0')) {
        return false;
    }
    char *end;
    unsigned long value = strtoul(separator + 1, &end, 10);
    if ((*end != '\0') || (value == 0UL) || (value > UINT32_MAX)) {
        return false;
    }
    *id = (uint32_t)value;
    return true;
}

static esp_err_t network_put(httpd_req_t *request)
{
    REQUIRE_AUTH(request);
    uint32_t id;
    cJSON *body = receive_json(request);
    const char *ssid;
    const char *password = NULL;
    uint32_t revision;
    cJSON *ssid_item =
        (body == NULL) ? NULL : cJSON_GetObjectItemCaseSensitive(body, "ssid");
    cJSON *password_item = (body == NULL)
                               ? NULL
                               : cJSON_GetObjectItemCaseSensitive(body,
                                                                  "password");
    if (!uri_id(request->uri, &id) || (body == NULL) ||
        !json_uint32(body, "revision", &revision) ||
        !cJSON_IsString(ssid_item) ||
        ((password_item != NULL) && !cJSON_IsString(password_item))) {
        delete_sensitive_json(body);
        return send_error(request, "400 Bad Request", "invalid_json");
    }
    ssid = ssid_item->valuestring;
    if (password_item != NULL) {
        password = password_item->valuestring;
    }
    wifi_profile_snapshot_t snapshot = {0};
    esp_err_t err =
        wifi_profile_store_update(revision, id, ssid, password, &snapshot);
    delete_sensitive_json(body);
    if (err != ESP_OK) {
        clear_snapshot(&snapshot);
        return send_store_error(request, err);
    }
    sync_manager(&snapshot);
    return send_snapshot_json(request, "200 OK", &snapshot);
}

static esp_err_t network_delete(httpd_req_t *request)
{
    REQUIRE_AUTH(request);
    uint32_t id;
    cJSON *body = receive_json(request);
    uint32_t revision;
    if (!uri_id(request->uri, &id) || (body == NULL) ||
        !json_uint32(body, "revision", &revision)) {
        delete_sensitive_json(body);
        return send_error(request, "400 Bad Request", "invalid_json");
    }
    wifi_profile_snapshot_t snapshot = {0};
    esp_err_t err = wifi_profile_store_remove(revision, id, &snapshot);
    delete_sensitive_json(body);
    if (err != ESP_OK) {
        clear_snapshot(&snapshot);
        return send_store_error(request, err);
    }
    sync_manager(&snapshot);
    return send_snapshot_json(request, "200 OK", &snapshot);
}

static esp_err_t order_put(httpd_req_t *request)
{
    REQUIRE_AUTH(request);
    cJSON *body = receive_json(request);
    uint32_t revision;
    cJSON *ids =
        (body == NULL) ? NULL : cJSON_GetObjectItemCaseSensitive(body, "ids");
    if ((body == NULL) || !json_uint32(body, "revision", &revision) ||
        !cJSON_IsArray(ids) ||
        (cJSON_GetArraySize(ids) > WIFI_PROFILE_MAX_COUNT)) {
        delete_sensitive_json(body);
        return send_error(request, "400 Bad Request", "invalid_json");
    }
    uint32_t ordered[WIFI_PROFILE_MAX_COUNT];
    size_t count = (size_t)cJSON_GetArraySize(ids);
    for (size_t index = 0U; index < count; ++index) {
        cJSON *item = cJSON_GetArrayItem(ids, (int)index);
        if (!cJSON_IsNumber(item) || (item->valuedouble <= 0.0) ||
            (item->valuedouble > UINT32_MAX)) {
            delete_sensitive_json(body);
            return send_error(request, "400 Bad Request", "invalid_json");
        }
        ordered[index] = (uint32_t)item->valuedouble;
    }
    wifi_profile_snapshot_t snapshot = {0};
    esp_err_t err =
        wifi_profile_store_reorder(revision, ordered, count, &snapshot);
    delete_sensitive_json(body);
    if (err != ESP_OK) {
        clear_snapshot(&snapshot);
        return send_store_error(request, err);
    }
    sync_manager(&snapshot);
    return send_snapshot_json(request, "200 OK", &snapshot);
}

static esp_err_t scan_post(httpd_req_t *request)
{
    REQUIRE_AUTH(request);
    if (any_client_active()) {
        return send_error(request, "409 Conflict", "dap_client_active");
    }
    uint32_t operation_id;
    esp_err_t err = wifi_manager_scan_start(&operation_id);
    if (err != ESP_OK) {
        return send_error(request, "409 Conflict", "operation_busy");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "operation_id", operation_id);
    err = send_json(request, "202 Accepted", root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t scan_get(httpd_req_t *request)
{
    REQUIRE_AUTH(request);
    uint32_t operation_id;
    if (!uri_id(request->uri, &operation_id)) {
        return send_error(request, "400 Bad Request", "invalid_id");
    }
    wifi_manager_scan_result_t result;
    if (wifi_manager_scan_get(operation_id, &result) != ESP_OK) {
        return send_error(request, "404 Not Found", "not_found");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "operation_id", result.operation_id);
    cJSON_AddStringToObject(root, "state", operation_name(result.state));
    cJSON_AddNumberToObject(root, "error", result.error);
    cJSON *items = cJSON_AddArrayToObject(root, "items");
    wifi_profile_snapshot_t snapshot = {0};
    (void)wifi_profile_store_get(&snapshot);
    for (size_t index = 0U; index < result.count; ++index) {
        const wifi_manager_scan_record_t *record = &result.records[index];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", record->ssid);
        cJSON_AddNumberToObject(item, "rssi", record->rssi);
        cJSON_AddNumberToObject(item, "channel", record->channel);
        cJSON_AddStringToObject(item, "auth", auth_name(record->authmode));
        cJSON_AddBoolToObject(item, "supported",
                              auth_supported(record->authmode));
        bool saved = false;
        for (size_t profile = 0U; profile < snapshot.count; ++profile) {
            if (strcmp(snapshot.profiles[profile].ssid, record->ssid) == 0) {
                saved = true;
                break;
            }
        }
        cJSON_AddBoolToObject(item, "saved", saved);
        cJSON_AddItemToArray(items, item);
    }
    clear_snapshot(&snapshot);
    esp_err_t err = send_json(request, "200 OK", root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t connect_post(httpd_req_t *request)
{
    REQUIRE_AUTH(request);
    cJSON *body = receive_json(request);
    if (body == NULL) {
        return send_error(request, "400 Bad Request", "invalid_profile");
    }

    wifi_profile_snapshot_t snapshot = {0};
    esp_err_t err = wifi_profile_store_get(&snapshot);
    if (err != ESP_OK) {
        delete_sensitive_json(body);
        clear_snapshot(&snapshot);
        return send_store_error(request, err);
    }
    if (snapshot.locked) {
        delete_sensitive_json(body);
        clear_snapshot(&snapshot);
        return send_error(request, "423 Locked", "credentials_locked");
    }

    wifi_manager_credential_t credential = {0};
    uint32_t profile_id;
    cJSON *profile_id_item =
        cJSON_GetObjectItemCaseSensitive(body, "profile_id");
    if (profile_id_item != NULL) {
        if (!json_uint32(body, "profile_id", &profile_id) ||
            (profile_id == 0U) ||
            (cJSON_GetObjectItemCaseSensitive(body, "ssid") != NULL) ||
            (cJSON_GetObjectItemCaseSensitive(body, "password") != NULL)) {
            delete_sensitive_json(body);
            clear_snapshot(&snapshot);
            return send_error(request, "400 Bad Request", "invalid_profile");
        }
        const wifi_profile_t *profile = NULL;
        for (size_t index = 0U; index < snapshot.count; ++index) {
            if (snapshot.profiles[index].id == profile_id) {
                profile = &snapshot.profiles[index];
                break;
            }
        }
        if (profile == NULL) {
            delete_sensitive_json(body);
            clear_snapshot(&snapshot);
            return send_error(request, "404 Not Found", "not_found");
        }
        credential.id = profile->id;
        snprintf(credential.ssid, sizeof(credential.ssid), "%s",
                 profile->ssid);
        snprintf(credential.password, sizeof(credential.password), "%s",
                 profile->password);
    } else {
        const char *ssid;
        const char *password;
        if (!json_profile(body, &ssid, &password) ||
            (wifi_profile_validate(ssid, password) != ESP_OK)) {
            delete_sensitive_json(body);
            clear_snapshot(&snapshot);
            return send_error(request, "400 Bad Request", "invalid_profile");
        }
        bool existing = false;
        for (size_t index = 0U; index < snapshot.count; ++index) {
            if (strcmp(snapshot.profiles[index].ssid, ssid) == 0) {
                existing = true;
                break;
            }
        }
        if (!existing && (snapshot.count >= WIFI_PROFILE_MAX_COUNT)) {
            delete_sensitive_json(body);
            clear_snapshot(&snapshot);
            return send_error(request, "409 Conflict", "profile_limit");
        }
        snprintf(credential.ssid, sizeof(credential.ssid), "%s", ssid);
        snprintf(credential.password, sizeof(credential.password), "%s",
                 password);
    }

    clear_snapshot(&snapshot);
    uint32_t operation_id;
    err = wifi_manager_connect_start(&credential, &operation_id);
    secure_zero(&credential, sizeof(credential));
    delete_sensitive_json(body);
    if (err != ESP_OK) {
        return send_error(request, "409 Conflict", "operation_busy");
    }
    disconnect_clients();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "operation_id", operation_id);
    err = send_json(request, "202 Accepted", root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t connect_get(httpd_req_t *request)
{
    REQUIRE_AUTH(request);
    uint32_t operation_id;
    if (!uri_id(request->uri, &operation_id)) {
        return send_error(request, "400 Bad Request", "invalid_id");
    }
    wifi_manager_connect_result_t result;
    if (wifi_manager_connect_get(operation_id, &result) != ESP_OK) {
        return send_error(request, "404 Not Found", "not_found");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "operation_id", result.operation_id);
    cJSON_AddStringToObject(root, "state", operation_name(result.state));
    cJSON_AddNumberToObject(root, "error", result.error);
    cJSON_AddStringToObject(root, "ssid", result.ssid);
    esp_err_t err = send_json(request, "200 OK", root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t static_get(httpd_req_t *request)
{
    const static_resource_t *resource = NULL;
    for (size_t index = 0U;
         index < sizeof(s_static_resources) / sizeof(s_static_resources[0]);
         ++index) {
        if (strcmp(request->uri, s_static_resources[index].uri) == 0) {
            resource = &s_static_resources[index];
            break;
        }
    }
    if (resource == NULL) {
        return send_error(request, "404 Not Found", "not_found");
    }
    size_t file_size;
    if (flash_user_file_get_size(FLASH_USER_PARTITION_WEBPAGE_VFS,
                                 resource->path, &file_size) != ESP_OK) {
        return send_error(request, "503 Service Unavailable",
                          "web_assets_missing");
    }
    httpd_resp_set_type(request, resource->content_type);
    httpd_resp_set_hdr(request, "Cache-Control", resource->cache_control);
    uint8_t chunk[STATIC_CHUNK_SIZE];
    for (size_t offset = 0U; offset < file_size;) {
        size_t bytes_read;
        esp_err_t err =
            flash_user_file_read_at(FLASH_USER_PARTITION_WEBPAGE_VFS,
                                    resource->path, offset, chunk,
                                    sizeof(chunk), &bytes_read);
        if ((err != ESP_OK) || (bytes_read == 0U)) {
            (void)httpd_resp_send_chunk(request, NULL, 0);
            return ESP_FAIL;
        }
        err = httpd_resp_send_chunk(request, (const char *)chunk,
                                    (ssize_t)bytes_read);
        if (err != ESP_OK) {
            return err;
        }
        offset += bytes_read;
    }
    return httpd_resp_send_chunk(request, NULL, 0);
}

static esp_err_t register_handler(const char *uri, httpd_method_t method,
                                  esp_err_t (*handler)(httpd_req_t *))
{
    const httpd_uri_t descriptor = {
        .uri = uri,
        .method = method,
        .handler = handler,
    };
    return httpd_register_uri_handler(s_server, &descriptor);
}

esp_err_t provisioning_server_start(const provisioning_server_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_server != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_config = *config;
    generate_auth_token();
    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.server_port = 80U;
    http_config.stack_size = 6144U;
    http_config.core_id = 0;
    http_config.max_uri_handlers = 22U;
    http_config.lru_purge_enable = true;
    http_config.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t err = httpd_start(&s_server, &http_config);
    if (err != ESP_OK) {
        memset(&s_config, 0, sizeof(s_config));
        return err;
    }

    err = register_handler("/api/v1/login", HTTP_POST, login_post);
    if (err == ESP_OK) {
        err = register_handler("/api/v1/status", HTTP_GET, status_get);
    }
    if (err == ESP_OK) {
        err = register_handler("/api/v1/device", HTTP_GET, device_get);
    }
    if (err == ESP_OK) {
        err = register_handler("/api/v1/device", HTTP_PUT, device_put);
    }
    if (err == ESP_OK) {
        err = register_handler("/api/v1/serial", HTTP_GET, serial_get);
    }
    if (err == ESP_OK) {
        err = register_handler("/api/v1/serial", HTTP_PUT, serial_put);
    }
    if (err == ESP_OK) {
        err = register_handler("/api/v1/networks", HTTP_GET, networks_get);
    }
    if (err == ESP_OK) {
        err = register_handler("/api/v1/networks", HTTP_POST, networks_post);
    }
    if (err == ESP_OK) {
        err = register_handler("/api/v1/networks/reset", HTTP_POST,
                               networks_reset_post);
    }
    if (err == ESP_OK) {
        err = register_handler("/api/v1/networks/order", HTTP_PUT, order_put);
    }
    if (err == ESP_OK) {
        err = register_handler("/api/v1/networks/*", HTTP_GET,
                               network_password_get);
    }
    if (err == ESP_OK) {
        err = register_handler("/api/v1/networks/*", HTTP_PUT, network_put);
    }
    if (err == ESP_OK) {
        err =
            register_handler("/api/v1/networks/*", HTTP_DELETE, network_delete);
    }
    if (err == ESP_OK) {
        err = register_handler("/api/v1/scan", HTTP_POST, scan_post);
    }
    if (err == ESP_OK) {
        err = register_handler("/api/v1/scan/*", HTTP_GET, scan_get);
    }
    if (err == ESP_OK) {
        err = register_handler("/api/v1/connect", HTTP_POST, connect_post);
    }
    if (err == ESP_OK) {
        err = register_handler("/api/v1/connect/*", HTTP_GET, connect_get);
    }
    for (size_t index = 0U;
         (err == ESP_OK) &&
         (index < sizeof(s_static_resources) / sizeof(s_static_resources[0]));
         ++index) {
        err = register_handler(s_static_resources[index].uri, HTTP_GET,
                               static_get);
    }
    if (err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        memset(&s_config, 0, sizeof(s_config));
        return err;
    }
    ESP_LOGI(TAG, "Provisioning portal listening on HTTP port 80");
    return ESP_OK;
}

esp_err_t provisioning_server_stop(void)
{
    if (s_server == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = httpd_stop(s_server);
    if (err == ESP_OK) {
        s_server = NULL;
        memset(&s_config, 0, sizeof(s_config));
    }
    return err;
}
