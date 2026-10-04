#pragma once
#include <stdbool.h>
#include "esp_err.h"

// Optional native USB Serial/JTAG adapter. App supplies its TLS clock policy.
esp_err_t usb_provisioning_initialize(bool (*wait_for_time)(void));
void usb_provisioning_send(const char *format, ...) __attribute__((format(printf, 1, 2)));
// Blocking command loop; run from the Wi-Fi lifecycle controlling task.
void usb_provisioning_run(void);

// Register before run(); handler returns true only for commands it recognizes.
void usb_provisioning_set_command_handler(bool (*handler)(const char *));
