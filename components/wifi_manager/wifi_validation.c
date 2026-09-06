#include "wifi_manager.h"
#include <string.h>

esp_err_t wifi_manager_validate_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t ssid_length = strlen(ssid);
    size_t password_length = strlen(password);
    if (ssid_length == 0 || ssid_length > WIFI_SSID_MAX_BYTES ||
        password_length > WIFI_PASSWORD_MAX_BYTES ||
        (password_length != 0 && password_length < 8)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}
