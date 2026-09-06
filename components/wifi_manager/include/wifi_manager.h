#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define WIFI_SSID_MAX_BYTES 32
#define WIFI_PASSWORD_MAX_BYTES 63

// Singleton station, process lifetime. App initializes netif and default event
// loop first. Lifecycle calls must come from one controlling task (not events).
esp_err_t wifi_manager_initialize(const char *hostname);
esp_err_t wifi_manager_validate_credentials(const char *ssid, const char *password);
// Starts asynchronously, retrying indefinitely with capped exponential backoff.
esp_err_t wifi_manager_start_saved(void);
// Bounded 30s test. Saves only on success; restores saved settings on failure.
esp_err_t wifi_manager_test_and_save(const char *ssid, const char *password);
esp_err_t wifi_manager_forget(void);
bool wifi_manager_is_connected(void);
void wifi_manager_ip(char *buffer, size_t capacity);
