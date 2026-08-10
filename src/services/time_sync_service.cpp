#include "services/time_sync_service.h"

#include <esp_log.h>
#include <esp_sntp.h>

#include <atomic>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

namespace {

constexpr const char *TAG = "TIME_SYNC";

// 0 = not started, 1 = one caller is configuring lwIP SNTP, 2 = started.
std::atomic<uint8_t> s_start_state{0};

} // namespace

bool TIME_SYNC_StartIfNeeded()
{
    if (esp_sntp_enabled()) {
        s_start_state.store(2, std::memory_order_release);
        return false;
    }

    uint8_t expected = 0;
    if (!s_start_state.compare_exchange_strong(
            expected, 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return false;
    }

    // Recheck after winning the start race in case another IDF component
    // enabled SNTP immediately before the atomic transition.
    if (!esp_sntp_enabled()) {
        setenv("TZ", "CST-8", 1);
        tzset();
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, const_cast<char *>("ntp.aliyun.com"));
        esp_sntp_setservername(1, const_cast<char *>("ntp.ntsc.ac.cn"));
        esp_sntp_setservername(2, const_cast<char *>("pool.ntp.org"));
        esp_sntp_init();
        ESP_LOGI(TAG, "SNTP client started");
    }

    s_start_state.store(2, std::memory_order_release);
    return true;
}
