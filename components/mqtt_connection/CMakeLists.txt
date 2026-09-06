idf_component_register(SRCS "mqtt_connection.c" INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES mqtt credential_store esp_event)
