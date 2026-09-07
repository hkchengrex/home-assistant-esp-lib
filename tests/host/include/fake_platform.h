#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#define BIT0 1
#define BIT1 2
#define portMAX_DELAY UINT32_MAX
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_EVENT_ANY_ID -1
#define MQTT_EVENT_CONNECTED 1
#define MQTT_EVENT_DISCONNECTED 2
#define MQTT_EVENT_ERROR 3
#define MQTT_EVENT_DATA 4
#define MQTT_TRANSPORT_OVER_SSL 2
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define ESP_ERR_NVS_NOT_FOUND 0x1102

typedef const char *esp_event_base_t;
typedef struct events *EventGroupHandle_t;
typedef struct mutex *SemaphoreHandle_t;
typedef unsigned EventBits_t;
EventGroupHandle_t xEventGroupCreate(void);
EventBits_t xEventGroupGetBits(EventGroupHandle_t);
EventBits_t xEventGroupSetBits(EventGroupHandle_t, EventBits_t);
EventBits_t xEventGroupClearBits(EventGroupHandle_t, EventBits_t);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t, EventBits_t, int, int, unsigned);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t, unsigned);
int xSemaphoreGive(SemaphoreHandle_t);

typedef struct client *esp_mqtt_client_handle_t;
typedef struct {
    esp_mqtt_client_handle_t client;
    char *topic, *data;
    int topic_len, data_len, total_data_len, current_data_offset;
    bool retain;
} esp_mqtt_event_t;
typedef esp_mqtt_event_t *esp_mqtt_event_handle_t;
typedef struct {
    struct {
        struct { const char *hostname; unsigned port; int transport; } address;
        struct { const char *certificate; } verification;
    } broker;
    struct {
        const char *client_id;
        const char *username;
        struct { const char *password; } authentication;
    } credentials;
    struct {
        struct { const char *topic; const char *msg; int qos; bool retain; } last_will;
    } session;
    struct { uint64_t limit; } outbox;
} esp_mqtt_client_config_t;
esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *);
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t, int,
    void (*)(void *, esp_event_base_t, int32_t, void *), void *);
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t);
esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t);
esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t);
int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t, const char *, const char *, int, int, bool, bool);

typedef int nvs_handle_t;
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
esp_err_t nvs_get_str(nvs_handle_t, const char *, char *, size_t *);
esp_err_t nvs_get_u16(nvs_handle_t, const char *, uint16_t *);
esp_err_t nvs_set_str(nvs_handle_t, const char *, const char *);
esp_err_t nvs_set_u16(nvs_handle_t, const char *, uint16_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
size_t strlcpy(char *, const char *, size_t);

int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t, const char *, int);
esp_err_t esp_mqtt_client_disconnect(esp_mqtt_client_handle_t);
