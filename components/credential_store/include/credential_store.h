#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// Call once before any credential access. The application explicitly chooses
// whether first boot may permanently provision the configured HMAC eFuse slot.
// Existing NVS is never erased automatically; recovery belongs to the app.
esp_err_t credential_store_init(bool allow_key_provisioning);
void credential_store_clear_secret(void *buffer, size_t size);
esp_err_t credential_store_forget(const char *namespace_name);
