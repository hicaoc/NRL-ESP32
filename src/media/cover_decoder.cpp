#include "media/cover_decoder.h"

#include <esp_jpeg_common.h>
#include <esp_jpeg_dec.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_memory_utils.h>

#include <string.h>

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

extern "C" void COVER_Free(CoverBitmap *bitmap)
{
    if (bitmap == nullptr) {
        return;
    }
    if (bitmap->rgb565 != nullptr) {
        if (bitmap->rgb565_uses_jpeg_allocator) {
            jpeg_free_align(bitmap->rgb565);
        } else {
            heap_caps_free(bitmap->rgb565);
        }
    }
    memset(bitmap, 0, sizeof(*bitmap));
}
