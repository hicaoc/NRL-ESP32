// LVGL core allocator routed to PSRAM.
//
// LVGL is configured with LV_USE_CUSTOM_MALLOC on the display boards, which
// makes these *_core entry points the project's responsibility. The stock
// CLIB choice routes every lv_malloc() through malloc(); with
// CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=128 every allocation of <= 128 bytes
// (labels, style props, list nodes -- thousands of them on the 800x480
// browser) is forced into internal RAM, where the survivors permanently
// fragment the heap ("free=1359 largest=12" after a browsing session) until
// even the SDMMC DMA buffer fails to allocate. LVGL objects are only ever
// touched by the LVGL render task, so PSRAM-first with an internal fallback
// costs nothing measurable and keeps the internal heap for DMA/lwIP/WiFi.
#include <esp_heap_caps.h>

#if defined(CONFIG_LV_USE_CUSTOM_MALLOC)

#include <stddef.h>

#include "lvgl.h"

namespace {

void *alloc_psram_first(const size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == nullptr) {
        p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return p;
}

} // namespace

extern "C" void lv_mem_init(void)
{
    // Nothing to initialize: the ESP heap is always ready.
}

extern "C" void lv_mem_deinit(void)
{
}

extern "C" void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    (void)pool;
}

extern "C" void *lv_malloc_core(size_t size)
{
    return alloc_psram_first(size);
}

extern "C" void lv_free_core(void *p)
{
    heap_caps_free(p);
}

extern "C" void *lv_realloc_core(void *p, size_t new_size)
{
    if (p == nullptr) {
        return alloc_psram_first(new_size);
    }
    void *q = heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (q == nullptr) {
        q = heap_caps_realloc(p, new_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return q;
}

extern "C" void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    if (mon_p == nullptr) {
        return;
    }
    // Report the PSRAM heap; that is where LVGL memory lives.
    *mon_p = lv_mem_monitor_t{};
    const size_t free_sz = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    mon_p->total_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    mon_p->free_size = free_sz;
    mon_p->free_biggest_size = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    mon_p->used_pct = mon_p->total_size > 0
                          ? static_cast<uint8_t>(100u - (free_sz * 100u) / mon_p->total_size)
                          : 0;
    mon_p->frag_pct = mon_p->free_size > 0
                          ? static_cast<uint8_t>(100u - (mon_p->free_biggest_size * 100u) / mon_p->free_size)
                          : 0;
}

extern "C" lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}

#endif // CONFIG_LV_USE_CUSTOM_MALLOC
