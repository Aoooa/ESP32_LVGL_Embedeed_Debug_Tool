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
        "${MODULE_DIR}/platform/input/gesture.c"
        "${MODULE_DIR}/platform/components/flow_view/flow_model.c"
        "${MODULE_DIR}/platform/components/flow_view/flow_view.c"
        "${MODULE_DIR}/apps/file_browser/file_browser.c"
        "${MODULE_DIR}/apps/launcher/launcher.c"
        "${MODULE_DIR}/apps/launcher/launcher_icons.c"
        "${MODULE_DIR}/platform/components/font_manager/app_font.c"
        "${MODULE_DIR}/apps/reader/reader_index.c"
        "${MODULE_DIR}/apps/reader/reader.c"
        "${MODULE_DIR}/apps/reader/reader_view.c"
        "${MODULE_DIR}/apps/reader/reader_app.c"
        "${MODULE_DIR}/apps/terminal/terminal.c"
        "${MODULE_DIR}/apps/net_console/net_console.c"
        # "${MODULE_DIR}/apps/test/app_test.c"   # 测试模块（默认关闭；启用：取消注释 + main.c #if 1 + app_test_run()）
        "${MODULE_DIR}/fonts/han_sc_16.c"
        "${MODULE_DIR}/sdcard/drv_sdcard.c"
        "${MODULE_DIR}/sdcard/app_sdcard.c"
        "${MODULE_DIR}/sdcard/app_cardreader.c"
        "${MODULE_DIR}/apps/card_reader/card_reader.c"

    INCLUDE_DIRS
        "."
        "${MODULE_DIR}/uart"
        "${MODULE_DIR}/wifi"
        "${MODULE_DIR}/tcp"
        "${MODULE_DIR}/web"
        "${MODULE_DIR}/display"
        "${MODULE_DIR}/platform/input"
        "${MODULE_DIR}/apps"
        "${MODULE_DIR}/apps/launcher"
        "${MODULE_DIR}/apps/file_browser"
        "${MODULE_DIR}/apps/net_console"
        "${MODULE_DIR}/apps/reader"
        "${MODULE_DIR}/apps/terminal"
        "${MODULE_DIR}/apps/card_reader"
        "${MODULE_DIR}/apps/test"
        "${MODULE_DIR}/platform/components/flow_view"
        "${MODULE_DIR}/platform/components/font_manager"
        "${MODULE_DIR}/sdcard"

    PRIV_REQUIRES
        spi_flash nvs_flash esp_netif esp_event
        driver esp_wifi esp_http_server lwip freertos
        esp_lcd esp_lcd_touch esp_lcd_touch_cst816s esp_lvgl_adapter lvgl
        sdmmc fatfs heap
        esp_tinyusb usb esp_driver_sdspi hal soc
)
