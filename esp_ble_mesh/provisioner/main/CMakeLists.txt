set(srcs "main.c"
    "gateway_bridge.c")

idf_component_register(SRCS "${srcs}"
                    INCLUDE_DIRS "."
                    REQUIRES bt nvs_flash esp_wifi esp_event esp_netif mqtt)
