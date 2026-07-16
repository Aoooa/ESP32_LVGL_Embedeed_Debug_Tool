idf_component_register(
    SRCS "app_tcp.c"
    INCLUDE_DIRS "include"
    REQUIRES app_bridge drv_uart lwip
)
