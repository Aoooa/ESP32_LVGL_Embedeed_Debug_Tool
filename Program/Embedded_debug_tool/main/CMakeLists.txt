idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES spi_flash nvs_flash esp_netif esp_event
                                      app_uart app_wifi app_tcp app_web
                       INCLUDE_DIRS "")
