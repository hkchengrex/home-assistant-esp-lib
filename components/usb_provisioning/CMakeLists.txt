idf_component_register(SRCS "usb_provisioning.c" INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES esp_driver_usb_serial_jtag wifi_manager mqtt_connection credential_store)
