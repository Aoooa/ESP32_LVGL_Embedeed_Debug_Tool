idf_component_register(
    SRCS "app_tcp.c"
    INCLUDE_DIRS "."
    REQUIRES app_uart drv_uart lwip freertos
)
