#include "lib/heap_report.h"

#include "lib/nrl_psram.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <stdio.h>

#if CONFIG_HEAP_TASK_TRACKING
#include <esp_heap_task_info.h>
#endif

static const char *TAG = "HEAP";

extern "C" void NRL_HEAP_Report(void)
{
    ESP_LOGI(TAG, "internal: free=%u largest=%u min-ever=%u | psram: free=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             esp_get_minimum_free_heap_size(),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));

#if CONFIG_HEAP_TASK_TRACKING
    // Bucket 0 collects internal-RAM bytes, bucket 1 everything else (PSRAM).
    static heap_task_totals_t totals[48];
    size_t num_totals = 0;
    heap_task_info_params_t params = {};
    params.caps[0] = MALLOC_CAP_INTERNAL;
    params.mask[0] = MALLOC_CAP_INTERNAL;
    params.totals = totals;
    params.num_totals = &num_totals;
    params.max_totals = 48;
    heap_caps_get_per_task_info(&params);

    // Snapshot the live tasks first: a totals entry can reference a task
    // that has since been deleted (audio_passthrough restarts, one-shot
    // workers), and dereferencing its stale TCB/name pointer faults.
    static NRL_PSRAM_BSS TaskStatus_t live[64];
    const UBaseType_t live_count = uxTaskGetSystemState(live, 64, nullptr);

    // Insertion sort by internal bytes, descending; show the top consumers.
    size_t order[48];
    for (size_t i = 0; i < num_totals; ++i) {
        order[i] = i;
    }
    for (size_t i = 1; i < num_totals; ++i) {
        const size_t key = order[i];
        size_t j = i;
        while (j > 0 && totals[order[j - 1]].size[0] < totals[key].size[0]) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = key;
    }
    const size_t shown = num_totals < 12 ? num_totals : 12;
    for (size_t i = 0; i < shown; ++i) {
        const heap_task_totals_t *t = &totals[order[i]];
        if (t->size[0] == 0u && t->size[1] == 0u) {
            break;
        }
        char name[configMAX_TASK_NAME_LEN] = {};
        const char *shown_name = "(pre-task)";
        if (t->task != nullptr) {
            shown_name = "(deleted)";
            for (UBaseType_t k = 0; k < live_count; ++k) {
                if (live[k].xHandle == t->task && live[k].pcTaskName != nullptr) {
                    snprintf(name, sizeof(name), "%s", live[k].pcTaskName);
                    shown_name = name;
                    break;
                }
            }
        }
        ESP_LOGI(TAG, "  %-16.16s internal=%7u B (%3u blk)  all=%7u B",
                 shown_name,
                 static_cast<unsigned>(t->size[0]),
                 static_cast<unsigned>(t->count[0]),
                 static_cast<unsigned>(t->size[0] + t->size[1]));
    }
#endif
}
