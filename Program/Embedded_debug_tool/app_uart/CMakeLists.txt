idf_component_register(
    SRCS "app_uart.c"
    INCLUDE_DIRS "."
    REQUIRES drv_uart freertos
)
