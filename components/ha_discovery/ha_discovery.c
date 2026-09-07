#include "ha_discovery.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef struct { char *out; size_t size; size_t used; bool failed; } writer_t;

static void append(writer_t *w, const char *text)
{
    size_t length = strlen(text);
    if (w->failed || length >= w->size - w->used) {
        w->failed = true;
        return;
    }
    memcpy(w->out + w->used, text, length + 1);
    w->used += length;
}

static void quoted(writer_t *w, const char *text)
{
    append(w, "\"");
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        char escaped[7];
        if (*p == '"' || *p == '\\') {
            escaped[0] = '\\'; escaped[1] = *p; escaped[2] = 0;
        } else if (*p < 0x20) {
            snprintf(escaped, sizeof(escaped), "\\u%04x", *p);
        } else {
            escaped[0] = *p; escaped[1] = 0;
        }
        append(w, escaped);
    }
    append(w, "\"");
}

static void field(writer_t *w, const char *key, const char *value)
{
    if (value == NULL) return;
    append(w, ","); quoted(w, key); append(w, ":"); quoted(w, value);
}

static bool valid_id(const char *id)
{
    if (!id || !id[0]) return false;
    for (; *id; ++id) {
        if (!((*id >= 'a' && *id <= 'z') || (*id >= 'A' && *id <= 'Z') ||
              (*id >= '0' && *id <= '9') || *id == '-' || *id == '_')) return false;
    }
    return true;
}

static esp_err_t render(const ha_device_t *device, const ha_sensor_t *sensor,
                              const ha_entity_t *entity,
                              char *topic, size_t topic_size,
                              char *payload, size_t payload_size)
{
    if (!device || !sensor || !topic || !topic_size || !payload || !payload_size ||
        !valid_id(device->id) || !valid_id(sensor->id) || !device->name ||
        !device->state_topic || !device->availability_topic || !sensor->name ||
        !sensor->value_template) return ESP_ERR_INVALID_ARG;
    topic[0] = payload[0] = 0;
    int length = snprintf(topic, topic_size, "homeassistant/%s/%s/%s/config", entity && entity->binary ? "binary_sensor" : "sensor", device->id, sensor->id);
    if (length < 0 || (size_t)length >= topic_size) {
        topic[0] = 0;
        return ESP_ERR_INVALID_SIZE;
    }
    writer_t w = { .out = payload, .size = payload_size };
    append(&w, "{\"unique_id\":");
    if (entity && entity->unique_id) {
        quoted(&w, entity->unique_id);
    } else {
        append(&w, "\""); append(&w, device->id); append(&w, "_"); append(&w, sensor->id); append(&w, "\"");
    }
    field(&w, "name", sensor->name);
    field(&w, "state_topic", device->state_topic);
    field(&w, "value_template", sensor->value_template);
    field(&w, "unit_of_measurement", sensor->unit);
    field(&w, "device_class", sensor->device_class);
    field(&w, "state_class", sensor->state_class);
    field(&w, "entity_category", sensor->entity_category);
    field(&w, "availability_topic", device->availability_topic);
    if (entity) {
        field(&w, "availability_template", entity->availability_template);
        if (entity->json_attributes_template) {
            field(&w, "json_attributes_topic", device->state_topic);
            field(&w, "json_attributes_template", entity->json_attributes_template);
        }
        if (entity->expire_after_s) {
            char number[32];
            snprintf(number, sizeof(number), ",\"expire_after\":%u", entity->expire_after_s);
            append(&w, number);
        }
        if (entity->binary) {
            field(&w, "payload_on", "ON");
            field(&w, "payload_off", "OFF");
        }
    }
    append(&w, ",\"device\":{\"identifiers\":["); quoted(&w, device->id); append(&w, "]");
    field(&w, "name", device->name);
    field(&w, "manufacturer", device->manufacturer);
    field(&w, "model", device->model);
    append(&w, "}}");
    if (w.failed) {
        topic[0] = payload[0] = 0;
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t ha_discovery_sensor(const ha_device_t *device, const ha_sensor_t *sensor,
                              char *topic, size_t topic_size, char *payload, size_t payload_size)
{
    return render(device, sensor, NULL, topic, topic_size, payload, payload_size);
}
esp_err_t ha_discovery_entity(const ha_device_t *device, const ha_entity_t *entity,
                              char *topic, size_t topic_size, char *payload, size_t payload_size)
{
    if (!entity) return ESP_ERR_INVALID_ARG;
    return render(device, &entity->sensor, entity, topic, topic_size, payload, payload_size);
}
