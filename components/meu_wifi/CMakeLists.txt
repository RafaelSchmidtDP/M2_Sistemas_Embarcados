idf_component_register(SRCS "wifi_config.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_wifi esp_event esp_netif log)