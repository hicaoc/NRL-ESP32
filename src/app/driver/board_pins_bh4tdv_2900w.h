#ifndef DRIVER_BOARD_PINS_BH4TDV_2900W_H
#define DRIVER_BOARD_PINS_BH4TDV_2900W_H

#define NRL_PIN_BOOT_BUTTON     0

// No SCI radio-control or GPS serial on this board: the 3188's SCI/GPS pins
// are reused for the RGB status LEDs and the battery sense.
#define NRL_PIN_SCI_RX          -1
#define NRL_PIN_SCI_TX          -1
#define NRL_HAS_SCI_SERIAL      0
#define NRL_PIN_GPS_RX          -1
#define NRL_PIN_GPS_TX          -1

// Radio control / status. SQL1/SQL2 are squelch inputs from the radio; voice
// is only sent to the network while squelch is active. On this board both
// radios key SQL low (open-drain style), so SQL1 is active-low with a
// pull-up, unlike the 3188's active-high SQL1.
#define NRL_SQL1_ACTIVE_LEVEL   0
#define NRL_PIN_PTT_OUT         39
#define NRL_PIN_SQL1            41
#define NRL_PIN_SQL2            40
#define NRL_PIN_STATUS_IO1      6   // blue LED: server link
#define NRL_PIN_STATUS_IO2      5   // green LED: radio SQL
#define NRL_PIN_STATUS_PTT_LED  4   // red LED: PTT

// Voice loopback test button (to GND): while held, mic audio captured from
// the ES8311 ADC is played straight back through the ES8311 DAC.
#define NRL_PIN_LOOP_TEST_BUTTON 1

// Second radio PTT output. It follows IO39, with an independently
// configurable active level (high by default).
#define NRL_PIN_PTT2_OUT        38
#define NRL_PTT2_ACTIVE_LEVEL   1

// Battery voltage sense -- ADC1 channel 6 (GPIO7).
#define NRL_PIN_BATTERY_ADC     7
#define NRL_BATTERY_ADC_CHANNEL ADC_CHANNEL_6
#define NRL_HAS_BATTERY_ADC     1

// No channel-select GPIOs on this board (the 3188's 40/39/38 are SQL_2,
// PTT_OUT and PTT2_OUT here).
#define NRL_PIN_CHANNEL_BIT0    -1
#define NRL_PIN_CHANNEL_BIT1    -1
#define NRL_PIN_CHANNEL_BIT2    -1

#define NRL_PIN_PA_EN           -1
#define NRL_HAS_ES7210          0
#define NRL_AUDIO_CODEC_ES8311  1
#define NRL_AUDIO_CODEC_ES8389  0
#define NRL_HAS_SDCARD          0
#define NRL_HAS_USB_HOST        0
#define NRL_HAS_DISPLAY         0
#define NRL_HAS_SIGNALING       0

// I2C bus.
#define NRL_PIN_I2C_SCL         14
#define NRL_PIN_I2C_SDA         21

// ES8311 I2S bus. The schematic's I2S_DI/I2S_DO labels are from the codec's
// perspective: I2S_DI (IO9) is data INTO the codec (ESP DOUT), I2S_DO (IO11)
// is data OUT of the codec (ESP DIN).
#define NRL_PIN_I2S_MCLK        13
#define NRL_PIN_I2S_BCLK        12
#define NRL_PIN_I2S_DOUT        9
#define NRL_PIN_I2S_LRCLK       10
#define NRL_PIN_I2S_DIN         11

#endif // DRIVER_BOARD_PINS_BH4TDV_2900W_H
