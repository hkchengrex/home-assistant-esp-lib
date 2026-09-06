#include "wifi_manager.h"
#include <stdio.h>
#include <string.h>
#include "credential_store.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_DISCONNECTED_BIT BIT1
#define WIFI_STOPPED_BIT BIT2
#define WIFI_NVS_NAMESPACE "wifi_cfg"
#define WIFI_NVS_SSID_KEY "ssid"
#define WIFI_NVS_PASSWORD_KEY "password"

static EventGroupHandle_t s_events;
static SemaphoreHandle_t s_lock;
static esp_netif_t *s_interface;
static bool s_started;
static bool s_pending;
static int64_t s_retry_at;
static unsigned s_backoff_seconds = 1;

static esp_err_t load_credentials(char *ssid, size_t ssid_capacity,
                                  char *password, size_t password_capacity)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (error != ESP_OK) {
        return error;
    }
    size_t ssid_length = ssid_capacity;
    size_t password_length = password_capacity;
    error = nvs_get_str(handle, WIFI_NVS_SSID_KEY, ssid, &ssid_length);
    if (error == ESP_OK) {
        error = nvs_get_str(handle, WIFI_NVS_PASSWORD_KEY, password,
                            &password_length);
    }
    nvs_close(handle);
    return error;
}

static esp_err_t save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_set_str(handle, WIFI_NVS_SSID_KEY, ssid);
    if (error == ESP_OK) {
        error = nvs_set_str(handle, WIFI_NVS_PASSWORD_KEY, password);
    }
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
        xEventGroupSetBits(s_events, WIFI_DISCONNECTED_BIT);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_STOP) {
        xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
        xEventGroupSetBits(s_events, WIFI_STOPPED_BIT);
    }
}

static void reconnect_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        EventBits_t bits = xEventGroupGetBits(s_events);
        if (bits & WIFI_CONNECTED_BIT) {
            s_backoff_seconds = 1;
            s_pending = false;
            xEventGroupClearBits(s_events, WIFI_DISCONNECTED_BIT);
        } else if (s_started) {
            if (bits & WIFI_DISCONNECTED_BIT) {
                xEventGroupClearBits(s_events, WIFI_DISCONNECTED_BIT);
                s_pending = true;
                s_retry_at = esp_timer_get_time() + s_backoff_seconds * 1000000LL;
                s_backoff_seconds = s_backoff_seconds < 16 ? s_backoff_seconds * 2 : 30;
            }
            if (s_pending && esp_timer_get_time() >= s_retry_at) {
                s_pending = false;
                if (esp_wifi_connect() != ESP_OK) {
                    xEventGroupSetBits(s_events, WIFI_DISCONNECTED_BIT);
                }
            }
        }
        xSemaphoreGive(s_lock);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// Called with the worker lock held. The STOP event fences previous driver
// events before a replacement can clear bits and start a new attempt.
static esp_err_t stop_locked(void)
{
    if (s_started) {
        xEventGroupClearBits(s_events, WIFI_STOPPED_BIT);
        esp_err_t error = esp_wifi_stop();
        if (error != ESP_OK) return error;
        if (!(xEventGroupWaitBits(s_events, WIFI_STOPPED_BIT, pdFALSE, pdFALSE,
                                  pdMS_TO_TICKS(5000)) & WIFI_STOPPED_BIT)) {
            return ESP_ERR_TIMEOUT;
        }
    }
    s_started = false;
    s_pending = false;
    xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT | WIFI_DISCONNECTED_BIT);
    return ESP_OK;
}

static esp_err_t start_credentials(const char *ssid, const char *password)
{
    esp_err_t error = wifi_manager_validate_credentials(ssid, password);
    if (error != ESP_OK) return error;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    error = stop_locked();
    if (error == ESP_OK) {
        wifi_config_t config = {0};
        memcpy(config.sta.ssid, ssid, strlen(ssid));
        memcpy(config.sta.password, password, strlen(password));
        config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        config.sta.threshold.authmode = WIFI_AUTH_OPEN;
        error = esp_wifi_set_config(WIFI_IF_STA, &config);
        credential_store_clear_secret(&config, sizeof(config));
        if (error == ESP_OK) error = esp_wifi_start();
        if (error == ESP_OK) {
            s_started = true;
            s_pending = true;
            s_retry_at = 0;
            s_backoff_seconds = 1;
        }
    }
    xSemaphoreGive(s_lock);
    return error;
}

esp_err_t wifi_manager_initialize(const char *hostname)
{
    if (s_events != NULL) return ESP_ERR_INVALID_STATE;
    if (hostname == NULL || hostname[0] == '\0') return ESP_ERR_INVALID_ARG;
    s_interface = esp_netif_create_default_wifi_sta();
    s_events = xEventGroupCreate();
    s_lock = xSemaphoreCreateMutex();
    if (!s_interface || !s_events || !s_lock) return ESP_ERR_NO_MEM;
    esp_err_t error = esp_netif_set_hostname(s_interface, hostname);
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    if (error == ESP_OK) error = esp_wifi_init(&config);
    if (error == ESP_OK) error = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    if (error == ESP_OK) error = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);
    if (error == ESP_OK) error = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (error == ESP_OK) error = esp_wifi_set_mode(WIFI_MODE_STA);
    if (error == ESP_OK && xTaskCreate(reconnect_task, "wifi_reconnect", 3072, NULL, 5, NULL) != pdPASS) {
        error = ESP_ERR_NO_MEM;
    }
    return error;
}

esp_err_t wifi_manager_start_saved(void)
{
    char ssid[WIFI_SSID_MAX_BYTES + 1] = {0};
    char password[WIFI_PASSWORD_MAX_BYTES + 1] = {0};
    esp_err_t error = load_credentials(ssid, sizeof(ssid), password, sizeof(password));
    if (error == ESP_OK) error = start_credentials(ssid, password);
    credential_store_clear_secret(password, sizeof(password));
    return error;
}

esp_err_t wifi_manager_test_and_save(const char *ssid, const char *password)
{
    esp_err_t error = wifi_manager_validate_credentials(ssid, password);
    if (error != ESP_OK) return error;
    error = start_credentials(ssid, password);
    if (error == ESP_OK) {
        EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_CONNECTED_BIT,
                                             pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
        error = (bits & WIFI_CONNECTED_BIT) ? save_credentials(ssid, password) : ESP_ERR_TIMEOUT;
    }
    if (error != ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        esp_err_t stop_error = stop_locked();
        xSemaphoreGive(s_lock);
        if (stop_error == ESP_OK) (void)wifi_manager_start_saved();
    }
    return error;
}

esp_err_t wifi_manager_forget(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t error = stop_locked();
    if (error == ESP_OK) error = credential_store_forget(WIFI_NVS_NAMESPACE);
    xSemaphoreGive(s_lock);
    return error;
}

bool wifi_manager_is_connected(void)
{
    return s_events && (xEventGroupGetBits(s_events) & WIFI_CONNECTED_BIT);
}

void wifi_manager_ip(char *buffer, size_t capacity)
{
    esp_netif_ip_info_t ip;
    if (wifi_manager_is_connected() && esp_netif_get_ip_info(s_interface, &ip) == ESP_OK) {
        snprintf(buffer, capacity, IPSTR, IP2STR(&ip.ip));
    } else {
        snprintf(buffer, capacity, "not-connected");
    }
}
