/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "bsp_adc_calibration.h"
#include "bsp/esp32_s31_korvo.h"
#include "button_interface.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/sdmmc_host.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_cam_ctlr_dvp.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt1151.h"
#include "esp_log.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "led_strip.h"

static const char *TAG = "S31-Korvo";

static esp_lcd_panel_handle_t s_lcd_panel;
static bsp_display_config_t s_lcd_config;
static bool s_lcd_config_valid;
static lv_display_t *s_lvgl_display;
static i2c_master_bus_handle_t s_i2c_bus;
static esp_lcd_panel_io_handle_t s_touch_io;
static esp_lcd_touch_handle_t s_touch;
static lv_indev_t *s_touch_indev;
static i2s_chan_handle_t s_audio_tx_chan;
static i2s_chan_handle_t s_audio_rx_chan;
static const audio_codec_data_if_t *s_audio_data_if;

/* Button ADC ladder on GPIO42 / ADC1_CH0_N (BT_ARRAY_ADC). */
static const uint16_t s_button_voltage_center_mv[BSP_BUTTON_NUM] = {
    [BSP_BUTTON_VOLUP] = 380,
    [BSP_BUTTON_VOLDOWN] = 820,
    [BSP_BUTTON_MODE] = 1340,
    [BSP_BUTTON_SET] = 1870,
};
static const uint16_t s_button_idle_voltage_mv = 2000;

#define BSP_BUTTON_ADC_SAMPLE_COUNT (4)

typedef struct {
    button_driver_t base;
    uint8_t index;
    uint16_t min_mv;
    uint16_t max_mv;
} bsp_adc_button_t;

static adc_oneshot_unit_handle_t s_button_adc_handle;
static bool s_button_adc_initialized;
static bsp_adc_button_t s_button_driver[BSP_BUTTON_NUM];
static led_strip_handle_t s_led_strip;
static sdmmc_card_t *s_sdcard;
static const char *s_sdcard_mount_point = BSP_SDCARD_MOUNT_POINT;
static bool s_sdcard_power_gpio_initialized;

struct bsp_camera_t {
    void *video_buffer[BSP_CAMERA_BUFFER_COUNT];
    size_t video_buffer_length[BSP_CAMERA_BUFFER_COUNT];
    uint32_t frame_size;
    bool started;
    bool first_frame_logged;
    bool video_initialized;
    int video_fd;
    bsp_camera_config_t config;
    bsp_camera_format_t format;
    struct v4l2_buffer dqbuf;
};

/* ==========================================================================
 * Private Functions
 * ========================================================================== */

static bsp_display_config_t bsp_display_get_default_config(void)
{
    const bsp_display_config_t config = BSP_DISPLAY_DEFAULT_CONFIG();
    return config;
}

static esp_err_t bsp_config_output_gpio(gpio_num_t gpio_num, int level)
{
    if (gpio_num == GPIO_NUM_NC) {
        return ESP_OK;
    }

    const gpio_config_t config = {
        .pin_bit_mask = BIT64(gpio_num),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "configure GPIO %d failed", (int)gpio_num);
    ESP_RETURN_ON_ERROR(gpio_set_level(gpio_num, level), TAG, "set GPIO %d failed", (int)gpio_num);
    return ESP_OK;
}

static void bsp_display_fill_data_gpio_nums(gpio_num_t data_gpio_nums[ESP_LCD_RGB_BUS_WIDTH_MAX])
{
    const gpio_num_t pins[ESP_LCD_RGB_BUS_WIDTH_MAX] = {
        BSP_LCD_DATA0,
        BSP_LCD_DATA1,
        BSP_LCD_DATA2,
        BSP_LCD_DATA3,
        BSP_LCD_DATA4,
        BSP_LCD_DATA5,
        BSP_LCD_DATA6,
        BSP_LCD_DATA7,
        BSP_LCD_DATA8,
        BSP_LCD_DATA9,
        BSP_LCD_DATA10,
        BSP_LCD_DATA11,
        BSP_LCD_DATA12,
        BSP_LCD_DATA13,
        BSP_LCD_DATA14,
        BSP_LCD_DATA15,
        GPIO_NUM_NC,
        GPIO_NUM_NC,
        GPIO_NUM_NC,
        GPIO_NUM_NC,
        GPIO_NUM_NC,
        GPIO_NUM_NC,
        GPIO_NUM_NC,
        GPIO_NUM_NC,
    };

    memcpy(data_gpio_nums, pins, sizeof(pins));
}

static void bsp_touch_get_rotation_flags(esp_lv_adapter_rotation_t rotation,
                                         bool *swap_xy,
                                         bool *mirror_x,
                                         bool *mirror_y)
{
    bool swap = false;
    bool x_mirror = false;
    bool y_mirror = false;

    switch (rotation) {
    case ESP_LV_ADAPTER_ROTATE_90:
        swap = true;
        x_mirror = true;
        break;
    case ESP_LV_ADAPTER_ROTATE_180:
        x_mirror = true;
        y_mirror = true;
        break;
    case ESP_LV_ADAPTER_ROTATE_270:
        swap = true;
        y_mirror = true;
        break;
    case ESP_LV_ADAPTER_ROTATE_0:
    default:
        break;
    }

    if (swap_xy) {
        *swap_xy = swap;
    }
    if (mirror_x) {
        *mirror_x = x_mirror;
    }
    if (mirror_y) {
        *mirror_y = y_mirror;
    }
}

static esp_err_t bsp_i2c_init(void)
{
    if (s_i2c_bus) {
        return ESP_OK;
    }

    // The application may already own this port (the NRL firmware creates
    // the shared touch/codec bus itself via s31_i2c.cpp before any BSP
    // camera call); creating a second master on the same port fails. Adopt
    // the existing bus handle instead -- the i2c_master driver serialises
    // transactions per bus, so camera SCCB traffic coexists with touch.
    if (i2c_master_get_bus_handle(I2C_NUM_0, &s_i2c_bus) == ESP_OK && s_i2c_bus) {
        ESP_LOGI(TAG, "I2C bus adopted from application (port 0)");
        return ESP_OK;
    }

    const i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .i2c_port = I2C_NUM_0,
        .flags.enable_internal_pullup = true,
    };

    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_bus_config, &s_i2c_bus),
                        TAG, "create I2C bus failed");
    ESP_LOGI(TAG, "I2C bus initialized: SDA=%d, SCL=%d", BSP_I2C_SDA, BSP_I2C_SCL);
    return ESP_OK;
}

static i2s_std_config_t bsp_audio_default_i2s_config(void)
{
    i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BSP_AUDIO_DEFAULT_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(BSP_AUDIO_DEFAULT_BITS_PER_SAMPLE, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_AUDIO_I2S_MCLK,
            .bclk = BSP_AUDIO_I2S_SCLK,
            .ws = BSP_AUDIO_I2S_LRCLK,
            .dout = BSP_AUDIO_I2S_SDOUT,
            .din = BSP_AUDIO_I2S_DSIN,
        },
    };

    return config;
}

/* ==========================================================================
 * Audio
 * ========================================================================== */

esp_err_t bsp_audio_init(const i2s_std_config_t *i2s_config)
{
    if (s_audio_data_if) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    /* Deep TX DMA buffering: the RGB panel + LVGL FULL-mode render bursts
     * (~80-106 ms) saturate the PSRAM bus once a second (top-bar clock),
     * and the default 6x240 frames (= 33 ms at 44.1 kHz) of I2S buffering
     * underruns right through it -- audible as strictly periodic stutter
     * during hi-fi playback. 8x735 frames = 133 ms at 44.1 kHz rides it
     * out (~24 KB DMA RAM per direction). The voice path only queues
     * real-time 20 ms chunks, so its latency is unaffected.
     * NOTE: do NOT cut this to free RAM for Bluetooth -- this codec I2S is
     * created lazily when audio first plays, well after BT is enabled, so
     * trimming it does nothing for the BT-enable internal-RAM budget; it only
     * starves the audio path (measured: cutting to 4x caused SCO underrun,
     * core-1 pegged at 100%, and silent BT audio). Free BT RAM elsewhere. */
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 735;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_audio_tx_chan, &s_audio_rx_chan),
                        TAG, "create I2S channels failed");

    i2s_std_config_t default_config = bsp_audio_default_i2s_config();
    const i2s_std_config_t *active_config = i2s_config ? i2s_config : &default_config;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_audio_tx_chan, active_config),
                        TAG, "init I2S TX std mode failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_audio_rx_chan, active_config),
                        TAG, "init I2S RX std mode failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_audio_tx_chan), TAG, "enable I2S TX failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_audio_rx_chan), TAG, "enable I2S RX failed");

    audio_codec_i2s_cfg_t i2s_data_cfg = {
        .port = I2S_NUM_0,
        .tx_handle = s_audio_tx_chan,
        .rx_handle = s_audio_rx_chan,
    };
    s_audio_data_if = audio_codec_new_i2s_data(&i2s_data_cfg);
    ESP_RETURN_ON_FALSE(s_audio_data_if, ESP_FAIL, TAG, "create audio I2S data interface failed");

    ESP_LOGI(TAG, "Audio I2S initialized");
    return ESP_OK;
}

static esp_codec_dev_handle_t bsp_audio_codec_init(esp_codec_dev_type_t dev_type,
                                                   int16_t pa_pin)
{
    esp_err_t ret = bsp_i2c_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed (%s)", esp_err_to_name(ret));
        return NULL;
    }

    ret = bsp_audio_init(NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed (%s)", esp_err_to_name(ret));
        return NULL;
    }

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio_if, NULL, TAG, "create codec GPIO interface failed");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .addr = ES8389_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c_bus,
        .clock_speed_hz = 0,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(ctrl_if, NULL, TAG, "create codec I2C control interface failed");

    es8389_codec_cfg_t es8389_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .sys_cfg = {
            .is_master = false,
            .no_mclk = true,
        },
        .adc_cfg = {
            .digital_mic = false,
        },
        .dac_cfg = {
            .ref_enable = false,
        },
        .pa_cfg = {
            .pa_pin = pa_pin,
            .pa_active_low = false,
            .hw_gain = {
                .pa_voltage = 5.0f,
                .codec_dac_voltage = 3.3f,
            },
        },
    };
    const audio_codec_if_t *codec_if = es8389_codec_new(&es8389_cfg);
    ESP_RETURN_ON_FALSE(codec_if, NULL, TAG, "create ES8389 codec interface failed");

    esp_codec_dev_cfg_t codec_dev_cfg = {
        .dev_type = dev_type,
        .codec_if = codec_if,
        .data_if = s_audio_data_if,
    };

    return esp_codec_dev_new(&codec_dev_cfg);
}

esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void)
{
    return bsp_audio_codec_init(ESP_CODEC_DEV_TYPE_OUT, BSP_AUDIO_PA_CTRL);
}

esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void)
{
    return bsp_audio_codec_init(ESP_CODEC_DEV_TYPE_IN, GPIO_NUM_NC);
}

/* ==========================================================================
 * Button
 * ========================================================================== */

static uint16_t bsp_button_mv_midpoint(uint16_t a, uint16_t b)
{
    return (uint16_t)(((uint32_t)a + (uint32_t)b) / 2U);
}

static esp_err_t bsp_button_adc_init(void)
{
    if (s_button_adc_initialized) {
        return ESP_OK;
    }

    const adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_config, &s_button_adc_handle),
                        TAG, "create button ADC unit failed");

    const adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_0,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_button_adc_handle, ADC_CHANNEL_0, &channel_config),
                        TAG, "configure button ADC channel failed");

    ESP_RETURN_ON_ERROR(bsp_s31_adc_calibration_init(s_button_adc_handle, ADC_UNIT_1, ADC_CHANNEL_0),
                        TAG, "button ADC software calibration failed");

    s_button_adc_initialized = true;
    ESP_LOGI(TAG, "Button ADC initialized on ADC1_CH0 with S31 software calibration");
    return ESP_OK;
}

static uint8_t bsp_button_get_key_level(button_driver_t *button_driver)
{
    bsp_adc_button_t *button = (bsp_adc_button_t *)button_driver;
    uint32_t raw_sum = 0;
    int raw = 0;

    for (int i = 0; i < BSP_BUTTON_ADC_SAMPLE_COUNT; i++) {
        if (adc_oneshot_read(s_button_adc_handle, ADC_CHANNEL_0, &raw) != ESP_OK) {
            return BUTTON_INACTIVE;
        }
        raw_sum += raw;
    }
    raw = raw_sum / BSP_BUTTON_ADC_SAMPLE_COUNT;

    int voltage_mv = 0;
    if (bsp_s31_adc_calibration_raw_to_mv(ADC_UNIT_1, raw, &voltage_mv) != ESP_OK) {
        return BUTTON_INACTIVE;
    }
    return (voltage_mv >= button->min_mv && voltage_mv <= button->max_mv) ? BUTTON_ACTIVE : BUTTON_INACTIVE;
}

esp_err_t bsp_iot_button_create(button_handle_t btn_array[], int *btn_cnt, int btn_array_size)
{
    ESP_RETURN_ON_FALSE(btn_array && btn_array_size >= BSP_BUTTON_NUM,
                        ESP_ERR_INVALID_ARG, TAG, "invalid button output array");

    ESP_RETURN_ON_ERROR(bsp_button_adc_init(), TAG, "button ADC init failed");

    const button_config_t button_config = {0};

    esp_err_t ret = ESP_OK;
    for (int i = 0; i < BSP_BUTTON_NUM; i++) {
        s_button_driver[i].base.get_key_level = bsp_button_get_key_level;
        s_button_driver[i].index = i;

        if (i == 0) {
            uint16_t upper = bsp_button_mv_midpoint(s_button_voltage_center_mv[i], s_button_voltage_center_mv[i + 1]);
            uint16_t margin = upper - s_button_voltage_center_mv[i];
            s_button_driver[i].min_mv = s_button_voltage_center_mv[i] > margin ?
                                        s_button_voltage_center_mv[i] - margin : 0;
            s_button_driver[i].max_mv = upper;
        } else if (i == BSP_BUTTON_NUM - 1) {
            uint16_t lower = bsp_button_mv_midpoint(s_button_voltage_center_mv[i - 1], s_button_voltage_center_mv[i]);
            s_button_driver[i].min_mv = lower;
            s_button_driver[i].max_mv = bsp_button_mv_midpoint(s_button_voltage_center_mv[i], s_button_idle_voltage_mv);
        } else {
            s_button_driver[i].min_mv = bsp_button_mv_midpoint(s_button_voltage_center_mv[i - 1],
                                                               s_button_voltage_center_mv[i]);
            s_button_driver[i].max_mv = bsp_button_mv_midpoint(s_button_voltage_center_mv[i],
                                                               s_button_voltage_center_mv[i + 1]);
        }

        ESP_LOGI(TAG, "Button %d threshold: center=%u mV, range=%u-%u mV",
                 i,
                 s_button_voltage_center_mv[i],
                 s_button_driver[i].min_mv,
                 s_button_driver[i].max_mv);
        ret |= iot_button_create(&button_config, &s_button_driver[i].base, &btn_array[i]);
    }

    if (btn_cnt) {
        *btn_cnt = BSP_BUTTON_NUM;
    }
    return ret;
}

/* ==========================================================================
 * LED
 * ========================================================================== */

esp_err_t bsp_led_init(void)
{
    if (s_led_strip) {
        return ESP_OK;
    }

    const led_strip_config_t strip_config = {
        .strip_gpio_num = BSP_LED_WS2812,
        .max_leds = BSP_LED_NUM,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        },
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags = {
            .with_dma = false,
        },
    };

    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led_strip),
                        TAG, "create WS2812 LED strip failed");
    ESP_RETURN_ON_ERROR(led_strip_clear(s_led_strip), TAG, "clear WS2812 LED failed");
    ESP_LOGI(TAG, "WS2812 LED initialized on GPIO%d", BSP_LED_WS2812);
    return ESP_OK;
}

esp_err_t bsp_led_set_rgb(bsp_led_t led, uint8_t red, uint8_t green, uint8_t blue)
{
    ESP_RETURN_ON_FALSE(led < BSP_LED_NUM, ESP_ERR_INVALID_ARG, TAG, "invalid LED index");
    ESP_RETURN_ON_ERROR(bsp_led_init(), TAG, "LED init failed");
    ESP_RETURN_ON_ERROR(led_strip_set_pixel(s_led_strip, led, red, green, blue),
                        TAG, "set LED pixel failed");
    return led_strip_refresh(s_led_strip);
}

esp_err_t bsp_led_clear(void)
{
    ESP_RETURN_ON_ERROR(bsp_led_init(), TAG, "LED init failed");
    return led_strip_clear(s_led_strip);
}

/* ==========================================================================
 * Touch
 * ========================================================================== */

esp_err_t bsp_touch_new(esp_lv_adapter_rotation_t rotation, esp_lcd_touch_handle_t *ret_touch)
{
    ESP_RETURN_ON_FALSE(ret_touch, ESP_ERR_INVALID_ARG, TAG, "touch handle output is null");

    if (s_touch) {
        *ret_touch = s_touch;
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "I2C init failed");

    bool swap_xy = false;
    bool mirror_x = false;
    bool mirror_y = false;
    bsp_touch_get_rotation_flags(rotation, &swap_xy, &mirror_x, &mirror_y);

    const esp_lcd_touch_config_t touch_config = {
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = BSP_LCD_TOUCH_RST,
        .int_gpio_num = BSP_LCD_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
    };

    esp_lcd_panel_io_i2c_config_t touch_io_config = ESP_LCD_TOUCH_IO_I2C_GT1151_CONFIG();
    touch_io_config.scl_speed_hz = 400000;

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c_bus, &touch_io_config, &s_touch_io),
                        TAG, "create GT1151 panel IO failed");
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_gt1151(s_touch_io, &touch_config, &s_touch),
                        TAG, "create GT1151 touch failed");

    *ret_touch = s_touch;
    ESP_LOGI(TAG, "GT1151 touch initialized");
    return ESP_OK;
}

/* ==========================================================================
 * Camera
 * ========================================================================== */

static bsp_camera_config_t bsp_camera_get_default_config(void)
{
    const bsp_camera_config_t config = BSP_CAMERA_DEFAULT_CONFIG();
    return config;
}

static esp_err_t bsp_camera_video_ioctl(int fd, unsigned long request, void *arg, const char *name)
{
    int ret = ioctl(fd, request, arg);
    ESP_RETURN_ON_FALSE(ret == 0, ESP_FAIL, TAG, "esp-video %s failed", name);
    return ESP_OK;
}

static uint32_t bsp_camera_to_v4l2_pixel_format(bsp_camera_pixel_format_t pixel_format)
{
    switch (pixel_format) {
    case BSP_CAMERA_PIXEL_FORMAT_RGB565_BE:
        return V4L2_PIX_FMT_RGB565X;
    case BSP_CAMERA_PIXEL_FORMAT_RGB565:
        return V4L2_PIX_FMT_RGB565;
    case BSP_CAMERA_PIXEL_FORMAT_JPEG:
    default:
        return V4L2_PIX_FMT_JPEG;
    }
}

static void bsp_camera_fill_dvp_pins(esp_cam_ctlr_dvp_pin_config_t *pin_cfg)
{
    *pin_cfg = (esp_cam_ctlr_dvp_pin_config_t) {
        .data_width = 8,
        .data_io = {
            BSP_CAMERA_D0,
            BSP_CAMERA_D1,
            BSP_CAMERA_D2,
            BSP_CAMERA_D3,
            BSP_CAMERA_D4,
            BSP_CAMERA_D5,
            BSP_CAMERA_D6,
            BSP_CAMERA_D7,
        },
        .vsync_io = BSP_CAMERA_VSYNC,
        .de_io = BSP_CAMERA_HSYNC,
        .pclk_io = BSP_CAMERA_PCLK,
        .xclk_io = BSP_CAMERA_XCLK,
    };
}

static esp_err_t bsp_camera_video_init(bsp_camera_t *camera)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "camera shared I2C init failed");

    esp_cam_ctlr_dvp_pin_config_t dvp_pin = {0};
    bsp_camera_fill_dvp_pins(&dvp_pin);

    esp_video_init_dvp_config_t dvp_config = {
        .sccb_config = {
            .init_sccb = false,
            .i2c_handle = s_i2c_bus,
            .freq = BSP_CAMERA_DEFAULT_SCCB_FREQ_HZ,
        },
        .reset_pin = BSP_CAMERA_RESET,
        .pwdn_pin = BSP_CAMERA_PWDN,
        .dvp_pin = dvp_pin,
        .xclk_freq = camera->config.xclk_freq_hz,
    };
    esp_video_init_config_t video_config = {
        .dvp = &dvp_config,
    };

    ESP_LOGI(TAG, "Initializing camera: dev=%s, shared_i2c=1, xclk=%"PRIu32,
             ESP_VIDEO_DVP_DEVICE_NAME, camera->config.xclk_freq_hz);
    ESP_RETURN_ON_ERROR(esp_video_init_with_flags(&video_config, ESP_VIDEO_INIT_FLAGS_DVP),
                        TAG, "esp-video DVP init failed");
    camera->video_initialized = true;

    camera->video_fd = open(ESP_VIDEO_DVP_DEVICE_NAME, O_RDWR);
    ESP_RETURN_ON_FALSE(camera->video_fd >= 0, ESP_FAIL, TAG, "open %s failed", ESP_VIDEO_DVP_DEVICE_NAME);

    struct v4l2_format format = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .fmt.pix = {
            .width = camera->config.width,
            .height = camera->config.height,
            .pixelformat = bsp_camera_to_v4l2_pixel_format(camera->config.pixel_format),
        },
    };
    ESP_RETURN_ON_ERROR(bsp_camera_video_ioctl(camera->video_fd, VIDIOC_S_FMT, &format, "S_FMT"),
                        TAG, "esp-video set format failed");

    camera->format.width = format.fmt.pix.width;
    camera->format.height = format.fmt.pix.height;
    camera->format.pixelformat = format.fmt.pix.pixelformat;
    camera->format.bytesperline = format.fmt.pix.bytesperline;
    camera->format.sizeimage = format.fmt.pix.sizeimage;
    camera->frame_size = format.fmt.pix.sizeimage;
    ESP_LOGI(TAG, "Camera format: %"PRIu32"x%"PRIu32", fourcc=%c%c%c%c, stride=%"PRIu32", size=%"PRIu32,
             camera->format.width,
             camera->format.height,
             (char)(camera->format.pixelformat & 0xff),
             (char)((camera->format.pixelformat >> 8) & 0xff),
             (char)((camera->format.pixelformat >> 16) & 0xff),
             (char)((camera->format.pixelformat >> 24) & 0xff),
             camera->format.bytesperline,
             camera->format.sizeimage);

    struct v4l2_requestbuffers req = {
        .count = BSP_CAMERA_BUFFER_COUNT,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    ESP_RETURN_ON_ERROR(bsp_camera_video_ioctl(camera->video_fd, VIDIOC_REQBUFS, &req, "REQBUFS"),
                        TAG, "esp-video request buffers failed");
    ESP_RETURN_ON_FALSE(req.count > 0 && req.count <= BSP_CAMERA_BUFFER_COUNT,
                        ESP_FAIL, TAG, "unexpected esp-video buffer count: %"PRIu32, req.count);

    for (uint32_t i = 0; i < req.count; i++) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
        };
        ESP_RETURN_ON_ERROR(bsp_camera_video_ioctl(camera->video_fd, VIDIOC_QUERYBUF, &buf, "QUERYBUF"),
                            TAG, "esp-video query buffer failed");
        void *mapped_buffer = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                                   camera->video_fd, buf.m.offset);
        ESP_RETURN_ON_FALSE(mapped_buffer && mapped_buffer != MAP_FAILED,
                            ESP_FAIL, TAG, "esp-video mmap buffer %"PRIu32" failed", i);
        camera->video_buffer[i] = mapped_buffer;
        camera->video_buffer_length[i] = buf.length;
        ESP_RETURN_ON_ERROR(bsp_camera_video_ioctl(camera->video_fd, VIDIOC_QBUF, &buf, "QBUF"),
                            TAG, "esp-video queue buffer failed");
        ESP_LOGI(TAG, "Camera buffer[%"PRIu32"]: ptr=%p, length=%"PRIu32,
                 i, camera->video_buffer[i], buf.length);
    }

    return ESP_OK;
}

esp_err_t bsp_camera_open(const bsp_camera_config_t *config, bsp_camera_t **ret_camera)
{
    ESP_RETURN_ON_FALSE(ret_camera, ESP_ERR_INVALID_ARG, TAG, "camera handle output is null");
    *ret_camera = NULL;

    bsp_camera_config_t active_config = config ? *config : bsp_camera_get_default_config();
    if (active_config.width == 0) {
        active_config.width = BSP_CAMERA_DEFAULT_WIDTH;
    }
    if (active_config.height == 0) {
        active_config.height = BSP_CAMERA_DEFAULT_HEIGHT;
    }
    if (active_config.xclk_freq_hz == 0) {
        active_config.xclk_freq_hz = BSP_CAMERA_DEFAULT_XCLK_FREQ_HZ;
    }

    bsp_camera_t *camera = heap_caps_calloc(1, sizeof(*camera), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(camera, ESP_ERR_NO_MEM, TAG, "no memory for camera context");
    camera->config = active_config;
    camera->video_fd = -1;

    esp_err_t ret = bsp_camera_video_init(camera);
    ESP_GOTO_ON_ERROR(ret, error, TAG, "esp-video camera init failed");

    *ret_camera = camera;
    return ESP_OK;

error:
    bsp_camera_close(camera);
    return ret;
}

esp_err_t bsp_camera_start(bsp_camera_t *camera)
{
    ESP_RETURN_ON_FALSE(camera, ESP_ERR_INVALID_ARG, TAG, "camera handle is null");
    if (camera->started) {
        return ESP_OK;
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_ERROR(bsp_camera_video_ioctl(camera->video_fd, VIDIOC_STREAMON, &type, "STREAMON"),
                        TAG, "esp-video stream on failed");
    camera->started = true;
    ESP_LOGI(TAG, "Camera stream started");
    return ESP_OK;
}

esp_err_t bsp_camera_set_jpeg_quality(bsp_camera_t *camera, int quality)
{
    ESP_RETURN_ON_FALSE(camera, ESP_ERR_INVALID_ARG, TAG, "camera handle is null");
    (void)quality;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_camera_set_orientation(bsp_camera_t *camera, bool horizontal_mirror, bool vertical_flip)
{
    ESP_RETURN_ON_FALSE(camera, ESP_ERR_INVALID_ARG, TAG, "camera handle is null");
    (void)horizontal_mirror;
    (void)vertical_flip;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_camera_stop(bsp_camera_t *camera)
{
    ESP_RETURN_ON_FALSE(camera, ESP_ERR_INVALID_ARG, TAG, "camera handle is null");
    if (!camera->started) {
        return ESP_OK;
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_ERROR(bsp_camera_video_ioctl(camera->video_fd, VIDIOC_STREAMOFF, &type, "STREAMOFF"),
                        TAG, "esp-video stream off failed");
    camera->started = false;
    ESP_LOGI(TAG, "Camera stream stopped");
    return ESP_OK;
}

esp_err_t bsp_camera_get_frame(bsp_camera_t *camera, bsp_camera_frame_t *frame)
{
    ESP_RETURN_ON_FALSE(camera && frame, ESP_ERR_INVALID_ARG, TAG, "invalid get frame argument");
    ESP_RETURN_ON_FALSE(camera->started, ESP_ERR_INVALID_STATE, TAG, "camera stream is not started");

    memset(&camera->dqbuf, 0, sizeof(camera->dqbuf));
    camera->dqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    camera->dqbuf.memory = V4L2_MEMORY_MMAP;
    ESP_RETURN_ON_ERROR(bsp_camera_video_ioctl(camera->video_fd, VIDIOC_DQBUF, &camera->dqbuf, "DQBUF"),
                        TAG, "esp-video dequeue buffer failed");
    ESP_RETURN_ON_FALSE(camera->dqbuf.index < BSP_CAMERA_BUFFER_COUNT &&
                        camera->video_buffer[camera->dqbuf.index],
                        ESP_FAIL, TAG, "esp-video invalid dequeued buffer index: %"PRIu32, camera->dqbuf.index);

    if (!camera->first_frame_logged) {
        ESP_LOGI(TAG, "Camera first frame: index=%"PRIu32", bytes=%"PRIu32", length=%"PRIu32,
                 camera->dqbuf.index, camera->dqbuf.bytesused, camera->dqbuf.length);
        camera->first_frame_logged = true;
    }

    frame->data = camera->video_buffer[camera->dqbuf.index];
    frame->size = camera->dqbuf.bytesused ? camera->dqbuf.bytesused : camera->frame_size;
    frame->index = camera->dqbuf.index;
    return ESP_OK;
}

esp_err_t bsp_camera_return_frame(bsp_camera_t *camera, const bsp_camera_frame_t *frame)
{
    ESP_RETURN_ON_FALSE(camera && frame, ESP_ERR_INVALID_ARG, TAG, "invalid return frame argument");
    ESP_RETURN_ON_FALSE(frame->index < BSP_CAMERA_BUFFER_COUNT, ESP_ERR_INVALID_ARG,
                        TAG, "invalid camera buffer index: %u", (unsigned int)frame->index);
    struct v4l2_buffer buf = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
        .index = frame->index,
    };
    return bsp_camera_video_ioctl(camera->video_fd, VIDIOC_QBUF, &buf, "QBUF");
}

esp_err_t bsp_camera_get_format(bsp_camera_t *camera, bsp_camera_format_t *out_format)
{
    ESP_RETURN_ON_FALSE(camera && out_format, ESP_ERR_INVALID_ARG, TAG, "invalid camera format argument");
    *out_format = camera->format;
    return ESP_OK;
}

esp_err_t bsp_camera_close(bsp_camera_t *camera)
{
    if (!camera) {
        return ESP_OK;
    }

    if (camera->started) {
        (void)bsp_camera_stop(camera);
    }

    for (int i = 0; i < BSP_CAMERA_BUFFER_COUNT; i++) {
        if (camera->video_buffer[i]) {
            (void)munmap(camera->video_buffer[i], camera->video_buffer_length[i]);
            camera->video_buffer[i] = NULL;
        }
    }
    if (camera->video_fd >= 0) {
        (void)close(camera->video_fd);
        camera->video_fd = -1;
    }
    if (camera->video_initialized) {
        (void)esp_video_deinit_with_flags(ESP_VIDEO_INIT_FLAGS_DVP);
        camera->video_initialized = false;
    }
    heap_caps_free(camera);
    return ESP_OK;
}

/* ==========================================================================
 * Display
 * ========================================================================== */

esp_err_t bsp_display_backlight_on(void)
{
    return bsp_config_output_gpio(BSP_LCD_BACKLIGHT, 1);
}

esp_err_t bsp_display_backlight_off(void)
{
    return bsp_config_output_gpio(BSP_LCD_BACKLIGHT, 0);
}

esp_err_t bsp_display_new(const bsp_display_config_t *config, esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(ret_panel, ESP_ERR_INVALID_ARG, TAG, "panel handle output is null");

    if (s_lcd_panel) {
        *ret_panel = s_lcd_panel;
        return ESP_OK;
    }

    bsp_display_config_t active_config = config ? *config : bsp_display_get_default_config();
    uint8_t num_fbs = esp_lv_adapter_get_required_frame_buffer_count(active_config.tear_avoid_mode,
                                                                     active_config.rotation);
    ESP_RETURN_ON_FALSE(num_fbs > 0 && num_fbs <= ESP_RGB_LCD_PANEL_MAX_FB_NUM,
                        ESP_ERR_INVALID_ARG, TAG, "invalid RGB frame buffer count: %u",
                        (unsigned int)num_fbs);

    ESP_LOGI(TAG, "Initializing RGB LCD: %dx%d RGB565, pclk=%d Hz, fb_count=%u",
             BSP_LCD_H_RES, BSP_LCD_V_RES, BSP_LCD_PIXEL_CLOCK_HZ, (unsigned int)num_fbs);

    ESP_RETURN_ON_ERROR(bsp_display_backlight_off(), TAG, "turn off backlight failed");

    esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = BSP_LCD_PIXEL_CLOCK_HZ,
            .h_res = BSP_LCD_H_RES,
            .v_res = BSP_LCD_V_RES,
            .hsync_pulse_width = BSP_LCD_HSYNC_PULSE_WIDTH,
            .hsync_back_porch = BSP_LCD_HSYNC_BACK_PORCH,
            .hsync_front_porch = BSP_LCD_HSYNC_FRONT_PORCH,
            .vsync_pulse_width = BSP_LCD_VSYNC_PULSE_WIDTH,
            .vsync_back_porch = BSP_LCD_VSYNC_BACK_PORCH,
            .vsync_front_porch = BSP_LCD_VSYNC_FRONT_PORCH,
            .flags = {
                .pclk_active_neg = true,
            },
        },
        .data_width = BSP_LCD_DATA_WIDTH,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = num_fbs,
        .dma_burst_size = 128,
        .hsync_gpio_num = BSP_LCD_HSYNC,
        .vsync_gpio_num = BSP_LCD_VSYNC,
        .de_gpio_num = BSP_LCD_DE,
        .pclk_gpio_num = BSP_LCD_PCLK,
        .disp_gpio_num = BSP_LCD_DISP_EN,
        .flags = {
            .fb_in_psram = true,
        },
    };
    bsp_display_fill_data_gpio_nums(panel_config.data_gpio_nums);

    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&panel_config, &panel), TAG, "create RGB panel failed");

    esp_err_t ret = esp_lcd_panel_reset(panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "reset RGB panel failed (%s)", esp_err_to_name(ret));
        esp_lcd_panel_del(panel);
        return ret;
    }

    ret = esp_lcd_panel_init(panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init RGB panel failed (%s)", esp_err_to_name(ret));
            esp_lcd_panel_del(panel);
        return ret;
    }

    ret = bsp_display_backlight_on();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "turn on backlight failed (%s)", esp_err_to_name(ret));
        esp_lcd_panel_del(panel);
        return ret;
    }

    s_lcd_panel = panel;
    s_lcd_config = active_config;
    s_lcd_config_valid = true;
    *ret_panel = panel;

    ESP_LOGI(TAG, "RGB LCD initialized");
    return ESP_OK;
}

lv_display_t *bsp_display_start(void)
{
    return bsp_display_start_with_config(NULL);
}

lv_display_t *bsp_display_start_with_config(const bsp_display_config_t *config)
{
    if (s_lvgl_display) {
        return s_lvgl_display;
    }

    bsp_display_config_t active_config = config ? *config : bsp_display_get_default_config();

    esp_err_t ret = bsp_display_new(&active_config, &s_lcd_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "display panel init failed (%s)", esp_err_to_name(ret));
        return NULL;
    }
    if (s_lcd_config_valid) {
        active_config = s_lcd_config;
    }

    if (!esp_lv_adapter_is_initialized()) {
        esp_lv_adapter_config_t adapter_config = ESP_LV_ADAPTER_DEFAULT_CONFIG();
        if (active_config.task_stack_size > 0) {
            adapter_config.task_stack_size = active_config.task_stack_size;
        }
        ret = esp_lv_adapter_init(&adapter_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "LVGL adapter init failed (%s)", esp_err_to_name(ret));
            return NULL;
        }
    }

    esp_lv_adapter_display_config_t display_config = ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(
                s_lcd_panel, NULL, BSP_LCD_H_RES, BSP_LCD_V_RES, active_config.rotation);
    display_config.tear_avoid_mode = active_config.tear_avoid_mode;
    if (active_config.buffer_height > 0) {
        display_config.profile.buffer_height = active_config.buffer_height;
    }
    display_config.profile.enable_ppa_accel = active_config.enable_ppa_accel;

    s_lvgl_display = esp_lv_adapter_register_display(&display_config);
    if (!s_lvgl_display) {
        ESP_LOGE(TAG, "register RGB display to LVGL adapter failed");
        return NULL;
    }

    if (!active_config.enable_touch) {
        ESP_LOGW(TAG, "Touch init skipped by display configuration");
    } else {
        ret = bsp_touch_new(active_config.rotation, &s_touch);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "touch init failed (%s), continue without touch", esp_err_to_name(ret));
        } else {
            esp_lv_adapter_touch_config_t touch_config = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(s_lvgl_display, s_touch);
            s_touch_indev = esp_lv_adapter_register_touch(&touch_config);
            if (!s_touch_indev) {
                ESP_LOGW(TAG, "register touch to LVGL adapter failed");
            }
        }
    }

    ret = esp_lv_adapter_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LVGL adapter start failed (%s)", esp_err_to_name(ret));
        esp_lv_adapter_unregister_display(s_lvgl_display);
        s_lvgl_display = NULL;
        return NULL;
    }

    ESP_LOGI(TAG, "Display registered to esp_lvgl_adapter");
    return s_lvgl_display;
}

lv_display_t *bsp_display_get(void)
{
    return s_lvgl_display;
}

esp_lcd_panel_handle_t bsp_display_get_panel(void)
{
    return s_lcd_panel;
}

lv_indev_t *bsp_display_get_input_dev(void)
{
    return s_touch_indev;
}

bool bsp_display_lock(int32_t timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

void bsp_display_unlock(void)
{
    esp_lv_adapter_unlock();
}

/* ==========================================================================
 * SD Card
 * ========================================================================== */

static bsp_sdcard_config_t bsp_sdcard_get_default_config(void)
{
    const bsp_sdcard_config_t config = BSP_SDCARD_DEFAULT_CONFIG();
    return config;
}

static esp_err_t bsp_sdcard_set_power(bool on)
{
    if (BSP_SD_CTRL == GPIO_NUM_NC) {
        return ESP_OK;
    }

    int level = on ? BSP_SD_CTRL_ACTIVE_LEVEL : BSP_SD_CTRL_INACTIVE_LEVEL;
    ESP_LOGI(TAG, "%s SD card power/control GPIO%d, level=%d",
             on ? "Enable" : "Disable", BSP_SD_CTRL, level);
    if (!s_sdcard_power_gpio_initialized) {
        ESP_RETURN_ON_ERROR(bsp_config_output_gpio(BSP_SD_CTRL, level),
                            TAG, "configure SD card power/control GPIO failed");
        s_sdcard_power_gpio_initialized = true;
        return ESP_OK;
    }

    return gpio_set_level(BSP_SD_CTRL, level);
}

esp_err_t bsp_sdcard_mount(const bsp_sdcard_config_t *config, sdmmc_card_t **out_card)
{
    ESP_RETURN_ON_FALSE(!s_sdcard, ESP_ERR_INVALID_STATE, TAG, "SD card already mounted");

    bsp_sdcard_config_t active_config = config ? *config : bsp_sdcard_get_default_config();
    if (!active_config.mount_point) {
        active_config.mount_point = BSP_SDCARD_MOUNT_POINT;
    }
    if (active_config.max_files == 0) {
        active_config.max_files = BSP_SDCARD_MAX_FILES;
    }
    if (active_config.allocation_unit_size == 0) {
        active_config.allocation_unit_size = BSP_SDCARD_ALLOC_UNIT_SIZE;
    }
    if (active_config.max_freq_khz == 0) {
        active_config.max_freq_khz = BSP_SDCARD_MAX_FREQ_KHZ;
    }

    /* NRL PATCH: power-cycle the card before every init attempt. A soft
     * reset (esptool RTS, AT+REBOOT, WDT) does not cut card VDD, and a card
     * left mid-transaction ignores the next init sequence (send_op_cond
     * timeout 0x107) until it really loses power. Cutting the GPIO39 gate
     * alone is not enough: the card keeps itself alive through the CMD/DAT
     * pull-ups (phantom power), so drive every bus line low for the off
     * window, then release them for the SDMMC host to reclaim. */
    {
        const gpio_num_t sd_bus_pins[] = {
            BSP_SD_CLK, BSP_SD_CMD, BSP_SD_D0, BSP_SD_D1, BSP_SD_D2, BSP_SD_D3,
        };
        for (size_t i = 0; i < sizeof(sd_bus_pins) / sizeof(sd_bus_pins[0]); i++) {
            gpio_reset_pin(sd_bus_pins[i]);
            gpio_set_direction(sd_bus_pins[i], GPIO_MODE_OUTPUT);
            gpio_set_level(sd_bus_pins[i], 0);
        }
        ESP_RETURN_ON_ERROR(bsp_sdcard_set_power(false), TAG, "SD card power off failed");
        vTaskDelay(pdMS_TO_TICKS(200));
        for (size_t i = 0; i < sizeof(sd_bus_pins) / sizeof(sd_bus_pins[0]); i++) {
            gpio_reset_pin(sd_bus_pins[i]);
        }
        ESP_RETURN_ON_ERROR(bsp_sdcard_set_power(true), TAG, "SD card power on failed");
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = active_config.max_freq_khz;
    host.flags &= ~(SDMMC_HOST_FLAG_1BIT | SDMMC_HOST_FLAG_4BIT | SDMMC_HOST_FLAG_8BIT);
    host.flags |= SDMMC_HOST_FLAG_4BIT;
    if ((active_config.max_freq_khz == SDMMC_FREQ_SDR50) || (active_config.max_freq_khz == SDMMC_FREQ_SDR104)) {
        host.flags &= ~SDMMC_HOST_FLAG_DDR;
    }
    host.unaligned_multi_block_rw_max_chunk_size = 8;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = BSP_SDCARD_BUS_WIDTH;
    slot_config.cd = SDMMC_SLOT_NO_CD;
    slot_config.wp = SDMMC_SLOT_NO_WP;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    if (active_config.max_freq_khz > SDMMC_FREQ_HIGHSPEED) {
        slot_config.flags |= SDMMC_SLOT_FLAG_UHS1;
    }
#ifdef CONFIG_SOC_SDMMC_USE_GPIO_MATRIX
    slot_config.clk = BSP_SD_CLK;
    slot_config.cmd = BSP_SD_CMD;
    slot_config.d0 = BSP_SD_D0;
    slot_config.d1 = BSP_SD_D1;
    slot_config.d2 = BSP_SD_D2;
    slot_config.d3 = BSP_SD_D3;
#endif

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = active_config.format_if_mount_failed,
        .max_files = active_config.max_files,
        .allocation_unit_size = active_config.allocation_unit_size,
    };

    ESP_LOGI(TAG, "Mount SD card: mount=%s, width=%d, freq=%d kHz, uhs1=%d, CLK=%d CMD=%d D0=%d D1=%d D2=%d D3=%d",
             active_config.mount_point,
             slot_config.width,
             host.max_freq_khz,
             !!(slot_config.flags & SDMMC_SLOT_FLAG_UHS1),
             BSP_SD_CLK,
             BSP_SD_CMD,
             BSP_SD_D0,
             BSP_SD_D1,
             BSP_SD_D2,
             BSP_SD_D3);

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(active_config.mount_point, &host, &slot_config, &mount_config, &s_sdcard);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card mount failed (%s)", esp_err_to_name(ret));
        return ret;
    }

    s_sdcard_mount_point = active_config.mount_point;
    if (out_card) {
        *out_card = s_sdcard;
    }
    sdmmc_card_print_info(stdout, s_sdcard);
    return ESP_OK;
}

esp_err_t bsp_sdcard_unmount(void)
{
    if (!s_sdcard) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_vfs_fat_sdcard_unmount(s_sdcard_mount_point, s_sdcard),
                        TAG, "SD card unmount failed");
    s_sdcard = NULL;
    s_sdcard_mount_point = BSP_SDCARD_MOUNT_POINT;
    return bsp_sdcard_set_power(false);
}

sdmmc_card_t *bsp_sdcard_get_card(void)
{
    return s_sdcard;
}

const char *bsp_sdcard_get_mount_point(void)
{
    return s_sdcard_mount_point;
}
