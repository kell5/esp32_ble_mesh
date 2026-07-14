set(srcs "main.c"
    "board.c")

if(CONFIG_IDF_TARGET_ESP32S3)
    list(APPEND srcs "led_strip_encoder.c")
endif()

idf_component_register(SRCS "${srcs}"
                    INCLUDE_DIRS "."
                    REQUIRES bt nvs_flash
                    PRIV_REQUIRES esp_driver_gpio esp_driver_rmt)
