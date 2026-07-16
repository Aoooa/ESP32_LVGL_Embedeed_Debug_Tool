idf_component_register(SRCS "app_web_http.c" "app_web_ws.c" "app_web_broadcast.c" INCLUDE_DIRS "." REQUIRES uart esp_http_server lwip freertos)
