idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES spi_flash nvs_flash esp_netif esp_event
                                      drv_uart drv_wifi app_bridge app_tcp app_web
                       INCLUDE_DIRS "")
