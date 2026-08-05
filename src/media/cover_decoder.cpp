#include "media/cover_decoder.h"

#include <esp_jpeg_common.h>
#include <esp_jpeg_dec.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <soc/soc_caps.h>

#include <string.h>

#if SOC_JPEG_CODEC_SUPPORTED
// Hardware JPEG codec peripheral (ESP32-S31). Full-resolution decode in
// microseconds instead of the ~1 s software cost that starved cores under
// camera load; downscaling happens at display time instead.
#include <driver/jpeg_decode.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

static const char *TAG = "COVER";

namespace {

// Round down to the decoder's multiple-of-8 requirement, staying >= 8.
static uint16_t align8(const uint32_t value)
{
    const uint32_t aligned = value & ~7u;
    return static_cast<uint16_t>((aligned < 8u) ? 8u : aligned);
}

} // namespace

extern "C" bool COVER_DecodeJpeg(const uint8_t *jpeg, const size_t jpeg_size,
                                 const uint16_t max_dim, CoverBitmap *out)
{
    if (out == nullptr) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (jpeg == nullptr || jpeg_size == 0u || max_dim < 8u) {
        return false;
    }

    // The decoder requires a 16-byte-aligned input buffer, but it does not
    // require internal RAM. Borrow aligned callers' buffers directly. For
    // legacy/misaligned callers, make the compatibility copy in PSRAM first
    // so a cover or map tile cannot consume the small internal-RAM heap.
    bool input_borrowed = (reinterpret_cast<uintptr_t>(jpeg) & 0x0Fu) == 0u;
#if CONFIG_SPIRAM
    // On PSRAM-equipped targets, alignment alone is not sufficient: borrowing
    // a large internal buffer would defeat the decoder's external-memory rule.
    input_borrowed = input_borrowed &&
                     esp_ptr_external_ram(jpeg) &&
                     esp_ptr_external_ram(jpeg + jpeg_size - 1u);
#endif
    bool input_uses_jpeg_allocator = false;
    uint8_t *inbuf = input_borrowed ? const_cast<uint8_t *>(jpeg) : nullptr;
    if (inbuf == nullptr) {
#if CONFIG_SPIRAM
        inbuf = static_cast<uint8_t *>(heap_caps_aligned_alloc(
            16u, jpeg_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
        // Preserve operation on targets that genuinely have no PSRAM.
        inbuf = static_cast<uint8_t *>(jpeg_calloc_align(jpeg_size, 16));
        input_uses_jpeg_allocator = inbuf != nullptr;
#endif
        if (inbuf == nullptr) {
            return false;
        }
        memcpy(inbuf, jpeg, jpeg_size);
    }

    bool ok = false;
    jpeg_dec_handle_t dec = nullptr;
    uint8_t *outbuf = nullptr;
    bool outbuf_uses_jpeg_allocator = false;

    do {
        // First pass: parse the header only, to compute the scale target.
        jpeg_dec_config_t probe_cfg = DEFAULT_JPEG_DEC_CONFIG();
        probe_cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
        if (jpeg_dec_open(&probe_cfg, &dec) != JPEG_ERR_OK) {
            break;
        }
        jpeg_dec_io_t io = {};
        io.inbuf = inbuf;
        io.inbuf_len = static_cast<int>(jpeg_size);
        jpeg_dec_header_info_t header = {};
        if (jpeg_dec_parse_header(dec, &io, &header) != JPEG_ERR_OK ||
            header.width == 0u || header.height == 0u) {
            ESP_LOGW(TAG, "JPEG header parse failed");
            break;
        }
        jpeg_dec_close(dec);
        dec = nullptr;

        // Fit within max_dim, keeping aspect; the decoder's maximum
        // downscale is 1/8, which any realistic cover satisfies.
        jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
        cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
        uint16_t out_w = header.width;
        uint16_t out_h = header.height;
        if (header.width > max_dim || header.height > max_dim) {
            const uint32_t larger = (header.width > header.height) ? header.width : header.height;
            out_w = align8((static_cast<uint32_t>(header.width) * max_dim) / larger);
            out_h = align8((static_cast<uint32_t>(header.height) * max_dim) / larger);
            const uint32_t min_w = (header.width + 7u) / 8u;   // 1/8 downscale floor
            const uint32_t min_h = (header.height + 7u) / 8u;
            if (out_w < min_w) { out_w = align8(min_w + 7u); }
            if (out_h < min_h) { out_h = align8(min_h + 7u); }
            cfg.scale.width = out_w;
            cfg.scale.height = out_h;
        }

        if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
            break;
        }
        memset(&io, 0, sizeof(io));
        io.inbuf = inbuf;
        io.inbuf_len = static_cast<int>(jpeg_size);
        if (jpeg_dec_parse_header(dec, &io, &header) != JPEG_ERR_OK) {
            break;
        }
        int outbuf_len = 0;
        if (jpeg_dec_get_outbuf_len(dec, &outbuf_len) != JPEG_ERR_OK || outbuf_len <= 0) {
            break;
        }
#if CONFIG_SPIRAM
        outbuf = static_cast<uint8_t *>(heap_caps_aligned_alloc(
            16u, static_cast<size_t>(outbuf_len), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
        outbuf = static_cast<uint8_t *>(jpeg_calloc_align(static_cast<size_t>(outbuf_len), 16));
        outbuf_uses_jpeg_allocator = outbuf != nullptr;
#endif
        if (outbuf == nullptr) {
            ESP_LOGW(TAG, "cover outbuf %d alloc failed", outbuf_len);
            break;
        }
        io.outbuf = outbuf;

        int process_count = 1;
        (void)jpeg_dec_get_process_count(dec, &process_count);
        bool decode_ok = true;
        for (int i = 0; i < process_count; ++i) {
            if (jpeg_dec_process(dec, &io) != JPEG_ERR_OK) {
                decode_ok = false;
                break;
            }
        }
        if (!decode_ok) {
            ESP_LOGW(TAG, "JPEG decode failed");
            break;
        }

        out->width = out_w;
        out->height = out_h;
        out->stride = static_cast<uint16_t>(out_w * 2u);
        out->rgb565 = outbuf;
        out->bytes = static_cast<size_t>(outbuf_len);
        out->rgb565_uses_jpeg_allocator = outbuf_uses_jpeg_allocator;
        outbuf = nullptr; // ownership moved to caller
        ok = true;
        ESP_LOGI(TAG, "cover decoded %ux%u (%u bytes)",
                 static_cast<unsigned>(out->width),
                 static_cast<unsigned>(out->height),
                 static_cast<unsigned>(out->bytes));
    } while (false);

    if (dec != nullptr) {
        jpeg_dec_close(dec);
    }
    if (outbuf != nullptr) {
        if (outbuf_uses_jpeg_allocator) {
            jpeg_free_align(outbuf);
        } else {
            heap_caps_free(outbuf);
        }
    }
    if (!input_borrowed) {
        if (input_uses_jpeg_allocator) {
            jpeg_free_align(inbuf);
        } else {
            heap_caps_free(inbuf);
        }
    }
    return ok;
}

extern "C" bool COVER_FitRgb565(const uint8_t *src, const uint16_t src_w, const uint16_t src_h,
                                const uint32_t src_stride, const uint16_t fit_w, const uint16_t fit_h,
                                CoverBitmap *out)
{
    if (out != nullptr) {
        memset(out, 0, sizeof(*out));
    }
    if (src == nullptr || out == nullptr || src_w == 0u || src_h == 0u ||
        fit_w < 8u || fit_h < 8u) {
        return false;
    }
    uint32_t out_w = src_w;
    uint32_t out_h = src_h;
    if (src_w > fit_w || src_h > fit_h) {
        // Scale by the tighter axis so the result stays inside the box.
        if (static_cast<uint32_t>(fit_w) * src_h < static_cast<uint32_t>(fit_h) * src_w) {
            out_w = fit_w;
            out_h = (static_cast<uint32_t>(src_h) * fit_w) / src_w;
        } else {
            out_h = fit_h;
            out_w = (static_cast<uint32_t>(src_w) * fit_h) / src_h;
        }
        if (out_w == 0u) out_w = 1u;
        if (out_h == 0u) out_h = 1u;
    }
    const size_t bytes = static_cast<size_t>(out_w) * out_h * 2u;
    uint8_t *buf = static_cast<uint8_t *>(
        heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buf == nullptr) {
        return false;
    }
    const uint32_t stride = (src_stride != 0u) ? src_stride
                                               : static_cast<uint32_t>(src_w) * 2u;
    for (uint32_t y = 0; y < out_h; ++y) {
        const uint32_t sy = (y * src_h) / out_h;
        const uint8_t *srow = src + sy * stride;
        uint8_t *drow = buf + y * out_w * 2u;
        for (uint32_t x = 0; x < out_w; ++x) {
            const uint32_t sx = (x * src_w) / out_w;
            drow[x * 2u] = srow[sx * 2u];
            drow[x * 2u + 1u] = srow[sx * 2u + 1u];
        }
    }
    out->width = static_cast<uint16_t>(out_w);
    out->height = static_cast<uint16_t>(out_h);
    out->stride = static_cast<uint16_t>(out_w * 2u);
    out->rgb565 = buf;
    out->bytes = bytes;
    return true;
}

extern "C" bool COVER_ScaleRgb565(const uint8_t *src, const uint16_t src_w, const uint16_t src_h,
                                  const uint32_t src_stride, const uint16_t max_dim, CoverBitmap *out)
{
    return COVER_FitRgb565(src, src_w, src_h, src_stride, max_dim, max_dim, out);
}

extern "C" void COVER_Free(CoverBitmap *bitmap)
{
    if (bitmap == nullptr) {
        return;
    }
    if (bitmap->rgb565 != nullptr && !bitmap->rgb565_borrowed) {
        if (bitmap->rgb565_uses_jpeg_allocator) {
            jpeg_free_align(bitmap->rgb565);
        } else {
            heap_caps_free(bitmap->rgb565);
        }
    }
    memset(bitmap, 0, sizeof(*bitmap));
}

// ---- Hardware JPEG decode (ESP32-S31 JPEG codec peripheral) -----------------

#if SOC_JPEG_CODEC_SUPPORTED

namespace {

// One engine for the whole firmware: the peripheral is a single unit and the
// decode task is the only user. Created lazily on first call.
jpeg_decoder_handle_t s_hw_dec = nullptr;
SemaphoreHandle_t s_hw_lock = nullptr;

// Persistent grow-only output buffer: ONE ~1.8 MB PSRAM block for the whole
// firmware instead of a fresh 720p allocation per frame. Per-frame alloc/free
// churn of a block this size shattered the PSRAM heap until the 1.8 MB
// request started failing ("no mem for 1843200 bytes decode buffer"), which
// pushed video frames down the seconds-long software path and starved IDLE1
// into the task watchdog. Bitmaps borrowing this buffer set rgb565_borrowed.
uint8_t *s_hw_buf = nullptr;
size_t s_hw_buf_size = 0;

bool hwEnsureDecoder()
{
    if (s_hw_dec != nullptr) {
        return true;
    }
    if (s_hw_lock == nullptr) {
        s_hw_lock = xSemaphoreCreateMutex();
        if (s_hw_lock == nullptr) {
            return false;
        }
    }
    jpeg_decode_engine_cfg_t eng = {};
    eng.timeout_ms = 500; // a 720p frame decodes in well under this
    return jpeg_new_decoder_engine(&eng, &s_hw_dec) == ESP_OK;
}

} // namespace

extern "C" bool COVER_DecodeJpegHw(const uint8_t *jpeg, const size_t jpeg_size, CoverBitmap *out)
{
    if (out != nullptr) {
        memset(out, 0, sizeof(*out));
    }
    if (out == nullptr || jpeg == nullptr || jpeg_size == 0u) {
        return false;
    }
    if (!hwEnsureDecoder()) {
        return false;
    }
    xSemaphoreTake(s_hw_lock, portMAX_DELAY);
    bool ok = false;
    uint8_t *outbuf = nullptr;
    do {
        jpeg_decode_picture_info_t info = {};
        if (jpeg_decoder_get_info(jpeg, static_cast<uint32_t>(jpeg_size), &info) != ESP_OK ||
            info.width == 0u || info.height == 0u ||
            info.width > 4096u || info.height > 4096u) {
            ESP_LOGW(TAG, "hw jpeg header parse failed");
            break;
        }
        // YUV420/422 output is padded to the hardware's 16-pixel block layout;
        // LVGL gets the padded pitch through the bitmap stride.
        const uint32_t w16 = (info.width + 15u) & ~15u;
        const uint32_t h16 = (info.height + 15u) & ~15u;
        const size_t need = static_cast<size_t>(w16) * h16 * 2u;
        if (need > s_hw_buf_size) {
            size_t alloc_size = 0;
            jpeg_decode_memory_alloc_cfg_t mem_cfg = {};
            mem_cfg.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
            uint8_t *larger = static_cast<uint8_t *>(
                jpeg_alloc_decoder_mem(need, &mem_cfg, &alloc_size));
            if (larger == nullptr || alloc_size < need) {
                heap_caps_free(larger); // no-op on nullptr
                ESP_LOGW(TAG, "hw jpeg outbuf %u alloc failed", static_cast<unsigned>(need));
                break;
            }
            heap_caps_free(s_hw_buf);
            s_hw_buf = larger;
            s_hw_buf_size = alloc_size;
        }
        outbuf = s_hw_buf;
        const size_t alloc_size = s_hw_buf_size;
        jpeg_decode_cfg_t cfg = {};
        cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
        // BGR element order emits little-endian RGB565 bytes, matching the
        // software path (RGB565_LE) and LVGL's LV_COLOR_FORMAT_RGB565 layout.
        cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
        cfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
        uint32_t out_size = 0;
        if (jpeg_decoder_process(s_hw_dec, &cfg, jpeg, static_cast<uint32_t>(jpeg_size),
                                 outbuf, static_cast<uint32_t>(alloc_size), &out_size) != ESP_OK) {
            ESP_LOGW(TAG, "hw jpeg decode failed");
            break;
        }
        out->width = static_cast<uint16_t>(info.width);
        out->height = static_cast<uint16_t>(info.height);
        out->stride = static_cast<uint16_t>(w16 * 2u);
        out->rgb565 = outbuf;
        out->bytes = (out_size > 0u) ? out_size : need;
        // Borrowed module-owned persistent buffer: the caller must copy or
        // scale the pixels out before the next COVER_DecodeJpegHw call.
        out->rgb565_uses_jpeg_allocator = false;
        out->rgb565_borrowed = true;
        outbuf = nullptr; // persistent buffer, not caller-owned
        ok = true;
        ESP_LOGI(TAG, "hw jpeg decoded %ux%u (stride %u)",
                 static_cast<unsigned>(info.width), static_cast<unsigned>(info.height),
                 static_cast<unsigned>(w16 * 2u));
    } while (false);
    // A failed decode keeps the persistent buffer for the next attempt.
    xSemaphoreGive(s_hw_lock);
    return ok;
}

extern "C" void COVER_HwDecoderRelease(void)
{
    heap_caps_free(s_hw_buf);
    s_hw_buf = nullptr;
    s_hw_buf_size = 0;
}

#else // !SOC_JPEG_CODEC_SUPPORTED

extern "C" bool COVER_DecodeJpegHw(const uint8_t *, size_t, CoverBitmap *out)
{
    if (out != nullptr) {
        memset(out, 0, sizeof(*out));
    }
    return false;
}

extern "C" void COVER_HwDecoderRelease(void) {}

#endif // SOC_JPEG_CODEC_SUPPORTED
