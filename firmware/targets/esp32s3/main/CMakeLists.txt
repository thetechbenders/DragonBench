idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES common esp_http_server esp_wifi nvs_flash mdns app_update esp_partition esp_driver_tsens json
)
