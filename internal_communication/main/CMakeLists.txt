idf_component_register(SRCS "mesh_light.c"
                            "mesh_main.c"
                            "mesh_ota.c"
                            "led_strip_encoder.c"
                    INCLUDE_DIRS "." "include"
                    REQUIRES mqtt esp_wifi esp_event esp_netif nvs_flash esp_driver_rmt esp_driver_gpio esp_timer esp_hw_support network_provisioning app_update esp_https_ota esp_http_client mbedtls)
