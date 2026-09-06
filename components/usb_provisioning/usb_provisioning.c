#include "usb_provisioning.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "esp_rom_usb_serial.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "credential_store.h"
#include "wifi_manager.h"
#include "mqtt_connection.h"

static bool (*s_wait_for_time)(void);

esp_err_t usb_provisioning_initialize(bool (*wait_for_time)(void))
{
    if (wait_for_time == NULL) return ESP_ERR_INVALID_ARG;
    s_wait_for_time = wait_for_time;
    usb_serial_jtag_driver_config_t config = {
        .rx_buffer_size = 1024, .tx_buffer_size = 1024,
    };
    return usb_serial_jtag_driver_install(&config);
}

void usb_provisioning_send(const char *format, ...)
{
    char message[192];
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);

    if (length < 0) {
        return;
    }
    if ((size_t)length >= sizeof(message)) {
        length = sizeof(message) - 1;
    }
    for (int index = 0; index < length; ++index) {
        esp_rom_usb_serial_putc(message[index]);
    }
}

static bool read_serial_line(char *buffer, size_t capacity)
{
    size_t length = 0;
    bool overflow = false;

    for (;;) {
        uint8_t character;
        int received = usb_serial_jtag_read_bytes(&character, 1, portMAX_DELAY);
        if (received != 1) {
            continue;
        }
        if (character == 0) { overflow = true; continue; }
        if (character == '\r') {
            continue;
        }
        if (character == '\n') {
            buffer[length] = '\0';
            return !overflow;
        }
        if (length + 1 < capacity) {
            buffer[length++] = (char)character;
        } else {
            overflow = true;
        }
    }
}

static void provision_wifi(void)
{
    char ssid[WIFI_SSID_MAX_BYTES + 1] = {0};
    char password[WIFI_PASSWORD_MAX_BYTES + 1] = {0};
    usb_provisioning_send("WIFI_SSID_REQUEST\n");
    if (!read_serial_line(ssid, sizeof(ssid)) || ssid[0] == '\0') {
        usb_provisioning_send("WIFI_ERROR invalid-ssid; use 1-%u UTF-8 bytes\n", WIFI_SSID_MAX_BYTES);
        return;
    }
    usb_provisioning_send("WIFI_PASSWORD_REQUEST\n");
    if (!read_serial_line(password, sizeof(password))) {
        usb_provisioning_send("WIFI_ERROR invalid-password; use 8-%u UTF-8 bytes\n", WIFI_PASSWORD_MAX_BYTES);
        credential_store_clear_secret(password, sizeof(password));
        return;
    }
    size_t password_length = strlen(password);
    if (password_length != 0 && password_length < 8) {
        usb_provisioning_send("WIFI_ERROR invalid-password; use 8-%u UTF-8 bytes\n", WIFI_PASSWORD_MAX_BYTES);
        credential_store_clear_secret(password, sizeof(password));
        return;
    }

    usb_provisioning_send("WIFI_CONNECTING\n");
    esp_err_t error = wifi_manager_test_and_save(ssid, password);
    credential_store_clear_secret(password, sizeof(password));
    if (error != ESP_OK) {
        usb_provisioning_send("WIFI_FAILED connection-or-save-failed\n");
        return;
    }
    char ip[16];
    wifi_manager_ip(ip, sizeof(ip));
    usb_provisioning_send("WIFI_CONNECTED ip=%s\n", ip);
}

static void provision_mqtt(void)
{
    char host[MQTT_HOST_MAX_BYTES + 1] = {0};
    char port_text[8] = {0};
    char username[MQTT_USERNAME_MAX_BYTES + 1] = {0};
    char password[MQTT_PASSWORD_MAX_BYTES + 1] = {0};

    if (!wifi_manager_is_connected()) {
        usb_provisioning_send("MQTT_ERROR wifi-not-connected\n");
        return;
    }
    if (!s_wait_for_time()) {
        usb_provisioning_send("MQTT_ERROR time-sync-failed\n");
        return;
    }

    usb_provisioning_send("MQTT_HOST_REQUEST\n");
    if (!read_serial_line(host, sizeof(host)) || host[0] == '\0') {
        usb_provisioning_send("MQTT_ERROR invalid-host\n");
        return;
    }
    usb_provisioning_send("MQTT_PORT_REQUEST\n");
    if (!read_serial_line(port_text, sizeof(port_text))) {
        usb_provisioning_send("MQTT_ERROR invalid-port\n");
        return;
    }
    char *port_end = NULL;
    unsigned long port_value = strtoul(port_text, &port_end, 10);
    if (port_text[0] == '\0' || *port_end != '\0' || port_value == 0 ||
        port_value > UINT16_MAX) {
        usb_provisioning_send("MQTT_ERROR invalid-port\n");
        return;
    }
    usb_provisioning_send("MQTT_USERNAME_REQUEST\n");
    if (!read_serial_line(username, sizeof(username)) || username[0] == '\0') {
        usb_provisioning_send("MQTT_ERROR invalid-username\n");
        return;
    }
    usb_provisioning_send("MQTT_PASSWORD_REQUEST\n");
    if (!read_serial_line(password, sizeof(password)) || password[0] == '\0') {
        usb_provisioning_send("MQTT_ERROR invalid-password\n");
        credential_store_clear_secret(password, sizeof(password));
        return;
    }

    usb_provisioning_send("MQTT_CONNECTING\n");
    esp_err_t error = mqtt_connection_test_and_save(
        host, (uint16_t)port_value, username, password);
    credential_store_clear_secret(password, sizeof(password));
    mqtt_connection_host(host, sizeof(host));
    if (error == ESP_OK) {
        usb_provisioning_send("MQTT_CONNECTED broker=%s client=%s\n",
                 host, mqtt_connection_device_id());
    } else {
        usb_provisioning_send("MQTT_FAILED could-not-authenticate\n");
    }
}

void usb_provisioning_run(void)
{
    char command[64];
    usb_provisioning_send("COMMANDS WIFI_SETUP WIFI_STATUS WIFI_FORGET MQTT_SETUP "
             "MQTT_STATUS MQTT_FORGET DEVICE_INFO HELP\n");
    for (;;) {
        if (!read_serial_line(command, sizeof(command))) {
            vTaskDelay(pdMS_TO_TICKS(20));
        } else if (strcmp(command, "WIFI_SETUP") == 0) {
            provision_wifi();
        } else if (strcmp(command, "WIFI_STATUS") == 0) {
            bool connected = wifi_manager_is_connected();
            char ip[16];
            wifi_manager_ip(ip, sizeof(ip));
            if (connected) {
                usb_provisioning_send("WIFI_STATUS connected ip=%s\n", ip);
            } else {
                usb_provisioning_send("WIFI_STATUS disconnected\n");
            }
        } else if (strcmp(command, "WIFI_FORGET") == 0) {
            if (wifi_manager_forget() == ESP_OK) {
                usb_provisioning_send("WIFI_FORGOTTEN restarting\n");
                vTaskDelay(pdMS_TO_TICKS(250));
                esp_restart();
            }
            usb_provisioning_send("WIFI_ERROR could-not-forget\n");
        } else if (strcmp(command, "MQTT_SETUP") == 0) {
            provision_mqtt();
        } else if (strcmp(command, "MQTT_STATUS") == 0) {
            char host[MQTT_HOST_MAX_BYTES + 1];
            mqtt_connection_host(host, sizeof(host));
            if (mqtt_connection_is_connected()) {
                usb_provisioning_send("MQTT_STATUS connected broker=%s client=%s\n",
                         host, mqtt_connection_device_id());
            } else if (mqtt_connection_is_configured()) {
                usb_provisioning_send("MQTT_STATUS disconnected broker=%s client=%s\n",
                         host, mqtt_connection_device_id());
            } else {
                usb_provisioning_send("MQTT_STATUS not-configured client=%s\n",
                         mqtt_connection_device_id());
            }
        } else if (strcmp(command, "MQTT_FORGET") == 0) {
            if (mqtt_connection_forget() == ESP_OK) {
                usb_provisioning_send("MQTT_FORGOTTEN\n");
            } else {
                usb_provisioning_send("MQTT_ERROR could-not-forget\n");
            }
        } else if (strcmp(command, "DEVICE_INFO") == 0) {
            usb_provisioning_send("DEVICE_INFO id=%s\n", mqtt_connection_device_id());
        } else if (strcmp(command, "HELP") == 0) {
            usb_provisioning_send("COMMANDS WIFI_SETUP WIFI_STATUS WIFI_FORGET MQTT_SETUP "
                     "MQTT_STATUS MQTT_FORGET DEVICE_INFO HELP\n");
        } else if (command[0] != '\0') {
            usb_provisioning_send("WIFI_ERROR unknown-command\n");
        }
    }
}
