idf_component_register(SRCS "mqtt_adafruit.c"
                       INCLUDE_DIRS "include"
                       REQUIRES espressif__mqtt log freertos)