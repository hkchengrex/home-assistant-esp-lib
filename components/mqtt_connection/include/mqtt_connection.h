#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define MQTT_HOST_MAX_BYTES 127
#define MQTT_USERNAME_MAX_BYTES 63
#define MQTT_PASSWORD_MAX_BYTES 95

// Invoked on the MQTT task with a complete, bounded message. Copy/queue only;
// never block or call client lifecycle/publish APIs here. Pointers expire on return.
typedef void (*mqtt_message_handler_t)(const char *payload, size_t length,
                                       bool retained, void *context);

typedef struct {
    const char *device_id;
    const char *ca_certificate;
    const char *availability_topic;
    // Optional exact topic (no wildcards); lifetime must cover the client.
    const char *command_topic;
    mqtt_message_handler_t on_message;
    void *message_context;
    uint32_t outbox_limit_bytes; // 0 preserves ESP-MQTT default; nonzero bounds queued bytes
} mqtt_connection_options_t;

// Singleton, process lifetime. Options are copied; strings must remain valid.
// Initialize once. All subsequent APIs are task-safe; no lifecycle API may be
// called from an MQTT callback. This component owns its ESP-MQTT handle.
esp_err_t mqtt_connection_initialize(const mqtt_connection_options_t *options);
// Idempotent while a client exists; ESP-MQTT reconnects that client itself.
esp_err_t mqtt_connection_start_saved(void);
esp_err_t mqtt_connection_test_and_save(const char *host, uint16_t port,
                                        const char *username, const char *password);
esp_err_t mqtt_connection_forget(void);
bool mqtt_connection_is_connected(void);
bool mqtt_connection_is_configured(void);
const char *mqtt_connection_device_id(void);
void mqtt_connection_host(char *buffer, size_t capacity);
// Increments on every broker connection; lets app republish discovery/state.
uint32_t mqtt_connection_generation(void);
// Queues a NUL-terminated payload with QoS 1. Serialized against stop/destroy.
esp_err_t mqtt_connection_publish(const char *topic, const char *payload, bool retain);
