// Slippy-Map tile cache + loader, see map_tiles.h for the module contract.

#include "map_tiles.h"

#include "board_pins.h"

#if defined(NRL_HAS_DISPLAY) && NRL_HAS_DISPLAY && \
    (NRL_BOARD == NRL_BOARD_S31_KORVO || NRL_BOARD == NRL_BOARD_BI4UMD)

#include "../lib/nrl_psram.h"
#include "../lib/nrl_net_compat.h"
#include "../lib/nrl_version.h"
#include "../media/cover_decoder.h"
#include "storage_service.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_jpeg_enc.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// lvgl.h provides the LV_USE_LODEPNG guard. The vendored lodepng.h itself
// cannot be included from C++: it wraps everything -- including the
// overloaded C++ convenience wrappers -- in extern "C", which is rejected
// (conflicting declaration of C function / C++ templates with C linkage).
// Declare the one entry point we need instead. LVGL's fork returns an
// lv_draw_buf_t through this legacy pointer-shaped API; its pixel allocation
// must therefore be destroyed through lv_draw_buf_destroy().
#include <lvgl.h>
#include <src/draw/lv_draw_buf_private.h>

#if !defined(LV_USE_LODEPNG) || !LV_USE_LODEPNG
#error "map_tiles needs the vendored lodepng: set CONFIG_LV_USE_LODEPNG=y"
#endif

extern "C" unsigned lodepng_decode32(unsigned char **out, unsigned *w, unsigned *h,
                                     const unsigned char *in, size_t insize);

// LVGL's vendored lodepng normally allocates its inflate/conversion scratch
// buffers through lv_malloc()/malloc(). CMake disables those allocators for
// both map-capable boards, so keep that transient memory in PSRAM.
extern "C" void *lodepng_malloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

extern "C" void *lodepng_realloc(void *ptr, size_t new_size)
{
    return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

extern "C" void lodepng_free(void *ptr)
{
    heap_caps_free(ptr);
}

static const char *TAG = "MAP_TILES";

namespace {

constexpr uint8_t kMaxSlots = 16u;      // 16 x 128 KiB RGB565 in PSRAM
constexpr size_t kTilePx = MAP_TILES_TILE_PX;
constexpr size_t kTileBytes = kTilePx * kTilePx * 2u;
constexpr uint8_t kQueueDepth = 32u;
constexpr uint32_t kHttpTimeoutMs = 8000u;
constexpr uint32_t kHttpMinIntervalMs = 250u;
constexpr size_t kDownloadCap = 192u * 1024u; // OSM PNG tiles are typically 10-60 KB
constexpr size_t kJpegEncCap = 96u * 1024u;   // write-through encode buffer
constexpr uint32_t kFailRetryMs = 30000u;     // negative-cache window
constexpr uint32_t kOfflineRetryMs = 1000u;   // resume promptly after the route returns
constexpr size_t kTaskStackBytes = 8192u;

// Default raster source, {z}/{x}/{y} substituted. Compile-time constant for
// now; an NVS-backed override is planned (tile packing scripts must match).
constexpr char kTileUrlTemplate[] = "https://tile.openstreetmap.org/{z}/{x}/{y}.png";
constexpr char kTileUserAgent[] = NRL_FIRMWARE_NAME "/" NRL_FIRMWARE_VERSION
                                  " (+https://github.com/hicaoc/NRL-ESP32)";
// v2 deliberately ignores files written before policy-block responses were
// detected; OSM returns those error images with HTTP 200, so older firmware
// could have cached them as ordinary JPEG tiles.
constexpr char kTileCacheDir[] = "osm_tiles_v2";

struct TileKey {
    uint8_t z;
    int32_t x;
    int32_t y;
};

enum class SlotState : uint8_t { Empty, Loading, Ready, Failed };

struct TileSlot {
    TileKey key;
    SlotState state;
    uint32_t lru_tick;
    uint32_t fail_ms;
    uint32_t retry_ms;
};

TileSlot s_slots[kMaxSlots];
uint8_t *s_pixels = nullptr;   // s_slot_count * kTileBytes, PSRAM
uint8_t s_slot_count = 0u;
TileKey s_queue[kQueueDepth];
uint8_t s_queue_head = 0u;
uint8_t s_queue_count = 0u;
SemaphoreHandle_t s_lock = nullptr;
SemaphoreHandle_t s_queue_sem = nullptr;
volatile uint32_t s_revision = 0u;
uint32_t s_lru_counter = 0u;
uint32_t s_last_http_ms = 0u;

// Stack in PSRAM (CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY): the worker
// blocks on HTTP for seconds at a time, so it must not sit in internal SRAM.
NRL_PSRAM_BSS StackType_t s_task_stack[kTaskStackBytes / sizeof(StackType_t)];
StaticTask_t s_task_tcb;
bool s_task_started = false;

bool s_image_psram_handlers_installed = false;

void *imagePsramMalloc(size_t size, lv_color_format_t)
{
    // lv_draw_buf_create_ex will call the handler's existing align callback;
    // returning an already aligned block means it stays inside this allocation
    // without needing the default allocator's extra alignment padding.
    return heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, size,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void imagePsramFree(void *ptr)
{
    heap_caps_free(ptr);
}

bool installImagePsramHandlers()
{
    if (s_image_psram_handlers_installed) {
        return true;
    }
    lv_draw_buf_handlers_t *handlers = lv_draw_buf_get_image_handlers();
    if (handlers == nullptr) {
        ESP_LOGE(TAG, "LVGL image draw-buffer handlers unavailable");
        return false;
    }
    // Preserve LVGL's copy/alignment/cache/stride callbacks; replace only the
    // allocation pair used by lodepng's final lv_draw_buf_create_ex calls.
    handlers->buf_malloc_cb = imagePsramMalloc;
    handlers->buf_free_cb = imagePsramFree;
    s_image_psram_handlers_installed = true;
    ESP_LOGI(TAG, "LVGL image buffers forced to PSRAM");
    return true;
}

uint32_t millis()
{
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

bool sameKey(const TileKey &a, const TileKey &b)
{
    return a.z == b.z && a.x == b.x && a.y == b.y;
}

bool keyValid(const TileKey &key)
{
    if (key.z > 22u) {
        return false;
    }
    const int32_t limit = 1 << key.z;
    return key.x >= 0 && key.x < limit && key.y >= 0 && key.y < limit;
}

int findSlot(const TileKey &key)
{
    for (uint8_t i = 0u; i < s_slot_count; ++i) {
        if (s_slots[i].state != SlotState::Empty && sameKey(s_slots[i].key, key)) {
            return i;
        }
    }
    return -1;
}

void mapTilesTask(void *);

bool ensureInit()
{
    if (!installImagePsramHandlers()) {
        return false;
    }
    if (s_lock != nullptr) {
        return s_pixels != nullptr && s_task_started;
    }
    s_lock = xSemaphoreCreateMutex();
    s_queue_sem = xSemaphoreCreateCounting(kQueueDepth, 0);
    if (s_lock == nullptr || s_queue_sem == nullptr) {
        return false;
    }
    // Largest LRU that PSRAM grants: 16 -> 8 -> 4 slots.
    for (uint8_t count = kMaxSlots; count >= 4u; count >>= 1u) {
        // Both the JPEG decoder output path and the write-through encoder
        // require 16-byte-aligned pixel data. Keep the whole cache aligned so
        // every slot (kTileBytes is a multiple of 16) can be passed directly.
        s_pixels = static_cast<uint8_t *>(heap_caps_aligned_alloc(
            16u, static_cast<size_t>(count) * kTileBytes,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (s_pixels != nullptr) {
            s_slot_count = count;
            break;
        }
    }
    if (s_pixels == nullptr) {
        ESP_LOGE(TAG, "no PSRAM for tile cache");
        return false;
    }
    for (uint8_t i = 0u; i < kMaxSlots; ++i) {
        s_slots[i].state = SlotState::Empty;
        s_slots[i].key = {0xFFu, 0, 0};
    }
    s_task_started = xTaskCreateStaticPinnedToCore(
                         mapTilesTask, "map_tiles", kTaskStackBytes, nullptr, 3,
                         s_task_stack, &s_task_tcb, 0) != nullptr;
    if (!s_task_started) {
        ESP_LOGE(TAG, "worker task create failed");
    }
    ESP_LOGI(TAG, "tile cache ready: %u slots", static_cast<unsigned>(s_slot_count));
    return s_task_started;
}

uint8_t *slotPixels(uint8_t index)
{
    return s_pixels + static_cast<size_t>(index) * kTileBytes;
}

// ---- tile sources -----------------------------------------------------------

void tilePath(const TileKey &key, char *out, size_t out_size)
{
    const char *mnt = STORAGE_SdMountPoint();
    snprintf(out, out_size, "%s/%s/%u/%ld/%ld.jpg", mnt != nullptr ? mnt : "",
             kTileCacheDir, static_cast<unsigned>(key.z), static_cast<long>(key.x),
             static_cast<long>(key.y));
}

bool readSdTile(const TileKey &key, uint8_t *buf, size_t cap, size_t *out_size)
{
    if (!STORAGE_SdMounted()) {
        return false;
    }
    char path[128];
    tilePath(key, path, sizeof(path));
    FILE *file = fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }
    bool ok = false;
    if (fseek(file, 0, SEEK_END) == 0) {
        const long size = ftell(file);
        if (size > 0 && static_cast<size_t>(size) < cap && fseek(file, 0, SEEK_SET) == 0 &&
            fread(buf, 1, static_cast<size_t>(size), file) == static_cast<size_t>(size)) {
            *out_size = static_cast<size_t>(size);
            ok = true;
        }
    }
    fclose(file);
    return ok;
}

// Substitute {z}/{x}/{y} in the URL template.
void formatUrl(const TileKey &key, char *out, size_t out_size)
{
    size_t o = 0u;
    for (const char *p = kTileUrlTemplate; *p != '\0' && o + 1u < out_size;) {
        int value = -1;
        if (p[0] == '{' && p[2] == '}') {
            if (p[1] == 'z') { value = key.z; }
            else if (p[1] == 'x') { value = static_cast<int>(key.x); }
            else if (p[1] == 'y') { value = static_cast<int>(key.y); }
        }
        if (value >= 0) {
            const int n = snprintf(out + o, out_size - o, "%d", value);
            if (n < 0) {
                break;
            }
            o += static_cast<size_t>(n);
            p += 3;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
}

struct TileHttpContext {
    bool blocked;
    char reason[96];
};

esp_err_t tileHttpEvent(esp_http_client_event_t *event)
{
    auto *ctx = static_cast<TileHttpContext *>(event->user_data);
    if (ctx != nullptr && event->event_id == HTTP_EVENT_ON_HEADER &&
        event->header_key != nullptr && strcasecmp(event->header_key, "X-Blocked") == 0) {
        ctx->blocked = true;
        snprintf(ctx->reason, sizeof(ctx->reason), "%s",
                 event->header_value != nullptr ? event->header_value : "policy");
    }
    return ESP_OK;
}

bool httpDownload(const TileKey &key, uint8_t *buf, size_t cap, size_t *out_size)
{
    // Avoid repeatedly entering esp-tls while Wi-Fi/Ethernet has no usable IP.
    // A connection can still disappear after this check; that in-flight call
    // may fail once, but the following queued tiles will be deferred quietly.
    if (!nrlNetworkConnected()) {
        return false;
    }

    // The worker is serial already; add an explicit ceiling so a fresh view or
    // rapid pan cannot turn its queued requests into a burst.
    const uint32_t now = millis();
    if (s_last_http_ms != 0u) {
        const uint32_t elapsed = now - s_last_http_ms;
        if (elapsed < kHttpMinIntervalMs) {
            vTaskDelay(pdMS_TO_TICKS(kHttpMinIntervalMs - elapsed));
        }
    }
    s_last_http_ms = millis();

    char url[192];
    formatUrl(key, url, sizeof(url));
    TileHttpContext ctx = {};
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = static_cast<int>(kHttpTimeoutMs);
    cfg.buffer_size = 2048;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.event_handler = tileHttpEvent;
    cfg.user_data = &ctx;
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (http == nullptr) {
        return false;
    }
    // Stable, unique native-app identification with a public contact URL.
    (void)esp_http_client_set_header(http, "User-Agent", kTileUserAgent);
    (void)esp_http_client_set_header(http, "Accept", "image/png");
    bool ok = false;
    if (esp_http_client_open(http, 0) == ESP_OK) {
        (void)esp_http_client_fetch_headers(http);
        const int status = esp_http_client_get_status_code(http);
        // OSM intentionally serves policy error tiles as HTTP 200. Never
        // decode or persist one when the accompanying X-Blocked header exists.
        if (ctx.blocked) {
            ESP_LOGE(TAG, "tile request blocked by OSM policy: %s", ctx.reason);
        } else if (status == 200) {
            size_t got = 0u;
            while (got < cap) {
                const int n = esp_http_client_read(http, reinterpret_cast<char *>(buf) + got,
                                                   static_cast<int>(cap - got));
                if (n <= 0) {
                    break;
                }
                got += static_cast<size_t>(n);
            }
            // A body that exactly fills the buffer may be truncated; reject it.
            if (got > 0u && got < cap) {
                *out_size = got;
                ok = true;
            }
        } else {
            ESP_LOGW(TAG, "GET %s -> %d", url, status);
        }
        (void)esp_http_client_close(http);
    }
    esp_http_client_cleanup(http);
    return ok;
}

// ---- decoders ----------------------------------------------------------------

// Nearest-neighbor copy of an RGB565 bitmap into the 256x256 tile buffer
// (well-formed tiles are already 256x256, so this is a straight copy then).
void blitRgb565(const uint8_t *src, uint16_t w, uint16_t h, uint8_t *dst)
{
    const uint16_t *s = reinterpret_cast<const uint16_t *>(src);
    uint16_t *d = reinterpret_cast<uint16_t *>(dst);
    for (size_t y = 0u; y < kTilePx; ++y) {
        const uint32_t sy = static_cast<uint32_t>(y) * h / kTilePx;
        for (size_t x = 0u; x < kTilePx; ++x) {
            const uint32_t sx = static_cast<uint32_t>(x) * w / kTilePx;
            d[y * kTilePx + x] = s[sy * w + sx];
        }
    }
}

bool decodeJpegToTile(const uint8_t *data, size_t size, uint8_t *out)
{
    CoverBitmap bmp = {};
    if (!COVER_DecodeJpeg(data, size, static_cast<uint16_t>(kTilePx), &bmp) ||
        bmp.rgb565 == nullptr ||
        bmp.width == 0u || bmp.height == 0u) {
        COVER_Free(&bmp);
        return false;
    }
    blitRgb565(bmp.rgb565, bmp.width, bmp.height, out);
    COVER_Free(&bmp);
    return true;
}

bool decodePngToTile(const uint8_t *data, size_t size, uint8_t *out)
{
    // LVGL's vendored lodepng does not return a bare RGBA allocation. It
    // returns an lv_draw_buf_t whose small descriptor may live in internal
    // RAM, while draw_buf->data owns the large pixel allocation in PSRAM.
    lv_draw_buf_t *draw_buf = nullptr;
    unsigned w = 0u;
    unsigned h = 0u;
    const unsigned err = lodepng_decode32(
        reinterpret_cast<unsigned char **>(&draw_buf), &w, &h, data, size);
    if (err != 0u || draw_buf == nullptr || draw_buf->data == nullptr ||
        w == 0u || h == 0u) {
        ESP_LOGW(TAG, "PNG decode failed (%u)", err);
        if (draw_buf != nullptr) {
            lv_draw_buf_destroy(draw_buf);
        }
        return false;
    }

    const size_t row_bytes = static_cast<size_t>(w) * 4u;
    const size_t pixel_bytes = static_cast<size_t>(draw_buf->header.stride) * h;
    const bool layout_ok = draw_buf->header.w == w && draw_buf->header.h == h &&
                           draw_buf->header.stride >= row_bytes &&
                           pixel_bytes <= draw_buf->data_size;
    // The handler installed by ensureInit() makes PSRAM an invariant on both
    // map-capable boards. Check both ends to catch allocator/config errors.
    const bool storage_ok = pixel_bytes != 0u &&
                            esp_ptr_external_ram(draw_buf->data) &&
                            esp_ptr_external_ram(draw_buf->data + pixel_bytes - 1u);
    if (!layout_ok || !storage_ok) {
        ESP_LOGE(TAG, "PNG buffer invalid: data=%p %ux%u stride=%u size=%u storage=%d",
                 draw_buf->data, w, h,
                 static_cast<unsigned>(draw_buf->header.stride),
                 static_cast<unsigned>(draw_buf->data_size),
                 storage_ok ? 1 : 0);
        lv_draw_buf_destroy(draw_buf);
        return false;
    }

    uint16_t *d = reinterpret_cast<uint16_t *>(out);
    for (size_t y = 0u; y < kTilePx; ++y) {
        const uint32_t sy = static_cast<uint32_t>(y) * h / kTilePx;
        const uint8_t *row = draw_buf->data +
                             static_cast<size_t>(sy) * draw_buf->header.stride;
        for (size_t x = 0u; x < kTilePx; ++x) {
            const uint32_t sx = static_cast<uint32_t>(x) * w / kTilePx;
            const uint8_t *p = row + static_cast<size_t>(sx) * 4u;
            d[y * kTilePx + x] = static_cast<uint16_t>(
                ((p[0] & 0xF8u) << 8u) | ((p[1] & 0xFCu) << 3u) | (p[2] >> 3u));
        }
    }
    lv_draw_buf_destroy(draw_buf);
    return true;
}

// Re-encode a decoded tile as JPEG and store it on the TF card so the next
// boot (or an offline session) reads it from there. Best-effort.
void writeThroughSd(const TileKey &key, const uint8_t *rgb565)
{
    if (!STORAGE_SdMounted()) {
        return;
    }
    jpeg_enc_config_t enc_cfg = DEFAULT_JPEG_ENC_CONFIG();
    enc_cfg.width = static_cast<int>(kTilePx);
    enc_cfg.height = static_cast<int>(kTilePx);
    enc_cfg.src_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    enc_cfg.subsampling = JPEG_SUBSAMPLE_420;
    enc_cfg.quality = 80u;
    jpeg_enc_handle_t enc = nullptr;
    if (jpeg_enc_open(&enc_cfg, &enc) != JPEG_ERR_OK || enc == nullptr) {
        return;
    }
    uint8_t *jpeg = static_cast<uint8_t *>(
        heap_caps_malloc(kJpegEncCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    int jpeg_size = 0;
    bool ok = jpeg != nullptr &&
              jpeg_enc_process(enc, rgb565, static_cast<int>(kTileBytes), jpeg,
                               static_cast<int>(kJpegEncCap), &jpeg_size) == JPEG_ERR_OK &&
              jpeg_size > 0;
    (void)jpeg_enc_close(enc);
    if (!ok) {
        if (jpeg != nullptr) {
            heap_caps_free(jpeg);
        }
        return;
    }

    const char *mnt = STORAGE_SdMountPoint();
    char dir[96];
    snprintf(dir, sizeof(dir), "%s/%s", mnt, kTileCacheDir);
    (void)mkdir(dir, 0775);
    snprintf(dir, sizeof(dir), "%s/%s/%u", mnt, kTileCacheDir,
             static_cast<unsigned>(key.z));
    (void)mkdir(dir, 0775);
    snprintf(dir, sizeof(dir), "%s/%s/%u/%ld", mnt, kTileCacheDir,
             static_cast<unsigned>(key.z), static_cast<long>(key.x));
    (void)mkdir(dir, 0775);

    char path[128];
    tilePath(key, path, sizeof(path));
    FILE *file = fopen(path, "wb");
    if (file != nullptr) {
        ok = fwrite(jpeg, 1, static_cast<size_t>(jpeg_size), file) ==
             static_cast<size_t>(jpeg_size);
        fclose(file);
        if (ok) {
            ESP_LOGI(TAG, "cached %s", path);
        }
    }
    heap_caps_free(jpeg);
}

// TF card (JPEG) first, HTTP(S) download (PNG) otherwise.
enum class TileLoadResult : uint8_t { Ready, Failed, Offline };

TileLoadResult loadTile(const TileKey &key, uint8_t *out)
{
    // COVER_DecodeJpeg can borrow an already aligned input buffer. Allocating
    // the tile payload aligned in PSRAM avoids a second full-size copy into
    // scarce internal RAM before JPEG decoding.
    uint8_t *buf = static_cast<uint8_t *>(heap_caps_aligned_alloc(
        16u, kDownloadCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buf == nullptr) {
        return TileLoadResult::Failed;
    }
    size_t size = 0u;
    bool ok = false;
    if (readSdTile(key, buf, kDownloadCap, &size)) {
        ok = decodeJpegToTile(buf, size, out);
    }
    if (ok) {
        heap_caps_free(buf);
        return TileLoadResult::Ready;
    }
    if (!nrlNetworkConnected()) {
        heap_caps_free(buf);
        return TileLoadResult::Offline;
    }
    if (!ok && httpDownload(key, buf, kDownloadCap, &size)) {
        // The URL template may point at a JPEG source; sniff the magic bytes.
        if (size >= 2u && buf[0] == 0xFFu && buf[1] == 0xD8u) {
            ok = decodeJpegToTile(buf, size, out);
        } else {
            ok = decodePngToTile(buf, size, out);
        }
        if (ok) {
            writeThroughSd(key, out);
        }
    }
    heap_caps_free(buf);
    if (ok) {
        return TileLoadResult::Ready;
    }
    return nrlNetworkConnected() ? TileLoadResult::Failed : TileLoadResult::Offline;
}

// ---- worker -------------------------------------------------------------------

// Claim a cache slot for `key`: reuse its own (stale-failed) entry, an empty
// slot, or evict the least-recently-used one. Caller holds s_lock.
int claimSlot(const TileKey &key)
{
    const int own = findSlot(key);
    if (own >= 0) {
        return own;
    }
    int victim = -1;
    uint32_t oldest = UINT32_MAX;
    for (uint8_t i = 0u; i < s_slot_count; ++i) {
        if (s_slots[i].state == SlotState::Empty) {
            return i;
        }
        if (s_slots[i].state != SlotState::Loading && s_slots[i].lru_tick < oldest) {
            oldest = s_slots[i].lru_tick;
            victim = i;
        }
    }
    return victim;
}

void mapTilesTask(void *)
{
    for (;;) {
        xSemaphoreTake(s_queue_sem, portMAX_DELAY);

        TileKey key = {};
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_queue_count == 0u) {
            xSemaphoreGive(s_lock);
            continue;
        }
        key = s_queue[s_queue_head];
        s_queue_head = static_cast<uint8_t>((s_queue_head + 1u) % kQueueDepth);
        --s_queue_count;
        const int existing = findSlot(key);
        if (existing >= 0 && s_slots[existing].state == SlotState::Ready) {
            xSemaphoreGive(s_lock);
            continue; // arrived while queued
        }
        const int slot = claimSlot(key);
        if (slot < 0) {
            xSemaphoreGive(s_lock);
            continue; // every slot is mid-load; the UI re-requests on repaint
        }
        s_slots[slot].key = key;
        s_slots[slot].state = SlotState::Loading;
        uint8_t *pixels = slotPixels(static_cast<uint8_t>(slot));
        xSemaphoreGive(s_lock);

        const TileLoadResult result = loadTile(key, pixels);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (result == TileLoadResult::Ready) {
            s_slots[slot].state = SlotState::Ready;
            s_slots[slot].lru_tick = ++s_lru_counter;
            // Plain assignment: ++/-- on a volatile object is deprecated in
            // C++20 and this build treats warnings as errors.
            s_revision = s_revision + 1u;
        } else {
            s_slots[slot].state = SlotState::Failed;
            s_slots[slot].fail_ms = millis();
            s_slots[slot].retry_ms = result == TileLoadResult::Offline
                                         ? kOfflineRetryMs
                                         : kFailRetryMs;
        }
        xSemaphoreGive(s_lock);
    }
}

} // namespace

void MAP_TILES_LonLatToPixel(double lon, double lat, uint8_t zoom, double *px, double *py)
{
    const double world = ldexp(static_cast<double>(kTilePx), zoom);
    if (px != nullptr) {
        *px = (lon + 180.0) / 360.0 * world;
    }
    if (py != nullptr) {
        const double rad = lat * M_PI / 180.0;
        *py = (1.0 - log(tan(rad) + 1.0 / cos(rad)) / M_PI) / 2.0 * world;
    }
}

void MAP_TILES_PixelToLonLat(double px, double py, uint8_t zoom, double *lon, double *lat)
{
    const double world = ldexp(static_cast<double>(kTilePx), zoom);
    if (lon != nullptr) {
        *lon = px / world * 360.0 - 180.0;
    }
    if (lat != nullptr) {
        const double a = M_PI * (1.0 - 2.0 * py / world);
        *lat = 180.0 / M_PI * atan(sinh(a));
    }
}

void MAP_TILES_Request(uint8_t z, int32_t x, int32_t y)
{
    const TileKey key = {z, x, y};
    if (!keyValid(key) || !ensureInit()) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int slot = findSlot(key);
    if (slot >= 0) {
        const SlotState state = s_slots[slot].state;
        const bool cool_down = state == SlotState::Failed &&
                               (millis() - s_slots[slot].fail_ms) < s_slots[slot].retry_ms;
        if (state == SlotState::Ready || state == SlotState::Loading || cool_down) {
            xSemaphoreGive(s_lock);
            return;
        }
    }
    for (uint8_t i = 0u; i < s_queue_count; ++i) {
        if (sameKey(s_queue[(s_queue_head + i) % kQueueDepth], key)) {
            xSemaphoreGive(s_lock);
            return;
        }
    }
    if (s_queue_count < kQueueDepth) {
        const uint8_t tail = static_cast<uint8_t>((s_queue_head + s_queue_count) % kQueueDepth);
        s_queue[tail] = key;
        ++s_queue_count;
        xSemaphoreGive(s_queue_sem);
    }
    xSemaphoreGive(s_lock);
}

const uint8_t *MAP_TILES_Get(uint8_t z, int32_t x, int32_t y)
{
    const TileKey key = {z, x, y};
    if (!keyValid(key) || !ensureInit()) {
        return nullptr;
    }
    const uint8_t *pixels = nullptr;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int slot = findSlot(key);
    if (slot >= 0 && s_slots[slot].state == SlotState::Ready) {
        s_slots[slot].lru_tick = ++s_lru_counter;
        pixels = slotPixels(static_cast<uint8_t>(slot));
    }
    xSemaphoreGive(s_lock);
    if (pixels == nullptr) {
        MAP_TILES_Request(z, x, y);
    }
    return pixels;
}

uint32_t MAP_TILES_Revision(void)
{
    return s_revision;
}

#endif // NRL_HAS_DISPLAY && (NRL_BOARD == NRL_BOARD_S31_KORVO || NRL_BOARD == NRL_BOARD_BI4UMD)
