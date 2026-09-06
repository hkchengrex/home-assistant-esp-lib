#include "credential_store.h"
#include "wifi_manager.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"

void app_main(void)
{
    // This example deliberately requires a previously provisioned HMAC key.
    // A product may explicitly choose true here to allow first-boot eFuse
    // programming. Never change an existing device's key slot casually.
    esp_err_t error = credential_store_init(false);
    if (error != ESP_OK) {
        ESP_LOGE("wifi_example", "Encrypted storage unavailable: %s", esp_err_to_name(error));
        return;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(wifi_manager_initialize("ha-esp-example"));
    error = wifi_manager_start_saved();
    if (error != ESP_OK) {
        ESP_LOGW("wifi_example", "No usable saved network: %s", esp_err_to_name(error));
    }
    // Your own provisioning interface can call wifi_manager_test_and_save().
    // The manager's worker continues reconnecting after app_main returns.
}
