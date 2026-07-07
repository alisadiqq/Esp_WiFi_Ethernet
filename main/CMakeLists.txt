idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_netif
        esp_event
        esp_wifi
        esp_eth
        esp_http_server
        esp_http_client
)
