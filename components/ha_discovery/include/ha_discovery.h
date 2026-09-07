#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    const char *id;
    const char *name;
    const char *manufacturer;
    const char *model;
    const char *state_topic;
    const char *availability_topic;
} ha_device_t;

typedef struct {
    const char *id;
    const char *name;
    const char *value_template;
    const char *unit;
    const char *device_class;
    const char *state_class;
    const char *entity_category;
} ha_sensor_t;

// Pure formatter: no MQTT or Wi-Fi dependency. Optional metadata may be NULL.
// IDs must be nonempty ASCII letters/digits/underscore/hyphen. JSON text is
// escaped. Returns INVALID_SIZE instead of emitting a truncated document.
esp_err_t ha_discovery_sensor(const ha_device_t *device, const ha_sensor_t *sensor,
                              char *topic, size_t topic_size,
                              char *payload, size_t payload_size);

// Extended entity descriptor. Original sensor API remains unchanged.
typedef struct {
    ha_sensor_t sensor;
    const char *unique_id; // optional explicit migration identity
    const char *availability_template; // template on availability_topic
    const char *json_attributes_template; // state topic attributes
    unsigned expire_after_s;
    bool binary;
} ha_entity_t;
esp_err_t ha_discovery_entity(const ha_device_t *device, const ha_entity_t *entity,
                              char *topic, size_t topic_size,
                              char *payload, size_t payload_size);
