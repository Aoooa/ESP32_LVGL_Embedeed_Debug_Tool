idf_component_register(
    SRCS "app_bridge.c"
    INCLUDE_DIRS "include"
    REQUIRES drv_uart freertos
)
