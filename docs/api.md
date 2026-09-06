# Component API and ownership

## Boundaries and initialization

| Component | Owns | Application supplies |
| --- | --- | --- |
| `credential_store` | NVS encryption enforcement, namespace erasure, secret clearing | Security configuration and permission for first-boot eFuse provisioning |
| `wifi_manager` | Station interface, Wi-Fi credential schema, test/save, reconnect worker | Initialized netif/default event loop and hostname |
| `mqtt_connection` | Broker credential schema, TLS client, availability, synchronized publish/start/forget | Stable device ID, CA PEM, availability topic, network and clock readiness |
| `ha_discovery` | Escaped and bounded sensor discovery JSON/topic formatting | Device metadata, sensor descriptors, state and availability topics |
| `usb_provisioning` | Native USB driver and existing cable command protocol | Initialized managers and a callback that waits for a valid TLS clock |

`esp-monitor/main/app_main.c` is the full composition example in the consuming repository. It initializes encrypted NVS,
netif and the default event loop, Wi-Fi, MQTT, and SNTP. The application task
starts saved MQTT settings when Wi-Fi and time are ready, retries startup every
five seconds if needed, republishes discovery on each broker connection, and
publishes telemetry every 30 seconds. The USB command loop runs on the main task.

For another device, change application-owned metadata, topics, CA, and sensor
descriptors. Add sensor fields to the application state payload and corresponding
`ha_sensor_t` descriptors. `ha_discovery_sensor()` formats sensor discovery;
it is not an implementation of every Home Assistant entity type. MQTT currently
exposes the publishing operations needed by these sensors, not a general command
subscription framework. Broker ACLs must permit any new topics.

Each directory has its own `CMakeLists.txt` and public `include/` directory.
The MQTT package manifest lives with `mqtt_connection`, so its dependency follows
the component. Applications using their own provisioning interface can omit the
USB adapter. Public headers describe required ordering and string lifetimes.

## Security and persistence

The default NVS partition remains HMAC-backed XTS-AES encrypted through ESP-IDF.
Compilation fails if either NVS encryption or the HMAC scheme is disabled.
There is no plaintext fallback or custom password cipher. The application retains
`CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID=0` and explicitly calls
`credential_store_init(true)` to allow the existing first-boot KEY0 provisioning
policy. Another product may pass `false` to require an already provisioned HMAC
key. Never change a deployed board's slot casually: its existing data depends on
that key, and programming eFuse is permanent.

Initialization returns NVS errors without erasing the partition. This app stops
on such an error. Diagnose storage/version problems and choose deliberate recovery;
do not introduce automatic full-partition erasure in the component. External
flash erasure loses both sets of credentials but cannot erase the eFuse key.
Encrypted NVS protects credentials at rest, not against hostile firmware.

The existing partition layout and key/value representation remain unchanged:

| Namespace | Keys |
| --- | --- |
| `wifi_cfg` | `ssid` and `password` strings |
| `mqtt_cfg` | `host`, `username`, `password` strings and `port` uint16 |

Normal application flashing preserves these settings. Connection candidates are
persisted only after a successful connection; failed connection tests resume
previously saved settings. Multi-key NVS writes are not a power-failure-atomic
credential transaction. A storage failure or interrupted write may still require
reprovisioning. Credentials remain private runtime state and must never be committed.

## Concurrency and recovery

Wi-Fi lifecycle operations belong to one controlling task. A mutex coordinates
them with the reconnect worker, and the station STOP event fences replacement
attempts. Event handlers only update event bits. Reconnection waits 1, 2, 4, 8,
16, then at most 30 seconds between failures; getting an IP resets the backoff.
USB credential tests wait at most 30 seconds for an IP, plus driver stop/recovery
overhead. Invalid lengths are rejected before changing the active connection,
including SSIDs longer than 32 UTF-8 bytes and passwords longer than 63 bytes.

MQTT public operations serialize client creation, replacement, destruction, NVS
access, and enqueue operations. Connection tests can hold this mutex for up to
20 seconds while awaiting broker authentication. MQTT event callbacks never take
the application mutex because `esp_mqtt_client_stop()` waits for the MQTT task.
The callback uses its own event client to enqueue availability. Application
telemetry uses the locked publish API, so it cannot use a destroyed client.
The connection generation counter lets the app republish discovery after reconnects.

Initializers are called once; on initialization failure the app treats the error
as fatal. These are embedded process-lifetime services, without a reusable
initialize/deinitialize cycle. Call APIs only after successful initialization.
