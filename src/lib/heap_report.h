#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Log an internal-DRAM / PSRAM snapshot (free, largest block, min-ever
// watermark). With CONFIG_HEAP_TASK_TRACKING enabled it additionally logs the
// top per-task consumers of internal RAM -- used to answer "who ate the DRAM"
// when the WiFi driver or AEC runs out of internal heap.
void NRL_HEAP_Report(void);

#ifdef __cplusplus
}
#endif
