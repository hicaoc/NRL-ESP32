#ifndef DRIVER_BOARD_PINS_GEZIPAI_H
#define DRIVER_BOARD_PINS_GEZIPAI_H

#define NRL_PIN_BOOT_BUTTON     0

// SCI radio-control serial. GPIO44/43 are routed to UART1 through the GPIO
// matrix; UART0 is left for the console/AT path.
#define NRL_PIN_SCI_RX          44
#define NRL_PIN_SCI_TX          43
#define NRL_HAS_SCI_SERIAL      1

// GPS NMEA on UART2.
#define NRL_PIN_GPS_RX          8
#define NRL_PIN_GPS_TX          9

// Physical buttons.
#define NRL_HAS_USER_BUTTONS    1
#define NRL_PIN_BTN_VOL_UP      1
#define NRL_PIN_BTN_VOL_DOWN    2
#define NRL_PIN_BTN_PTT         18

// Active-low status LEDs.
#define NRL_PIN_LED_PTT         41
#define NRL_PIN_LED_AUDIO       42
#define NRL_PIN_LED_NET         17

#define NRL_PIN_PA_EN           46

// External-radio PTT keying output: driven high while inbound network voice
// (NRL downlink or FMO call) is being played, keying an attached radio so it
// re-transmits the call on RF. Mirrors the BH4TDV 3188 PTT_OUT behaviour.
#define NRL_PIN_PTT_OUT         14
#define NRL_HAS_ES7210          1
#define NRL_AUDIO_CODEC_ES8311  1
#define NRL_AUDIO_CODEC_ES8389  0
#define NRL_HAS_SDCARD          0
#define NRL_HAS_USB_HOST        0

// I2C bus.
#define NRL_PIN_I2C_SCL         47
#define NRL_PIN_I2C_SDA         38

// ES8311/ES7210 I2S bus.
#define NRL_PIN_I2S_MCLK        40
#define NRL_PIN_I2S_BCLK        39
#define NRL_PIN_I2S_DOUT        48
#define NRL_PIN_I2S_LRCLK       45
#define NRL_PIN_I2S_DIN         21

// ST7789 240x240 SPI LCD.
#define NRL_PIN_DISPLAY_SCLK    7
#define NRL_PIN_DISPLAY_MOSI    6
#define NRL_PIN_DISPLAY_DC      16
#define NRL_PIN_DISPLAY_CS      15
#define NRL_PIN_DISPLAY_RST     5
#define NRL_PIN_DISPLAY_BL      4
#define NRL_DISPLAY_BUS_ST7789  1
#define NRL_DISPLAY_BUS_RGB     0
#define NRL_DISPLAY_WIDTH       240
#define NRL_DISPLAY_HEIGHT      240

// Battery voltage sense -- ADC1 channel 2 (GPIO3), behind a 1:2 divider.
#define NRL_PIN_BATTERY_ADC     3
#define NRL_BATTERY_ADC_CHANNEL ADC_CHANNEL_2
#define NRL_HAS_BATTERY_ADC     1
#define NRL_HAS_DISPLAY         1

#endif // DRIVER_BOARD_PINS_GEZIPAI_H
