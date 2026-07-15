/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "driver/uart.h"

#define UART_PORT       UART_NUM_1
#define UART_TX_PIN     2
#define UART_RX_PIN     4
#define UART_BAUD_RATE  115200
#define UART_BUF_SIZE   256

void app_main(void)
{
    /* Configure UART1 parameters */
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    /* Install UART driver */
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, UART_BUF_SIZE, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    printf("UART1 configured: TX=IO%d, RX=IO%d, baud=%d\n",
           UART_TX_PIN, UART_RX_PIN, UART_BAUD_RATE);

    const char *msg = "test\n";
    while (1) {
        uart_write_bytes(UART_PORT, msg, strlen(msg));
        printf("[UART1] sent: test\n");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
