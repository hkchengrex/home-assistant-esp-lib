"""Compile and execute the actual portable component C code on Linux/WSL.

Run: python3 tests/host/test_components.py
Requires a C compiler (CC defaults to cc); no third-party Python packages.
"""
import ctypes as c
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
TEMP = tempfile.TemporaryDirectory(prefix="esp-monitor-tests-")
LIB = Path(TEMP.name) / "components.so"
subprocess.run([
    os.environ.get("CC", "cc"), "-shared", "-fPIC", "-std=c11", "-O2",
    "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "tests/host/include"),
    "-I", str(ROOT / "components/wifi_manager/include"),
    "-I", str(ROOT / "components/ha_discovery/include"),
    str(ROOT / "components/wifi_manager/wifi_validation.c"),
    str(ROOT / "components/ha_discovery/ha_discovery.c"), "-o", str(LIB),
], check=True)
lib = c.CDLL(str(LIB))
lib.wifi_manager_validate_credentials.argtypes = [c.c_char_p, c.c_char_p]
lib.wifi_manager_validate_credentials.restype = c.c_int


class Device(c.Structure):
    _fields_ = [(key, c.c_char_p) for key in
                ("id", "name", "manufacturer", "model", "state_topic", "availability_topic")]


class Sensor(c.Structure):
    _fields_ = [(key, c.c_char_p) for key in
                ("id", "name", "value_template", "unit", "device_class", "state_class", "entity_category")]


class Entity(c.Structure):
    _fields_ = [("sensor", Sensor), ("unique_id", c.c_char_p),
                ("availability_template", c.c_char_p), ("json_attributes_template", c.c_char_p),
                ("expire_after_s", c.c_uint), ("binary", c.c_bool)]


lib.ha_discovery_sensor.argtypes = [c.POINTER(Device), c.POINTER(Sensor),
                                   c.c_void_p, c.c_size_t, c.c_void_p, c.c_size_t]
lib.ha_discovery_sensor.restype = c.c_int


class Components(unittest.TestCase):
    def setUp(self):
        self.device = Device(b"esp-c5-10bda3cf4e98", b"esp32-dev", b"Espressif", b"ESP32-C5",
                             b"esp-monitor/esp-c5-10bda3cf4e98/state",
                             b"esp-monitor/esp-c5-10bda3cf4e98/availability")
        self.sensor = Sensor(b"uptime", b"Uptime", b"{{ value_json.uptime_s }}", b"s",
                             None, None, b"diagnostic")

    def render(self, topic_size=160, payload_size=768):
        # Canary bytes detect writes beyond each caller-declared capacity.
        topic = c.create_string_buffer(b"!" * (topic_size + 8), topic_size + 8)
        payload = c.create_string_buffer(b"!" * (payload_size + 8), payload_size + 8)
        result = lib.ha_discovery_sensor(c.byref(self.device), c.byref(self.sensor),
                                         topic, topic_size, payload, payload_size)
        self.assertEqual(topic.raw[topic_size:], b"!" * 8)
        self.assertEqual(payload.raw[payload_size:], b"!" * 8)
        return result, topic.value, payload.value

    def test_extended_entity_bounds_and_binary_discovery(self):
        entity = Entity(self.sensor, b'custom"id', b"{{ value_json.valid }}", None, 3, True)
        for size in range(1, 1000):
            topic = c.create_string_buffer(200)
            output = c.create_string_buffer(b"!" * (size + 8), size + 8)
            error = lib.ha_discovery_entity(c.byref(self.device), c.byref(entity),
                                             topic, 200, output, size)
            self.assertEqual(output.raw[size:], b"!" * 8)
            if error == 0:
                data = json.loads(output.value)
                self.assertEqual(data["unique_id"], 'custom"id')
                self.assertEqual(data["payload_on"], "ON")
                self.assertEqual(data["expire_after"], 3)
                self.assertIn(b"/binary_sensor/", topic.value)
            else:
                self.assertEqual(error, 0x104)
                self.assertEqual(output.value, b"")

    def test_wifi_all_boundaries(self):
        for ssid_size in range(0, 35):
            for password_size in range(0, 67):
                expected = 0 if 1 <= ssid_size <= 32 and (password_size == 0 or 8 <= password_size <= 63) else 0x102
                self.assertEqual(lib.wifi_manager_validate_credentials(b"s" * ssid_size, b"p" * password_size), expected,
                                 (ssid_size, password_size))

    def test_wifi_null_and_utf8_bytes(self):
        for ssid, password in [(None, b""), (b"ssid", None)]:
            self.assertEqual(lib.wifi_manager_validate_credentials(ssid, password), 0x102)
        self.assertEqual(lib.wifi_manager_validate_credentials(("é" * 16).encode(), b"12345678"), 0)
        self.assertEqual(lib.wifi_manager_validate_credentials(("é" * 17).encode(), b"12345678"), 0x102)

    def test_discovery_preserves_existing_contract(self):
        error, topic, payload = self.render()
        self.assertEqual(error, 0)
        self.assertEqual(topic, b"homeassistant/sensor/esp-c5-10bda3cf4e98/uptime/config")
        self.assertEqual(json.loads(payload), {
            "unique_id": "esp-c5-10bda3cf4e98_uptime", "name": "Uptime",
            "state_topic": self.device.state_topic.decode(), "value_template": "{{ value_json.uptime_s }}",
            "unit_of_measurement": "s", "entity_category": "diagnostic",
            "availability_topic": self.device.availability_topic.decode(),
            "device": {"identifiers": ["esp-c5-10bda3cf4e98"], "name": "esp32-dev",
                       "manufacturer": "Espressif", "model": "ESP32-C5"},
        })

    def test_wifi_signal_metadata(self):
        self.sensor = Sensor(b"wifi_signal", b"Wi-Fi Signal", b"{{ value_json.wifi_rssi_dbm }}",
                             b"dBm", b"signal_strength", b"measurement", b"diagnostic")
        error, topic, payload = self.render()
        self.assertEqual(error, 0)
        data = json.loads(payload)
        self.assertTrue(topic.endswith(b"/wifi_signal/config"))
        self.assertEqual(data["unique_id"], "esp-c5-10bda3cf4e98_wifi_signal")
        self.assertEqual(data["device_class"], "signal_strength")
        self.assertEqual(data["state_class"], "measurement")

    def test_json_escaping(self):
        value = 'Room "A" \\ café\n' + ''.join(chr(i) for i in range(1, 32))
        self.device.name = value.encode()
        error, _, payload = self.render(payload_size=2048)
        self.assertEqual(error, 0)
        self.assertEqual(json.loads(payload)["device"]["name"], value)

    def test_output_capacity_every_boundary(self):
        _, topic, payload = self.render()
        for topic_size in range(1, len(topic) + 2):
            error, _, _ = self.render(topic_size=topic_size)
            self.assertEqual(error, 0 if topic_size > len(topic) else 0x104)
        for payload_size in range(1, len(payload) + 2):
            error, out_topic, out_payload = self.render(payload_size=payload_size)
            self.assertEqual(error, 0 if payload_size > len(payload) else 0x104)
            if error:
                self.assertEqual((out_topic, out_payload), (b"", b""))

    def test_invalid_topic_ids(self):
        for value in [b"", b"bad/topic", b"+", b"#", b'"', None]:
            self.sensor.id = value
            self.assertEqual(self.render()[0], 0x102)

    def test_mqtt_lifecycle_concurrency(self):
        executable = Path(TEMP.name) / "mqtt-test"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-g", "-O1", "-pthread",
            "-fsanitize=address,undefined", "-Wall", "-Wextra", "-Werror",
            "-I", str(ROOT / "tests/host/include"),
            "-I", str(ROOT / "components/mqtt_connection/include"),
            "-I", str(ROOT / "components/credential_store/include"),
            str(ROOT / "tests/host/test_mqtt.c"),
            str(ROOT / "components/mqtt_connection/mqtt_connection.c"),
            "-o", str(executable),
        ], check=True)
        # The component's mutex and event group deliberately live until reboot.
        environment = dict(os.environ, ASAN_OPTIONS="detect_leaks=0", UBSAN_OPTIONS="halt_on_error=1")
        subprocess.run([str(executable)], check=True, timeout=20, env=environment)

    def test_plaintext_storage_build_is_rejected(self):
        headers = Path(TEMP.name) / "security-headers"
        headers.mkdir(exist_ok=True)
        for header in ("esp_efuse.h", "nvs_flash.h"):
            (headers / header).write_text("")
        for encrypted, hmac in [(0, 0), (1, 0), (0, 1)]:
            (headers / "sdkconfig.h").write_text(
                f"#define CONFIG_NVS_ENCRYPTION {encrypted}\n"
                f"#define CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC {hmac}\n")
            result = subprocess.run([
                os.environ.get("CC", "cc"), "-E", "-I", str(headers),
                "-I", str(ROOT / "tests/host/include"),
                "-I", str(ROOT / "components/credential_store/include"),
                str(ROOT / "components/credential_store/credential_store.c"),
            ], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("plaintext fallback is forbidden", result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
