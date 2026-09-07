#include "mqtt_connection.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "credential_store.h"
#include "esp_event.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define MQTT_CONNECTED_BIT BIT0
#define MQTT_FAILED_BIT BIT1
#define MQTT_CONNECT_TIMEOUT_MS 20000
#define MQTT_NVS_NAMESPACE "mqtt_cfg"

typedef struct {
    char host[MQTT_HOST_MAX_BYTES + 1];
    char username[MQTT_USERNAME_MAX_BYTES + 1];
    char password[MQTT_PASSWORD_MAX_BYTES + 1];
    uint16_t port;
} saved_mqtt_config_t;

static EventGroupHandle_t s_mqtt_events;
static SemaphoreHandle_t s_lock;
static esp_mqtt_client_handle_t s_client;
static saved_mqtt_config_t s_config;
static mqtt_connection_options_t s_options;
static atomic_uint s_generation;
static char s_incoming[1024];
static size_t s_received, s_expected;
static bool s_retained;

static void receive_message(esp_mqtt_event_handle_t event)
{
    if (!s_options.on_message) return;
    if (event->current_data_offset == 0) {
        s_received = s_expected = 0;
        if (!event->topic || event->topic_len != (int)strlen(s_options.command_topic) ||
            memcmp(event->topic, s_options.command_topic, event->topic_len) ||
            event->total_data_len <= 0 || event->total_data_len >= (int)sizeof(s_incoming)) return;
        s_expected = event->total_data_len;
        s_retained = event->retain;
    }
    if (!s_expected) return;
    if (!event->data || event->data_len <= 0 ||
        event->current_data_offset != (int)s_received ||
        event->total_data_len != (int)s_expected ||
        (size_t)event->data_len > s_expected - s_received) {
        s_received = s_expected = 0;
        return;
    }
    memcpy(s_incoming + s_received, event->data, event->data_len);
    s_received += event->data_len;
    if (s_received == s_expected) {
        s_incoming[s_received] = 0;
        // Embedded NUL would make parsers disagree about message length.
        if (!memchr(s_incoming, 0, s_received))
            s_options.on_message(s_incoming, s_received, s_retained, s_options.message_context);
        s_received = s_expected = 0;
    }
}

static esp_err_t load_config(saved_mqtt_config_t *configuration)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(MQTT_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (error != ESP_OK) {
        return error;
    }

    size_t host_length = sizeof(configuration->host);
    size_t username_length = sizeof(configuration->username);
    size_t password_length = sizeof(configuration->password);
    error = nvs_get_str(handle, "host", configuration->host, &host_length);
    if (error == ESP_OK) {
        error = nvs_get_u16(handle, "port", &configuration->port);
    }
    if (error == ESP_OK) {
        error = nvs_get_str(handle, "username", configuration->username,
                            &username_length);
    }
    if (error == ESP_OK) {
        error = nvs_get_str(handle, "password", configuration->password,
                            &password_length);
    }
    nvs_close(handle);
    return error;
}

static esp_err_t save_config(const saved_mqtt_config_t *configuration)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(MQTT_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }

    error = nvs_set_str(handle, "host", configuration->host);
    if (error == ESP_OK) {
        error = nvs_set_u16(handle, "port", configuration->port);
    }
    if (error == ESP_OK) {
        error = nvs_set_str(handle, "username", configuration->username);
    }
    if (error == ESP_OK) {
        error = nvs_set_str(handle, "password", configuration->password);
    }
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}

// No application mutex here: stop() waits for the MQTT task to exit. Taking
// s_lock in this callback would deadlock a simultaneous stop/publish operation.
static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t event = data;
    if (id == MQTT_EVENT_CONNECTED) {
        xEventGroupClearBits(s_mqtt_events, MQTT_FAILED_BIT);
        esp_mqtt_client_enqueue(event->client, s_options.availability_topic,
                                "online", 0, 1, true, true);
        s_received = s_expected = 0;
        if (s_options.command_topic && esp_mqtt_client_subscribe(event->client, s_options.command_topic, 1) < 0) {
            xEventGroupSetBits(s_mqtt_events, MQTT_FAILED_BIT);
            esp_mqtt_client_disconnect(event->client);
            return;
        }
        atomic_fetch_add(&s_generation, 1);
        xEventGroupSetBits(s_mqtt_events, MQTT_CONNECTED_BIT);
    } else if (id == MQTT_EVENT_DATA) {
        receive_message(event);
    } else if (id == MQTT_EVENT_DISCONNECTED) {
        s_received = s_expected = 0;
        xEventGroupClearBits(s_mqtt_events, MQTT_CONNECTED_BIT);
    } else if (id == MQTT_EVENT_ERROR) {
        xEventGroupSetBits(s_mqtt_events, MQTT_FAILED_BIT);
    }
}

static void stop_client(void)
{
    if (s_client != NULL) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    xEventGroupClearBits(s_mqtt_events, MQTT_CONNECTED_BIT | MQTT_FAILED_BIT);
    credential_store_clear_secret(&s_config, sizeof(s_config));
}

static esp_err_t start_config(const saved_mqtt_config_t *configuration,
                              bool wait_for_connection)
{
    stop_client();
    s_config = *configuration;
    esp_mqtt_client_config_t client_configuration = {
        .broker.address.hostname = s_config.host,
        .broker.address.port = s_config.port,
        .broker.address.transport = MQTT_TRANSPORT_OVER_SSL,
        .broker.verification.certificate =
            s_options.ca_certificate,
        .credentials.client_id = s_options.device_id,
        .credentials.username = s_config.username,
        .credentials.authentication.password = s_config.password,
        .outbox.limit = s_options.outbox_limit_bytes,
        .session.last_will.topic = s_options.availability_topic,
        .session.last_will.msg = "offline",
        .session.last_will.qos = 1,
        .session.last_will.retain = true,
    };

    s_client = esp_mqtt_client_init(&client_configuration);
    if (s_client == NULL) {
        credential_store_clear_secret(&s_config, sizeof(s_config));
        return ESP_ERR_NO_MEM;
    }
    esp_err_t error = esp_mqtt_client_register_event(
        s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (error == ESP_OK) {
        error = esp_mqtt_client_start(s_client);
    }
    if (error != ESP_OK) {
        stop_client();
        return error;
    }

    if (!wait_for_connection) {
        return ESP_OK;
    }
    EventBits_t result = xEventGroupWaitBits(
        s_mqtt_events, MQTT_CONNECTED_BIT | MQTT_FAILED_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(MQTT_CONNECT_TIMEOUT_MS));
    if ((result & MQTT_CONNECTED_BIT) != 0) {
        return ESP_OK;
    }
    stop_client();
    return ESP_ERR_TIMEOUT;
}

esp_err_t mqtt_connection_initialize(const mqtt_connection_options_t *options)
{
    if (s_lock != NULL) return ESP_ERR_INVALID_STATE;
    if (!options || !options->device_id || !options->device_id[0] ||
        !options->ca_certificate || !options->ca_certificate[0] ||
        !options->availability_topic || !options->availability_topic[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((options->command_topic == NULL) != (options->on_message == NULL) ||
        (options->command_topic && (!options->command_topic[0] ||
         strlen(options->command_topic) > 127 || strpbrk(options->command_topic, "+#"))))
        return ESP_ERR_INVALID_ARG;
    s_options = *options;
    s_lock = xSemaphoreCreateMutex();
    s_mqtt_events = xEventGroupCreate();
    return s_lock && s_mqtt_events ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t start_saved_locked(void)
{
    saved_mqtt_config_t config = {0};
    esp_err_t error = load_config(&config);
    if (error == ESP_OK) error = start_config(&config, false);
    credential_store_clear_secret(&config, sizeof(config));
    return error;
}

esp_err_t mqtt_connection_start_saved(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t error = s_client ? ESP_OK : start_saved_locked();
    xSemaphoreGive(s_lock);
    return error;
}

esp_err_t mqtt_connection_test_and_save(const char *host, uint16_t port,
                                        const char *username, const char *password)
{
    if (!host || !username || !password || !host[0] || !username[0] || !password[0] ||
        !port || strlen(host) > MQTT_HOST_MAX_BYTES ||
        strlen(username) > MQTT_USERNAME_MAX_BYTES || strlen(password) > MQTT_PASSWORD_MAX_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    saved_mqtt_config_t candidate = {0};
    strlcpy(candidate.host, host, sizeof(candidate.host));
    strlcpy(candidate.username, username, sizeof(candidate.username));
    strlcpy(candidate.password, password, sizeof(candidate.password));
    candidate.port = port;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t error = start_config(&candidate, true);
    if (error == ESP_OK) error = save_config(&candidate);
    if (error != ESP_OK) {
        stop_client();
        (void)start_saved_locked();
    }
    credential_store_clear_secret(&candidate, sizeof(candidate));
    xSemaphoreGive(s_lock);
    return error;
}

esp_err_t mqtt_connection_forget(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stop_client();
    esp_err_t error = credential_store_forget(MQTT_NVS_NAMESPACE);
    xSemaphoreGive(s_lock);
    return error;
}

bool mqtt_connection_is_connected(void)
{
    return s_mqtt_events && (xEventGroupGetBits(s_mqtt_events) & MQTT_CONNECTED_BIT);
}

bool mqtt_connection_is_configured(void)
{
    saved_mqtt_config_t config = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool configured = load_config(&config) == ESP_OK;
    credential_store_clear_secret(&config, sizeof(config));
    xSemaphoreGive(s_lock);
    return configured;
}

const char *mqtt_connection_device_id(void)
{
    return s_options.device_id;
}

void mqtt_connection_host(char *buffer, size_t capacity)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(buffer, capacity, "%s", s_config.host[0] ? s_config.host : "not-configured");
    xSemaphoreGive(s_lock);
}

uint32_t mqtt_connection_generation(void)
{
    return atomic_load(&s_generation);
}

esp_err_t mqtt_connection_publish(const char *topic, const char *payload, bool retain)
{
    if (!topic || !topic[0] || !payload) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (s_client && mqtt_connection_is_connected()) {
        int id = esp_mqtt_client_enqueue(s_client, topic, payload, 0, 1, retain, true);
        error = id >= 0 ? ESP_OK : ESP_FAIL;
    }
    xSemaphoreGive(s_lock);
    return error;
}
