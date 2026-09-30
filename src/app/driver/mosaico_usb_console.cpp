// ESP-Mosaico USB device console: tinyusb CDC-ACM on the Type-C OTG port.
// The board enumerates as a COM port while the app runs, so the same Type-C
// cable that flashes the board also shows ESP_LOG output. Called first thing
// in initApp() so boot logs land on the port as early as possible.

#include "board_pins.h"

#if NRL_BOARD == NRL_BOARD_ESP_MOSAICO

#include "mosaico_usb_console.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <tinyusb.h>
#include <tinyusb_cdc_acm.h>
#include <tinyusb_console.h>
#include <tinyusb_default_config.h>

// Provided by display_mosaico.cpp; weak so the console works without it.
extern "C" void MosaicoDisp_DumpStatus(void) __attribute__((weak));

namespace {

const char *kTag = "MOSAICO_USB";

// The CDC console only streams while a host has the port open, so everything
// printed before the host connects is lost. Dump the display status on every
// DTR assert (terminal open) instead: no power-cycle needed to see the state.
void cdcLineState(int, cdcacm_event_t *event)
{
    if (event->type == CDC_EVENT_LINE_STATE_CHANGED &&
        event->line_state_changed_data.dtr) {
        ESP_LOGI(kTag, "console connected, uptime=%ums",
                 (unsigned)(esp_timer_get_time() / 1000));
        if (MosaicoDisp_DumpStatus != nullptr) {
            MosaicoDisp_DumpStatus();
        }
    }
}

} // namespace

extern "C" bool MOSAICO_USB_CONSOLE_Init(void)
{
    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "tinyusb install failed: %s", esp_err_to_name(err));
        return false;
    }

    const tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = nullptr,
        .callback_rx_wanted_char = nullptr,
        .callback_line_state_changed = cdcLineState,
        .callback_line_coding_changed = nullptr,
    };
    err = tinyusb_cdcacm_init(&acm_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "cdc-acm init failed: %s", esp_err_to_name(err));
        return false;
    }

    // Route stdout/stderr (and therefore ESP_LOGx) to the CDC port.
    err = tinyusb_console_init(TINYUSB_CDC_ACM_0);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "console redirect failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(kTag, "USB CDC console up on the Type-C port");
    return true;
}

#endif // NRL_BOARD == NRL_BOARD_ESP_MOSAICO
