idf_component_register(SRCS "hello_world_main.c"
                       PRIV_REQUIRES spi_flash esp_driver_uart esp_wifi esp_netif nvs_flash
                       INCLUDE_DIRS "")
