idf_component_register(
    SRCS "app_tcp.c"
    INCLUDE_DIRS "."
    REQUIRES uart lwip freertos
)
