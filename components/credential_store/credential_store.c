#include "credential_store.h"
#include "sdkconfig.h"
#include "esp_efuse.h"
#include "nvs.h"
#include "nvs_flash.h"

#if !CONFIG_NVS_ENCRYPTION || !CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC
#error "credential_store requires HMAC-backed NVS encryption; plaintext fallback is forbidden"
#endif

esp_err_t credential_store_init(bool allow_key_provisioning)
{
    esp_efuse_block_t block = EFUSE_BLK_KEY0 + CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID;
    if (!allow_key_provisioning &&
        esp_efuse_get_key_purpose(block) != ESP_EFUSE_KEY_PURPOSE_HMAC_UP) {
        return ESP_ERR_INVALID_STATE;
    }
    return nvs_flash_init();
}

void credential_store_clear_secret(void *buffer, size_t size)
{
    volatile unsigned char *cursor = buffer;
    while (size-- > 0) {
        *cursor++ = 0;
    }
}

esp_err_t credential_store_forget(const char *namespace_name)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(namespace_name, NVS_READWRITE, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_erase_all(handle);
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}
