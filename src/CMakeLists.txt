set(app_sources
    app/main.cpp
    app/aec/aec_processor.cpp
    app/aec/esp_sr_probe.cpp
    audio/audio_focus.cpp
    audio/audio_router.cpp
    audio/voice_resampler.cpp
    media/cover_decoder.cpp
    media/media_decoder.cpp
    media/media_metadata.cpp
    media/opus_voice.cpp
    services/ai_assistant.cpp
    services/aprs_service.cpp
    services/cellular_rndis_service.cpp
    services/config_notify.cpp
    services/display_notice.cpp
    services/espnow_link.cpp
    services/lan_discovery.cpp
    services/music_player.cpp
    services/music_playlist.cpp
    services/nanny.cpp
    services/ota_service.cpp
    services/radio_favorites.cpp
    services/signaling_service.cpp
    services/smb_stream.cpp
    services/smb_vfs.cpp
    services/storage_service.cpp
    services/video_call.cpp
    app/driver/audio_passthrough.cpp
    app/driver/game_tetris.cpp
    app/driver/fonts/lv_font_cjk_16.c
    app/driver/fonts/lv_font_cjk_20.c
    app/driver/eeprom.cpp
    app/driver/display.cpp
    app/driver/display_bi4umd.cpp
    app/driver/touch_bi4umd.cpp
    app/driver/display_s31.cpp
    app/driver/es7210.cpp
    app/driver/es8311.cpp
    app/driver/es8389.cpp
    app/driver/external_radio.cpp
    app/driver/gps_serial.cpp
    app/driver/i2c.cpp
    app/driver/sci_serial.cpp
    app/driver/serial_port_config.cpp
    app/driver/status_io.cpp
    lib/aprs/aprs_ax25.cpp
    lib/aprs/aprs_crc_ccit.c
    lib/aprs/aprs_modem.cpp
    lib/aprs/parse_aprs.cpp
    lib/ble_config.cpp
    lib/improv_protocol.cpp
    lib/nrl_bt_hfp.cpp
    lib/nrl_ethernet.cpp
    lib/nrl_at_commands.cpp
    lib/nrl_audio_bridge.cpp
    lib/nrl_captive_dns.cpp
    lib/nrl_g711.cpp
    lib/nrl_smb_readv.cpp
    lib/dtmf_codec.cpp
    lib/ctcss_decoder.cpp
    lib/nrl_usb_console.cpp
    lib/nrl_wifi.cpp
    lib/wifi_config_portal.cpp
    lib/wifi_config_portal_view.cpp
)

# LVGL is vendored under components/lvgl/ only for display-equipped boards.
# On bh4tdv and the S31 Function CoreBoard, display sources compile their LVGL
# code out through board capability macros, so we must not declare the
# dependency even if a developer's shared worktree already contains LVGL.
set(app_priv_requires
    app_update
    bt
    driver
    efuse
    esp_adc
    espressif__esp_audio_codec
    espressif__esp_codec_dev
    espressif__esp_new_jpeg
    esp_coex
    esp_driver_gpio
    esp_driver_i2c
    esp_driver_i2s
    esp_driver_spi
    esp_driver_uart
    esp_driver_usb_serial_jtag
    esp_event
    esp_eth
    esp_http_client
    esp_https_ota
    esp_http_server
    mbedtls
    mdc1200
    esp_lcd
    esp_netif
    esp_ringbuf
    esp_partition
    esp_psram
    esp_timer
    esp_wifi
    fatfs
    sdmmc
    espressif__esp-sr
    lwip
    nvs_flash
    spi_flash
)
# BLE provisioning (ble_config.cpp) uses the NimBLE host via esp-nimble-cpp.
# S31 instead uses the Bluedroid host (Classic BT + HFP for a BT headset); the
# two hosts are mutually exclusive, so only pull esp-nimble-cpp on non-S31.
if(NOT IDF_TARGET STREQUAL "esp32s31")
    list(APPEND app_priv_requires h2zero__esp-nimble-cpp)
endif()
if(NOT NRL_BOARD_ID STREQUAL "NRL_BOARD_S31_FUNCTION_COREBOARD" AND
   EXISTS "${CMAKE_CURRENT_LIST_DIR}/../components/lvgl/lvgl.h")
    list(APPEND app_priv_requires lvgl)
endif()
# Korvo's BSP is linked from the top-level CMakeLists after project(), where the
# custom board selection is reliably available. Declaring it here would make
# ESP-IDF's early requirement scan pull the display BSP into every S31 build.
# SMB network playback is available on every board.
list(APPEND app_priv_requires sahlberg__libsmb2 vfs)

# USB flash drive and the S31-only assistant/display dependencies.
if(IDF_TARGET STREQUAL "esp32s31")
    list(APPEND app_priv_requires usb espressif__usb_host_msc espressif__led_strip fatfs
                                  espressif__esp_websocket_client espressif__cjson)
endif()

# Optional USB-RNDIS backend. Current production boards leave this disabled;
# enabling the experimental board macro pulls the driver dependencies in.
if(NRL_BOARD_ID STREQUAL "NRL_BOARD_USB_RNDIS_EXPERIMENT")
    list(APPEND app_priv_requires usb espressif__iot_eth espressif__iot_usbh_cdc
                                  espressif__iot_usbh_rndis)
endif()

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS
        .
        app
        app/driver
    PRIV_REQUIRES ${app_priv_requires}
    # WHOLE_ARCHIVE keeps app_main (and other referenced-by-startup symbols) from
    # being dropped at link time. Under PlatformIO src is the main component
    # (already whole-archived); for the native idf.py build (build-s31 job) src is
    # registered as a plain component via EXTRA_COMPONENT_DIRS, so it needs this
    # explicitly or the link fails with "undefined reference to app_main".
    WHOLE_ARCHIVE
)

# Vendored APRS sources retain upstream warning patterns.
set_source_files_properties(
    lib/aprs/parse_aprs.cpp
    lib/aprs/aprs_ax25.cpp
    lib/aprs/aprs_modem.cpp
    PROPERTIES COMPILE_OPTIONS
    "-Wno-parentheses;-Wno-type-limits;-Wno-implicit-fallthrough;-Wno-unused-variable;-Wno-unused-but-set-variable;-Wno-unused-label;-Wno-unused-value"
)
