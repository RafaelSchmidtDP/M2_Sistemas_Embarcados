idf_component_register(SRCS "sensores.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_gpio esp_timer freertos log)