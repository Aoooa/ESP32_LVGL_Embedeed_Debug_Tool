set(MODULE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

idf_component_register(
    SRCS
        "main.c"
        "${MODULE_DIR}/uart/drv_uart.c"
        "${MODULE_DIR}/uart/app_uart.c"
        "${MODULE_DIR}/wifi/drv_wifi.c"
        "${MODULE_DIR}/wifi/app_wifi.c"
        "${MODULE_DIR}/tcp/app_tcp.c"
        "${MODULE_DIR}/web/app_web_http.c"
        "${MODULE_DIR}/web/app_web_ws.c"
        "${MODULE_DIR}/web/app_web_broadcast.c"
        "${MODULE_DIR}/display/drv_display.c"
        "${MODULE_DIR}/display/app_display.c"
        "${MODULE_DIR}/ui/flow_view/flow_model.c"
        "${MODULE_DIR}/ui/flow_view/flow_view.c"
        "${MODULE_DIR}/ui/file_browser/file_browser.c"
        "${MODULE_DIR}/ui/font_manager/app_font.c"
        "${MODULE_DIR}/ui/reader/reader_index.c"
        "${MODULE_DIR}/ui/reader/reader.c"
        "${MODULE_DIR}/fonts/han_sc_16.c"
        "${MODULE_DIR}/sdcard/drv_sdcard.c"
        "${MODULE_DIR}/sdcard/app_sdcard.c"

    INCLUDE_DIRS
        "."
        "${MODULE_DIR}/uart"
        "${MODULE_DIR}/wifi"
        "${MODULE_DIR}/tcp"
        "${MODULE_DIR}/web"
        "${MODULE_DIR}/display"
        "${MODULE_DIR}/ui/flow_view"
        "${MODULE_DIR}/ui/file_browser"
        "${MODULE_DIR}/ui/font_manager"
        "${MODULE_DIR}/ui/reader"
        "${MODULE_DIR}/sdcard"

    PRIV_REQUIRES
        spi_flash nvs_flash esp_netif esp_event
        driver esp_wifi esp_http_server lwip freertos
        esp_lcd esp_lcd_touch esp_lcd_touch_cst816s esp_lvgl_adapter lvgl
        sdmmc fatfs heap
)
