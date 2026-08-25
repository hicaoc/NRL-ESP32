#ifndef DRIVER_BOARD_PINS_S31_FUNCTION_COREBOARD_H
#define DRIVER_BOARD_PINS_S31_FUNCTION_COREBOARD_H

#define NRL_PIN_BOOT_BUTTON     61

// External radio serial link.
#define NRL_PIN_SCI_RX          43
#define NRL_PIN_SCI_TX          44
#define NRL_HAS_SCI_SERIAL      1
#define NRL_PIN_GPS_RX          45
#define NRL_PIN_GPS_TX          46

// No physical volume/PTT keys.
#define NRL_HAS_USER_BUTTONS    0
#define NRL_PIN_BTN_VOL_UP      -1
#define NRL_PIN_BTN_VOL_DOWN    -1
#define NRL_PIN_BTN_PTT         -1

// Single addressable WS2812 RGB status LED.
#define NRL_HAS_WS2812_STATUS   1
#define NRL_PIN_WS2812_STATUS   60
#define NRL_WS2812_INVERT_OUT   0
#define NRL_PIN_LED_PTT         -1
#define NRL_PIN_LED_AUDIO       -1
#define NRL_PIN_LED_NET         -1

#define NRL_PIN_PA_EN           57
#define NRL_HAS_ES7210          0
#define NRL_AUDIO_CODEC_ES8311  1
#define NRL_AUDIO_CODEC_ES8389  0
#define NRL_HAS_SDCARD          0
#define NRL_HAS_USB_HOST        1

// On-board YT8531DC-CA Gigabit PHY through RGMII.
#define NRL_HAS_ETHERNET        1
#define NRL_PIN_ETH_MDC         5
#define NRL_PIN_ETH_MDIO        6
#define NRL_PIN_ETH_PHY_RESET   7
#define NRL_PIN_ETH_TXD0        8
#define NRL_PIN_ETH_TXD1        9
#define NRL_PIN_ETH_TXD2        10
#define NRL_PIN_ETH_TXD3        11
#define NRL_PIN_ETH_TX_CTL      12
#define NRL_PIN_ETH_TX_CLK      13
#define NRL_PIN_ETH_RX_CLK      14
#define NRL_PIN_ETH_RX_CTL      15
#define NRL_PIN_ETH_RXD3        16
#define NRL_PIN_ETH_RXD2        17
#define NRL_PIN_ETH_RXD1        18
#define NRL_PIN_ETH_RXD0        19

// ES8311 control and full-duplex I2S buses.
#define NRL_PIN_I2C_SCL         50
#define NRL_PIN_I2C_SDA         51
#define NRL_PIN_I2S_MCLK        52
#define NRL_PIN_I2S_BCLK        53
#define NRL_PIN_I2S_DIN         54
#define NRL_PIN_I2S_LRCLK       55
#define NRL_PIN_I2S_DOUT        56

#define NRL_HAS_DISPLAY         0
#define NRL_HAS_SIGNALING       0
#define NRL_HAS_BATTERY_ADC     0

#endif // DRIVER_BOARD_PINS_S31_FUNCTION_COREBOARD_H
