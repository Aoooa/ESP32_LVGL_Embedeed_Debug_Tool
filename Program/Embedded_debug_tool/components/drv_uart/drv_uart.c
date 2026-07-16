#include "drv_uart.h"
#include "esp_log.h"

static const char *TAG = "drv_uart";

void drv_uart_init(uart_port_t port, int tx_pin, int rx_pin)
{
    uart_config_t cfg = {
        .baud_rate = DRV_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(port, DRV_UART_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(port, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(port, tx_pin, rx_pin,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "UART%d init: TX=IO%d RX=IO%d", port, tx_pin, rx_pin);
}

int drv_uart_read(uart_port_t port, uint8_t *buf, int size, int timeout_ms)
{
    return uart_read_bytes(port, buf, size, pdMS_TO_TICKS(timeout_ms));
}

int drv_uart_write(uart_port_t port, const uint8_t *data, int len)
{
    return uart_write_bytes(port, data, len);
}
