idf_component_register(SRCS "firebase_client.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_client espressif__cjson log mbedtls)