idf_component_register(SRCS "wifi_manager.c" "wifi_validation.c" INCLUDE_DIRS "include"
                       REQUIRES esp_netif credential_store
                       PRIV_REQUIRES esp_wifi esp_event esp_timer)
