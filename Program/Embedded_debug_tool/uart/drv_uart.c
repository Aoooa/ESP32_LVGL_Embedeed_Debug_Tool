#include "drv_uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

void drv_uart_init(uart_port_t port, int tx_pin, int rx_pin)
{
    if (uart_is_driver_installed(port)) {
        ESP_LOGI("drv_uart", "UART%d already installed, skip", port);
        return;
    }
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
    ESP_LOGI("drv_uart", "UART%d: TX=IO%d RX=IO%d", port, tx_pin, rx_pin);
}

void drv_uart_deinit(uart_port_t port, int tx_pin, int rx_pin)
{
    if (!uart_is_driver_installed(port)) return;
    uart_driver_delete(port);
    /* 归还引脚为普通 GPIO（不再被 UART 占用，可选作他途） */
    if (tx_pin >= 0) gpio_reset_pin(tx_pin);
    if (rx_pin >= 0) gpio_reset_pin(rx_pin);
    ESP_LOGI("drv_uart", "UART%d deinit, IO%d/IO%d released", port, tx_pin, rx_pin);
}

int drv_uart_read(uart_port_t port, uint8_t *buf, int size, int timeout_ms)
{
    return uart_read_bytes(port, buf, size, pdMS_TO_TICKS(timeout_ms));
}

int drv_uart_write(uart_port_t port, const uint8_t *data, int len)
{
    return uart_write_bytes(port, data, len);
}
