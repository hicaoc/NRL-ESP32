#ifndef DRIVER_BOARD_PINS_BH4TDV_H
#define DRIVER_BOARD_PINS_BH4TDV_H

#define NRL_PIN_BOOT_BUTTON     0

// SCI radio-control serial.
#define NRL_PIN_SCI_RX          4
#define NRL_PIN_SCI_TX          5
#define NRL_HAS_SCI_SERIAL      1
#define NRL_PIN_GPS_RX          6
#define NRL_PIN_GPS_TX          7

// Radio control / status.
#define NRL_PIN_PTT_OUT         8
#define NRL_PIN_SQL1            17
#define NRL_PIN_SQL2            18
#define NRL_PIN_STATUS_IO1      1
#define NRL_PIN_STATUS_IO2      2
#define NRL_PIN_STATUS_PTT_LED  42

// Channel selection bits.
#define NRL_PIN_CHANNEL_BIT0    40
#define NRL_PIN_CHANNEL_BIT1    39
#define NRL_PIN_CHANNEL_BIT2    38

#define NRL_PIN_PA_EN           -1
#define NRL_HAS_ES7210          0
#define NRL_AUDIO_CODEC_ES8311  1
#define NRL_AUDIO_CODEC_ES8389  0
#define NRL_HAS_SDCARD          0
#define NRL_HAS_USB_HOST        0
#define NRL_HAS_DISPLAY         0
#define NRL_HAS_SIGNALING       0
#define NRL_HAS_BATTERY_ADC     0

// I2C bus.
#define NRL_PIN_I2C_SCL         14
#define NRL_PIN_I2C_SDA         21

// ES8311 I2S bus.
#define NRL_PIN_I2S_MCLK        9
#define NRL_PIN_I2S_BCLK        10
#define NRL_PIN_I2S_DOUT        13
#define NRL_PIN_I2S_LRCLK       12
#define NRL_PIN_I2S_DIN         11

#endif // DRIVER_BOARD_PINS_BH4TDV_H
