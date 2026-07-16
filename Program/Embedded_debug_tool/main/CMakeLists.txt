idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES spi_flash nvs_flash esp_netif esp_event
                                      uart wifi tcp web
                       INCLUDE_DIRS "")
