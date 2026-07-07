idf_component_register(SRCS "mesh_light.c"
                            "mesh_main.c"
                            "led_strip_encoder.c"
                    INCLUDE_DIRS "." "include"
                    REQUIRES mqtt esp_wifi esp_event esp_netif nvs_flash esp_driver_rmt esp_driver_gpio)
