#ifndef DRIVER_BOARD_PINS_ESP_MOSAICO_H
#define DRIVER_BOARD_PINS_ESP_MOSAICO_H

// ESP-Mosaico (ESP32-S31NRV16, CoreBoard V1.0). Pin map extracted from
// docs/ESP-Mosaico/ESP-Mosaico_CoreBoard_V1.0_原理图.pdf.

// BOOT button S4 on GPIO61 (low active).
#define NRL_PIN_BOOT_BUTTON     61

// No external radio / GPS serial wired on the core board (BTB expansion only).
#define NRL_PIN_SCI_RX          -1
#define NRL_PIN_SCI_TX          -1
#define NRL_HAS_SCI_SERIAL      0
#define NRL_PIN_GPS_RX          -1
#define NRL_PIN_GPS_TX          -1

// Single "AI" function button S2 on GPIO7 (low active), mapped to PTT.
#define NRL_HAS_ADC_BUTTONS     0
#define NRL_HAS_USER_BUTTONS    1
#define NRL_PIN_BTN_VOL_UP      -1
#define NRL_PIN_BTN_VOL_DOWN    -1
#define NRL_PIN_BTN_PTT         7

// Orange LED XL-1606UOC on GPIO3, low active (used as NET status LED).
#define NRL_PIN_LED_PTT         -1
#define NRL_PIN_LED_AUDIO       -1
#define NRL_PIN_LED_NET         3

// Vibration motor M1 via BSS138 on GPIO8, high active.
#define NRL_PIN_VIBRATION_MOTOR 8

// Audio: ES8311 codec behind a level shifter on the CODEC_3V3 rail
// (I2C 0x19, not the usual 0x18), NS4150B 3W amp on PA_CTRL (high = on).
// CODEC_PW (GPIO56, high = on) powers the codec LDO and must be enabled
// before any ES8311 I2C traffic.
#define NRL_PIN_CODEC_PW        56
#define NRL_PIN_PA_EN           45
#define NRL_PIN_PA_EN_ACTIVE_LEVEL 1
#define NRL_ES8311_I2C_ADDR     0x19
#define NRL_HAS_ES7210          0
#define NRL_AUDIO_CODEC_ES8311  1
#define NRL_AUDIO_CODEC_ES8389  0

// No SD card slot, no USB host. Mass storage comes from the SPI NAND
// (GD5F1GM7, wired to the SD/MSPI pads) once NAND support lands.
#define NRL_HAS_SDCARD          0
#define NRL_HAS_USB_HOST        0

// Single shared I2C bus: ES8311(0x19, level-shifted), CST9220(0x5A),
// BMI270(0x69), BMM150 #2/#3 (0x11/0x12), BQ27220(0x55).
#define NRL_PIN_I2C_SCL         1
#define NRL_PIN_I2C_SDA         0
#define NRL_BMI270_I2C_ADDR     0x69
#define NRL_BMM150_I2C_ADDR_2   0x11
#define NRL_BMM150_I2C_ADDR_3   0x12
#define NRL_BQ27220_I2C_ADDR    0x55
// IMU/BMM interrupt lines are diode-OR'd onto GPIO2.
#define NRL_PIN_SENSOR_INT      2

// ES8311 I2S bus. NOTE: the schematic names are codec-centric; GPIO52 is
// the MCU's I2S output (codec DSDIN), GPIO40 the MCU input (codec ASDOUT).
#define NRL_PIN_I2S_MCLK        54
#define NRL_PIN_I2S_BCLK        37
#define NRL_PIN_I2S_DOUT        52
#define NRL_PIN_I2S_LRCLK       49
#define NRL_PIN_I2S_DIN         40

// 480x480 QSPI AMOLED (CO5300) + CST9220 touch. LCD_RST and TP_RST share
// GPIO42. The LCD connector is powered from the VCC_3V3 rail, enabled by
// pulling VCC_PW (GPIO60) low. AMOLED has no backlight pin; brightness is
// a CO5300 command.
#define NRL_HAS_DISPLAY         1
#define NRL_DISPLAY_BUS_ST7789  0
#define NRL_DISPLAY_BUS_ILI9341 0
#define NRL_DISPLAY_BUS_RGB     0
#define NRL_DISPLAY_BUS_QSPI    1
#define NRL_DISPLAY_WIDTH       480
#define NRL_DISPLAY_HEIGHT      480
#define NRL_PIN_LCD_QSPI_CLK    44
#define NRL_PIN_LCD_QSPI_CS     50
#define NRL_PIN_LCD_QSPI_D0     36
#define NRL_PIN_LCD_QSPI_D1     51
#define NRL_PIN_LCD_QSPI_D2     35
#define NRL_PIN_LCD_QSPI_D3     9
#define NRL_PIN_LCD_TE          43
#define NRL_PIN_LCD_RST         42
#define NRL_PIN_LCD_PWR_EN      60
#define NRL_PIN_LCD_PWR_EN_ACTIVE_LEVEL 0
#define NRL_PIN_DISPLAY_BL      -1
#define NRL_HAS_TOUCH           1
#define NRL_TOUCH_I2C_ADDR      0x5A
#define NRL_PIN_TOUCH_INT       6
#define NRL_PIN_TOUCH_RST       42

// BQ27220 fuel gauge instead of an ADC divider.
#define NRL_HAS_BATTERY_ADC     0
#define NRL_HAS_BATTERY_GAUGE_BQ27220 1

// POWER_SWITCH (SAM8108 on/off controller + power button) on GPIO57.
#define NRL_PIN_POWER_SWITCH    57

// V1.2 hardware (eFuse USER_DATA 0x0101/0x0102) moves the shared I2C bus and
// swaps the LCD clock/reset pins; handled at runtime by mosaico_variant.cpp.
#define NRL_PIN_I2C_SDA_V1_2      56
#define NRL_PIN_I2C_SCL_V1_2      3
#define NRL_PIN_LCD_QSPI_CLK_V1_2 42
#define NRL_PIN_LCD_RST_V1_2      44

#endif // DRIVER_BOARD_PINS_ESP_MOSAICO_H
