idf_component_register(SRCS "credential_store.c" INCLUDE_DIRS "include"
                       REQUIRES nvs_flash PRIV_REQUIRES efuse)
