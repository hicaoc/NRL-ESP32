#ifndef DRIVER_BOARD_PINS_BI4UMD_H
#define DRIVER_BOARD_PINS_BI4UMD_H

// USER CUSTOM BEGIN: BI4UMD hardware adaptation.
#define NRL_PIN_BOOT_BUTTON     0
#define NRL_PIN_SCI_RX          2
#define NRL_PIN_SCI_TX          3
#define NRL_HAS_SCI_SERIAL      1
#define NRL_PIN_GPS_RX          44
#define NRL_PIN_GPS_TX          43

#define NRL_PIN_BTN_VOL_UP      -1
#define NRL_PIN_BTN_VOL_DOWN    14
#define NRL_PIN_BTN_PTT         21
#define NRL_PIN_LED_PTT         -1
#define NRL_PIN_LED_AUDIO       -1
#define NRL_PIN_LED_NET         -1

// Single XL-5050RGBC-WS2812B RGB LED on the display mainboard. Data in on
// GPIO42 = module physical pin 48 (MTMS). Shows R=TX, G=network audio,
// B=server link (slow blink while the heartbeat is missing), like S31.
#define NRL_PIN_WS2812_STATUS   42
#define NRL_WS2812_INVERT_OUT   0

#define NRL_PIN_PA_EN           1
#define NRL_PIN_PA_EN_ACTIVE_LEVEL 0
#define NRL_HAS_ES7210          0
#define NRL_AUDIO_CODEC_ES8311  1
#define NRL_AUDIO_CODEC_ES8389  0

#define NRL_HAS_SDCARD          1
#define NRL_SDCARD_NATIVE_SDMMC 1
#define NRL_PIN_SDCARD_CLK      38
#define NRL_PIN_SDCARD_CMD      40
#define NRL_PIN_SDCARD_D0       39
#define NRL_PIN_SDCARD_D1       41
#define NRL_PIN_SDCARD_D2       48
#define NRL_PIN_SDCARD_D3       47
#define NRL_HAS_USB_HOST        0

#define NRL_PIN_I2C_SCL         15
#define NRL_PIN_I2C_SDA         16
#define NRL_HAS_TOUCH           1
#define NRL_TOUCH_I2C_ADDR      0x38
#define NRL_PIN_TOUCH_INT       17
#define NRL_PIN_TOUCH_RST       18

#define NRL_PIN_I2S_MCLK        4
#define NRL_PIN_I2S_BCLK        5
#define NRL_PIN_I2S_DOUT        8
#define NRL_PIN_I2S_LRCLK       7
#define NRL_PIN_I2S_DIN         6

#define NRL_PIN_DISPLAY_SCLK    12
#define NRL_PIN_DISPLAY_MOSI    11
#define NRL_PIN_DISPLAY_MISO    13
#define NRL_PIN_DISPLAY_CS      10
#define NRL_PIN_DISPLAY_RST     -1
#define NRL_PIN_DISPLAY_BL      45
#define NRL_PIN_DISPLAY_DC      46
#define NRL_DISPLAY_BUS_ST7789  0
#define NRL_DISPLAY_BUS_ILI9341 1
#define NRL_DISPLAY_BUS_RGB     0
#define NRL_DISPLAY_WIDTH       240
#define NRL_DISPLAY_HEIGHT      320
#define NRL_HAS_DISPLAY         1

#define NRL_PIN_BATTERY_ADC     9
#define NRL_BATTERY_ADC_CHANNEL ADC_CHANNEL_8
#define NRL_HAS_BATTERY_ADC     1
// USER CUSTOM END: BI4UMD hardware adaptation.

#endif // DRIVER_BOARD_PINS_BI4UMD_H
