# Optional MQTT command reception and extended discovery

Existing Wi-Fi-only and publish-only consumers continue to use their original
interfaces. These extensions are opt-in and keep the original sensor descriptor
and formatter ABI unchanged.

## MQTT options

Initialize `mqtt_connection_options_t` with designated fields. Optional
`command_topic` is one exact topic (no wildcards, maximum 127 bytes). Supply it
together with `on_message`; supplying only one is rejected. Topic strings remain
valid for the client's lifetime. `message_context` is passed unchanged.

The library subscribes at QoS 1 on every connection, including reconnects, and
assembles fragmented messages into a bounded 1024-byte buffer. Oversized,
out-of-order, wrong-topic and embedded-NUL messages are discarded. Completed
messages invoke `on_message(payload, length, retained, context)` on the MQTT task.
Pointers expire when the callback returns. Copy to a bounded queue and return:
do not block, perform sensor I/O, publish, or call client lifecycle methods there.
Applications decide whether to accept retained messages; actuator commands should
reject them and implement expiry and durable duplicate suppression.

Optional `outbox_limit_bytes` bounds queued MQTT bytes. Zero preserves the
ESP-MQTT default for old consumers. At capacity, publication returns an error;
the application should retry current state without accumulating another queue.

## Home Assistant entities

`ha_discovery_entity()` accepts `ha_entity_t`, which embeds the existing
`ha_sensor_t` and adds:

- `binary`: binary_sensor discovery with ON/OFF payloads;
- `unique_id`: optional explicit identity, escaped as JSON;
- `expire_after_s`: stale-data timeout, omitted when zero;
- `availability_template`: optional template on the device availability topic;
- `json_attributes_template`: optional attributes derived from the state topic.

Applications can use distinct device descriptors for separate state topics and
validity templates. Expiry should be used with non-retained measurement messages
so a retained old snapshot cannot make a stale sensor appear healthy on restart.
The old `ha_discovery_sensor()` output is unchanged.

The consumer owns MQTT topic ACLs, entity migration, measurement validity,
command semantics, request acknowledgments, scheduling and sensor drivers.

Run `python3 tests/host/test_components.py` under Linux/WSL for formatter capacity
and escaping checks, original API compatibility, and MQTT lifecycle/concurrency
tests with fragmented command delivery and resubscription.
