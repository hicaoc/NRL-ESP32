#include "nrl_usb_console.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>

#include <string.h>

// Console topology:
//   * UART0 primary (CONFIG_ESP_CONSOLE_UART_DEFAULT) on S31 boards whose USB
//     port is a CP210x UART bridge wired to UART0 TX0/RX0.
//   * USB-Serial-JTAG primary (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG) on boards
//     whose USB port is the chip's native USB (gezipai / bh4tdv).
#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) && CONFIG_ESP_CONSOLE_UART_DEFAULT
#define NRL_CONSOLE_UART 1
#include <driver/uart.h>
#include <driver/uart_vfs.h>
#ifdef CONFIG_ESP_CONSOLE_UART_NUM
#define NRL_CONSOLE_UART_NUM CONFIG_ESP_CONSOLE_UART_NUM
#else
#define NRL_CONSOLE_UART_NUM 0
#endif
#else
#define NRL_CONSOLE_UART 0
#include <driver/usb_serial_jtag.h>
#include <driver/usb_serial_jtag_vfs.h>
#endif


static const char *TAG = "CONSOLE";

namespace {

constexpr size_t kRxBufferBytes = 1024;
constexpr size_t kTxBufferBytes = 1024;

bool s_installed = false;

// One-byte "peek" buffer per input source.
uint8_t s_peek_byte = 0;
bool s_peek_valid = false;

int consoleReadBytes(uint8_t *buf, size_t len)
{
#if NRL_CONSOLE_UART
    return uart_read_bytes((uart_port_t)NRL_CONSOLE_UART_NUM, buf, len, 0);
#else
    return usb_serial_jtag_read_bytes(buf, len, 0);
#endif
}

} // namespace

extern "C" bool NRL_USB_Console_Init(void)
{
    if (s_installed) {
        return true;
    }
#if NRL_CONSOLE_UART
    if (!uart_is_driver_installed((uart_port_t)NRL_CONSOLE_UART_NUM)) {
        const esp_err_t err = uart_driver_install((uart_port_t)NRL_CONSOLE_UART_NUM,
                                                  kRxBufferBytes, kTxBufferBytes, 0, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "uart_driver_install failed: %d", err);
            return false;
        }
    }
    uart_vfs_dev_use_driver(NRL_CONSOLE_UART_NUM);
    s_installed = true;
    ESP_LOGI(TAG, "UART%d console ready", (int)NRL_CONSOLE_UART_NUM);
#else
    usb_serial_jtag_driver_config_t cfg = {};
    cfg.rx_buffer_size = kRxBufferBytes;
    cfg.tx_buffer_size = kTxBufferBytes;
    const esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "usb_serial_jtag_driver_install failed: %d", err);
        return false;
    }
    usb_serial_jtag_vfs_use_driver();
    s_installed = true;
    ESP_LOGI(TAG, "USB-Serial-JTAG console ready");
#endif


    return true;
}

extern "C" size_t NRL_USB_Console_Available(void)
{
    if (!s_installed) {
        return 0u;
    }
    if (s_peek_valid) {
        return 1u;
    }
    if (consoleReadBytes(&s_peek_byte, 1) > 0) {
        s_peek_valid = true;
        return 1u;
    }
    return 0u;
}

extern "C" size_t NRL_USB_Console_Read(uint8_t *buffer, const size_t buffer_size)
{
    if (!s_installed || buffer == nullptr || buffer_size == 0u) {
        return 0u;
    }
    size_t off = 0u;
    // Drain peek buffers first.
    if (s_peek_valid) {
        buffer[off++] = s_peek_byte;
        s_peek_valid = false;
        if (off >= buffer_size) return off;
    }
    // Read ONLY from primary console (UART0 on S31/CP210x boards).
    // Do NOT read from USB-Serial-JTAG here: its RX picks up the device's
    // own mirrored log output (containing "IMPROV" text), corrupting the
    // Improv frame parser with false triggers.
    const int got = consoleReadBytes(buffer + off, buffer_size - off);
    if (got > 0) {
        off += static_cast<size_t>(got);
    }
    return off;
}

extern "C" size_t NRL_USB_Console_Write(const uint8_t *data, const size_t size)
{
    if (!s_installed || data == nullptr || size == 0u) {
        return 0u;
    }
    size_t total = 0;
#if NRL_CONSOLE_UART
    const int wrote = uart_write_bytes((uart_port_t)NRL_CONSOLE_UART_NUM,
                                       reinterpret_cast<const char *>(data), size);
    if (wrote > 0) total = (size_t)wrote;
#else
    const int wrote = usb_serial_jtag_write_bytes(data, size, pdMS_TO_TICKS(100));
    if (wrote > 0) total = (size_t)wrote;
#endif

    return total;
}

extern "C" void NRL_USB_Console_Flush(void)
{
    if (!s_installed) {
        return;
    }
#if NRL_CONSOLE_UART
    uart_wait_tx_done((uart_port_t)NRL_CONSOLE_UART_NUM, pdMS_TO_TICKS(100));
#else
    vTaskDelay(pdMS_TO_TICKS(50));
#endif
}
