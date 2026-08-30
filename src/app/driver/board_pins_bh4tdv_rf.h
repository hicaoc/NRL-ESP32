#ifndef DRIVER_BOARD_PINS_BH4TDV_RF_H
#define DRIVER_BOARD_PINS_BH4TDV_RF_H

// BI4UMD main board plus the NRL companion RF expansion board.
#define NRL_PIN_BOOT_BUTTON     0

// SR-110U is on UART1; the proven UMD GPS wiring moves to UART2 on GPIO2/3.
#define NRL_PIN_SCI_RX          44
#define NRL_PIN_SCI_TX          43
#define NRL_HAS_SCI_SERIAL      1
#define NRL_HAS_SCI_BRIDGE      0
#define NRL_PIN_GPS_RX          2
#define NRL_PIN_GPS_TX          3

// F1 is a direct active-low PTT key. The other five keys are on PCA9555.
#define NRL_PIN_BTN_VOL_UP      -1
#define NRL_PIN_BTN_VOL_DOWN    -1
#define NRL_PIN_BTN_PTT         21
#define NRL_PIN_PCA9555_INT     14
#define NRL_HAS_PCA9555         1
#define NRL_HAS_SR110U          1
#define NRL_HAS_RF_KEYS         1
#define NRL_PIN_LED_PTT         -1
#define NRL_PIN_LED_AUDIO       -1
#define NRL_PIN_LED_NET         -1

// Single XL-5050RGBC-WS2812B RGB LED on the companion mainboard, mirroring
// the PCA9555 status lamps (NET/SQL/PTT). Data in on GPIO42 = module
// physical pin 48 (MTMS).
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
#define NRL_BMP280_I2C_ADDR     0x76
#define NRL_QMC5883L_I2C_ADDR   0x0D
// Datasheets sometimes write this as the 8-bit write address 0x46.
// ESP-IDF expects the unshifted 7-bit address: 0x46 >> 1 = 0x23.
#define NRL_BH1750_I2C_ADDR     0x23
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

#endif // DRIVER_BOARD_PINS_BH4TDV_RF_H
