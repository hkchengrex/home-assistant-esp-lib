# Home Assistant ESP library

Reusable ESP-IDF connectivity components, extracted from
[esp-monitor](https://github.com/hkchengrex/esp-monitor) at commit `761add2`.
One repository and one version (`0.1.0`) cover all components; applications select
the modules they need. This is an initial API, validated with ESP-IDF 6.0.2 on
ESP32-C5. Other chips and IDF versions are not yet qualified.

| Component | Purpose |
| --- | --- |
| `credential_store` | Enforce HMAC-backed encrypted NVS, clear secrets, forget a namespace |
| `wifi_manager` | Validate/test/save station credentials and reconnect with backoff |
| `mqtt_connection` | TLS MQTT, broker credentials, availability and synchronized publishing |
| `ha_discovery` | Generate bounded, escaped Home Assistant sensor discovery JSON |
| `usb_provisioning` | Optional native USB Serial/JTAG provisioning commands |

Device names, IDs, CA certificates, sensor definitions, state payloads, time
synchronization policy and publication schedules belong to the application.
Broker administration and account provisioning remain deployment tooling.
No credentials, CA private keys, or broker account data belong in this repository.

## Consume a pinned revision

From a consuming Git repository on your trusted development machine:

```sh
git submodule add git@github.com:hkchengrex/home-assistant-esp-lib.git vendor/home-assistant-esp-lib
git -C vendor/home-assistant-esp-lib checkout v0.1.0
git add .gitmodules vendor/home-assistant-esp-lib
git commit -m "Pin Home Assistant ESP library"
```

The consumer records the exact library commit as a Git submodule pointer.
The release tag is a convenient selector; updating the library requires an
explicit pointer change and consumer verification. No floating branch dependency
or five separately versioned packages are needed.

Add this before ESP-IDF's `project.cmake` in the application's root CMake file:

```cmake
cmake_minimum_required(VERSION 3.16)
# Optional; omission makes all components available.
set(HA_ESP_LIB_COMPONENTS wifi_manager mqtt_connection ha_discovery usb_provisioning)
include("${CMAKE_CURRENT_LIST_DIR}/vendor/home-assistant-esp-lib/cmake/components.cmake")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(my_device)
```

Declare the components used by each application component in `PRIV_REQUIRES`.
The helper adds the selected components and their internal dependencies without
overwriting existing `EXTRA_COMPONENT_DIRS`. Selecting only `wifi_manager` also
adds `credential_store`, without registering MQTT or USB. ESP-MQTT 1.1.0 is pinned
in the MQTT component's manifest and fetched by ESP-IDF only when that component
is registered.

Fresh consumer checkouts require:

```sh
git clone --recurse-submodules <consumer-repository>
# For an existing checkout:
git submodule update --init --recursive
```

Private repository access uses the development machine's existing SSH identity.
Never put a private SSH key or GitHub token on an ESP device or deployment host.

## Security and lifetime

Applications must enable `CONFIG_NVS_ENCRYPTION` and
`CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC` and choose their eFuse key slot explicitly.
Credential storage fails compilation without these protections; it never falls
back to plaintext or automatically erases NVS. `credential_store_init(true)`
permits permanent first-boot HMAC key provisioning; passing `false` requires the
key to exist already. Encrypted NVS protects data at rest, not against hostile
firmware. Partition layout and key selection remain product policy.

Managers are singletons initialized once for the lifetime of the firmware.
Wi-Fi lifecycle operations have one controlling task; MQTT lifecycle and publish
operations are synchronized. See [API and ownership](docs/api.md) and the public
headers for ordering, buffer limits and concurrency contracts.

## Examples and verification

`examples/wifi_only` is a minimal independent application that consumes only
Wi-Fi and credential storage. It uses an already provisioned device and has no
hardcoded passwords or USB provisioning dependency. Build it in ESP-IDF 6.0.2:

```sh
cd examples/wifi_only
idf.py set-target esp32c5
idf.py build
```

It is a build/usage example, not a replacement to flash over production firmware.
For the full Wi-Fi, TLS MQTT, sensor discovery, USB and telemetry composition,
see the esp-monitor consumer.

Run host regression tests on Linux/WSL with Python 3, a C compiler, and the
AddressSanitizer/UndefinedBehaviorSanitizer runtimes:

```sh
python3 tests/host/test_components.py
```

The tests compile actual component sources and check input boundaries, JSON
escaping and buffer limits, encryption-configuration rejection, and concurrent
MQTT publishing/replacement with rollback. esp-monitor retains its USB hardware
smoke test and deployment-specific validation history. The component C sources
in this initial extraction are unchanged from its hardware-verified revision.

## Maintaining the library

Change and test the components here, build the Wi-Fi-only example and full
consumer, then commit and push a library revision. Tag coordinated releases and
update `VERSION`. Update each consumer's submodule pointer explicitly and repeat
the checks relevant to that application. Commit consumer lockfiles as well.
