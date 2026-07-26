#include <esp_event.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_psram.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

#include "../services/config_notify.h"
#include "../lib/ble_config.h"
#include "../services/ai_assistant.h"
#include "../services/aprs_service.h"
#include "../services/cellular_rndis_service.h"
#include "../services/signaling_service.h"
#include "../services/espnow_link.h"
#include "../services/lan_discovery.h"
#include "../services/video_call.h"
#include "../services/music_player.h"
#include "../services/music_playlist.h"
#include "../services/nanny.h"
#include "../services/ota_service.h"
#include "../services/radio_favorites.h"
#include "../services/storage_service.h"
#include "../lib/nrl_audio_bridge.h"
#include "../lib/improv_protocol.h"
#include "../lib/nrl_bt_hfp.h"
#include "../lib/nrl_ethernet.h"
#include "../lib/nrl_net_compat.h"
#include "../lib/nrl_usb_console.h"
#include "../lib/nrl_version.h"
#include "../lib/nrl_wifi.h"
#include "../lib/wifi_config_portal.h"
#include "driver/board_pins.h"
#include "driver/display.h"
#include "driver/es7210.h"
#include "driver/es8311.h"
#include "driver/es8389.h"
#include "driver/external_radio.h"
#include "driver/status_io.h"
#include "main_loop_profile.h"

#include <string.h>

namespace {

constexpr unsigned long kPollIntervalMs = 20UL;
const char *TAG = "APP";

portMUX_TYPE s_loop_profile_lock = portMUX_INITIALIZER_UNLOCKED;
volatile bool s_loop_profile_enabled = false;
uint32_t s_loop_profile_loops = 0u;
uint64_t s_loop_profile_total_us[MAIN_LOOP_STAGE_COUNT] = {};
uint32_t s_loop_profile_max_us[MAIN_LOOP_STAGE_COUNT] = {};
bool s_display_started = false;
bool s_full_app_started = false;
bool s_waiting_for_provisioning = false;
uint32_t s_next_provision_wifi_attempt_ms = 0u;
uint32_t s_seen_config_generation = 0u;

static void recordMainLoopProfile(const uint32_t elapsed_us[MAIN_LOOP_STAGE_COUNT])
{
    portENTER_CRITICAL(&s_loop_profile_lock);
    if (s_loop_profile_enabled) {
        ++s_loop_profile_loops;
        for (size_t i = 0; i < MAIN_LOOP_STAGE_COUNT; ++i) {
            s_loop_profile_total_us[i] += elapsed_us[i];
            if (elapsed_us[i] > s_loop_profile_max_us[i]) {
                s_loop_profile_max_us[i] = elapsed_us[i];
            }
        }
    }
    portEXIT_CRITICAL(&s_loop_profile_lock);
}

// Boot-time internal-DRAM ruler: logs free/largest-block after each init step
// so a regression that starves the audio/AEC init (which needs contiguous
// internal RAM late in boot) can be pinned to the step that ate it.
static void logDramMark(const char *step)
{
    ESP_LOGI(TAG, "[DRAM] after %-14s free=%6u largest=%6u", step,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
}

static inline uint32_t nowMsApp()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

static bool wifiConfigured()
{
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    return config != nullptr && config->wifi_enabled &&
           EXTERNAL_RADIO_GetWifiProfileCount() > 0u;
}

static WifiConnResult connectSavedWifiLight()
{
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    if (config == nullptr || !config->wifi_enabled) {
        return WIFI_CONN_FAILED;
    }

    const size_t count = EXTERNAL_RADIO_GetWifiProfileCount();
    if (count == 0u) {
        return WIFI_CONN_FAILED;
    }

    // Provisioning always promotes the newly selected network to slot 0.
    // Try it directly before an active scan: scanning while the phone still
    // holds the BLE link can monopolize the shared 2.4 GHz radio and make the
    // mini program look disconnected before it receives the result.
    ESP_LOGI(TAG, "provisioning: trying primary WiFi 1/%u: \"%s\"",
             static_cast<unsigned>(count), config->wifi_profiles[0].ssid);
    WifiConnResult last = wifiEnsureConnected(config->wifi_profiles[0].ssid,
                                              config->wifi_profiles[0].password, 10000u);
    if (last == WIFI_CONN_OK || last == WIFI_ALREADY_ON_SSID) {
        return last;
    }
    if (count == 1u) return last;

    NrlWifiScanResult scanned[EXTERNAL_RADIO_MAX_WIFI_PROFILES] = {};
    const bool scan_ok = nrlWifiScanStartBlocking(5000u);
    const size_t scanned_count = scan_ok ? nrlWifiScanGetCache(scanned, EXTERNAL_RADIO_MAX_WIFI_PROFILES) : 0u;
    bool attempted[EXTERNAL_RADIO_MAX_WIFI_PROFILES] = {};
    attempted[0] = true;

    for (size_t i = 1; i < count; ++i) {
        bool visible = !scan_ok;
        for (size_t j = 0; j < scanned_count; ++j) {
            if (strcmp(config->wifi_profiles[i].ssid, scanned[j].ssid) == 0) {
                visible = true;
                break;
            }
        }
        if (!visible) {
            continue;
        }
        attempted[i] = true;
        ESP_LOGI(TAG, "provisioning: trying saved WiFi %u/%u: \"%s\"",
                 static_cast<unsigned>(i + 1u),
                 static_cast<unsigned>(count),
                 config->wifi_profiles[i].ssid);
        last = wifiEnsureConnected(config->wifi_profiles[i].ssid,
                                   config->wifi_profiles[i].password, 10000u);
        if (last == WIFI_CONN_OK || last == WIFI_ALREADY_ON_SSID) {
            return last;
        }
    }

    for (size_t i = 0; i < count; ++i) {
        if (attempted[i]) {
            continue;
        }
        ESP_LOGI(TAG, "provisioning: trying hidden/unseen saved WiFi %u/%u: \"%s\"",
                 static_cast<unsigned>(i + 1u),
                 static_cast<unsigned>(count),
                 config->wifi_profiles[i].ssid);
        last = wifiEnsureConnected(config->wifi_profiles[i].ssid,
                                   config->wifi_profiles[i].password, 10000u);
        if (last == WIFI_CONN_OK || last == WIFI_ALREADY_ON_SSID) {
            return last;
        }
    }

    return last;
}

static void initSystem()
{
    // The IDF gpio driver logs every gpio_config() at INFO level; the
    // bit-bang I2C reconfigures SDA/SCL constantly and floods the console.
    esp_log_level_set("gpio", ESP_LOG_WARN);
    // esp-sr's AFE prints "Ringbuffer of AFE is empty, Please use feed() to
    // write data" at WARN whenever its fetch outruns feed. The design here
    // only feeds the AEC reference channel while network voice is actually
    // playing (gezipai + bh4tdv both); during idle/RX-only the WARN spams
    // every ~200 ms. Suppress it; real errors still surface at ESP_LOG_ERROR.
    esp_log_level_set("AFE", ESP_LOG_ERROR);

    // One-time stack init that Arduino used to do before setup(). Idempotent;
    // nrl_wifi.cpp's nrlWifiInit() re-runs these but they short-circuit on
    // INVALID_STATE. Doing them here keeps boot ordering deterministic.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    esp_netif_init();
    esp_event_loop_create_default();

    NRL_USB_Console_Init();

    ESP_LOGI(TAG, "%s startup", NRL_FIRMWARE_BANNER);
    uint32_t flash_size = 0;
    (void)esp_flash_get_size(nullptr, &flash_size);
    const size_t psram_size = esp_psram_get_size();
    ESP_LOGI(TAG, "[BOOT] flash=%u bytes psram=%u bytes (psram %s)",
             static_cast<unsigned>(flash_size),
             static_cast<unsigned>(psram_size),
             psram_size > 0 ? "OK" : "absent");
}

static void applyPendingAudioConfig()
{
#if defined(NRL_AUDIO_CODEC_ES8311) && NRL_AUDIO_CODEC_ES8311
    if (const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig()) {
        ES8311_ApplyAudioConfig(config->mic_volume,
                                config->line_out_volume,
                                config->hp_drive_enabled,
                                config->drc_enabled,
                                config->drc_winsize,
                                config->drc_maxlevel,
                                config->drc_minlevel,
                                config->dac_ramprate,
                                config->dac_eq_bypass,
                                config->daceq_b0,
                                config->daceq_b1,
                                config->daceq_a1,
                                config->adc_dmic_enabled,
                                config->adc_linsel,
                                config->adc_pga_gain,
                                config->adc_ramprate,
                                config->adc_dmic_sense,
                                config->adc_sync,
                                config->adc_inv,
                                config->adc_ramclr,
                                config->adc_scale,
                                config->alc_enabled,
                                config->adc_automute_enabled,
                                config->alc_winsize,
                                config->alc_maxlevel,
                                config->alc_minlevel,
                                config->adc_automute_winsize,
                                config->adc_automute_noise_gate,
                                config->adc_automute_volume,
                                config->adc_hpfs1,
                                config->adc_eq_bypass,
                                config->adc_hpf,
                                config->adc_hpfs2,
                                config->adceq_b0,
                                config->adceq_a1,
                                config->adceq_a2,
                                config->adceq_b1,
                                config->adceq_b2);
        AUDIO_SetMicHpfEnabled(config->mic_hpf_enabled);
    }
#else
    if (const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig()) {
        AUDIO_SetMicHpfEnabled(config->mic_hpf_enabled);
    }
#endif
}

static bool s_storage_music_started = false;

static void initStorageAndMusic()
{
    if (s_storage_music_started) {
        return;
    }

    STORAGE_Init();
    MUSIC_Init();
    PLAYLIST_Init();
    RADIO_FAV_Init();
    NANNY_Init();
    if (STORAGE_SdMounted()) {
        PLAYLIST_Scan();
    }
    s_storage_music_started = true;
    logDramMark("storage+music");
}

static void initDisplay()
{
    if (s_display_started) {
        return;
    }
#if defined(NRL_HAS_DISPLAY) && NRL_HAS_DISPLAY && !(defined(NRL_SKIP_DISPLAY_INIT) && NRL_SKIP_DISPLAY_INIT)
    Display_Init();
    s_display_started = true;
    logDramMark("display");
#else
    s_display_started = true;
#endif
}

static bool initFullApp()
{
    if (s_full_app_started) {
        return true;
    }

    initStorageAndMusic();
    initDisplay();

    // Bring the audio bridge up BEFORE ES8311_Init. ES8311_Init internally
    // calls AEC_Init which mallocs ~50 KB (WebRtcNs + AFE state) from the
    // internal SRAM heap; if the bridge task's 8 KB stack hasn't been
    // allocated by then there's no contiguous block left and xTaskCreate
    // fails -- which silently kills STA reconnect (the bridge owns the
    // WiFi state machine). NRLAudioBridge_Init only registers its audio
    // router sinks/routes; frames only flow once ES8311 starts its
    // passthrough task, so the ordering swap is safe.
    if (!NRLAudioBridge_Init()) {
        ESP_LOGE(TAG, "NRL audio bridge init failed.");
        return false;
    }
    logDramMark("audio_bridge");

    // Bluetooth headset (HFP) voice link. Brought up only if enabled in config;
    // on non-S31 boards these are no-ops.
    NRL_BtHfp_Init();
    if (const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig()) {
        NRL_BtHfp_SetEnabled(config->bt_enabled);
    }
    logDramMark("bt_hfp");

    // ESP-NOW off-grid voice link: restores its persisted enable state,
    // waiting for the bridge task to start WiFi if needed.
    ESPNOW_LINK_Init();
    logDramMark("espnow");

    // APRS transceiver: GPS beacons to APRS-IS and/or AFSK over the radio,
    // plus RF demodulation into the station list. Idles until enabled.
    APRS_SERVICE_Init();
    logDramMark("aprs");

    // xiaozhi AI assistant: reconnects in the background when configured.
    AI_Init();

    // Video call: passive RX buffering; camera TX starts on demand.
    VIDEO_Init();
    logDramMark("ai+video");

#if defined(NRL_AUDIO_CODEC_ES8311) && NRL_AUDIO_CODEC_ES8311
    if (ES8311_Init()) {
        ESP_LOGI(TAG, "ES8311 ready.");
    } else {
        ESP_LOGE(TAG, "ES8311 initialization failed.");
    }

    if (!ES8311_SetReceiveMode()) {
        ESP_LOGE(TAG, "ES8311 set receive mode failed.");
    }
#elif defined(NRL_AUDIO_CODEC_ES8389) && NRL_AUDIO_CODEC_ES8389
    if (ES8389_Init()) {
        ESP_LOGI(TAG, "ES8389 ready.");
    } else {
        ESP_LOGE(TAG, "ES8389 initialization failed.");
    }

    if (!ES8389_SetReceiveMode()) {
        ESP_LOGE(TAG, "ES8389 set receive mode failed.");
    }

    // The ES8389 driver has no pending-config cache (unlike ES8311), so the
    // persisted volume/gain must be pushed after the codec device exists.
    if (const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig()) {
        ES8389_SetOutputVolume(config->line_out_volume);
        ES8389_SetInputGain(config->mic_volume);
    }
#endif

#if NRL_HAS_ES7210
    // The mic ADC on this board is a separate ES7210 chip. ES8311_Init()
    // above has already started I2S, so MCLK/BCLK/LRCK are running and the
    // ES7210 can lock its clock. Configure it now so I2S DIN carries audio.
    if (const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig()) {
        ES7210_SetMicVolume(config->mic_volume);
    }
    if (ES7210_Init()) {
        ESP_LOGI(TAG, "ES7210 mic ADC ready.");
    } else {
        ESP_LOGE(TAG, "ES7210 mic ADC initialization failed.");
    }
#endif

    // Signaling allocates MDC decoder state and starts its worker. Keep it
    // after codec/AEC initialization so it cannot consume the contiguous
    // internal RAM required to bring up speaker playback.
    SIGNALING_Init();
    logDramMark("signaling");

    s_full_app_started = true;
    s_waiting_for_provisioning = false;
    Display_SetProvisioningMode(false);

    ESP_LOGI(TAG, "[AT] serial console ready. Type \"AT\" for the command list, "
                  "e.g. AT+WIFI_SSID=MyNet then AT+WIFI_PASS=secret");
    return true;
}

static void initApp()
{
    logDramMark("boot");
    EXTERNAL_RADIO_Init();
    applyPendingAudioConfig();
    if (!nrlEthernetInit()) {
        ESP_LOGE(TAG, "Ethernet initialization failed; Wi-Fi fallback remains available.");
    }
    logDramMark("ethernet");
    if (!CELLULAR_RNDIS_Init()) {
        ESP_LOGW(TAG, "USB RNDIS 4G initialization failed; Wi-Fi fallback remains available.");
    }
    logDramMark("usb4g");

    STATUS_IO_Init();

    // On a factory-fresh device reserve the BLE host/controller memory before
    // the config portal starts WiFi, performs its pre-scan and creates the HTTP
    // server. Those allocations previously happened first and could push both
    // Gezipai and BI4UMD below BLEConfig_Init's internal-RAM safety threshold,
    // leaving no NRL-ESP32-CFG advertisement at all.
    s_seen_config_generation = CONFIG_NOTIFY_Generation();
    if (!nrlExternalNetworkConnected() && !wifiConfigured()) {
        s_waiting_for_provisioning = true;
        ESP_LOGI(TAG, "WiFi not configured; reserving BLE before config portal");
        if (!BLEConfig_Init()) {
            ESP_LOGE(TAG, "BLE provisioning init failed before config portal");
        }
        logDramMark("ble_config");
    }

    WifiConfigPortal_Init();
    LAN_DISCOVERY_Init();
    OtaService_Init();
    logDramMark("portal+ota");

    if (nrlExternalNetworkConnected()) {
        ESP_LOGI(TAG, "External network online; starting full app without BLE provisioning");
        if (s_waiting_for_provisioning) {
            BLEConfig_Stop();
        }
        initFullApp();
    } else if (wifiConfigured()) {
        ESP_LOGI(TAG, "WiFi configured; starting full app without BLE provisioning");
        initFullApp();
    } else {
        ESP_LOGI(TAG, "WiFi not configured; starting light provisioning mode");
        // BLE was reserved before the WiFi portal. The display remains
        // available, while storage, music, audio and the other large services
        // stay deferred until provisioning succeeds.
        Display_SetProvisioningMode(true);
        initDisplay();
        logDramMark("provision_display");
        ESP_LOGI(TAG, "[AT] serial console ready in provisioning mode. Configure WiFi via web, BLE, or AT.");
    }
}

static void pollProvisioningStart()
{
    if (!s_waiting_for_provisioning || s_full_app_started) {
        return;
    }

    const uint32_t generation = CONFIG_NOTIFY_Generation();
    if (generation != s_seen_config_generation) {
        s_seen_config_generation = generation;
        s_next_provision_wifi_attempt_ms = 0u;
    }

    if (nrlExternalNetworkConnected()) {
        ESP_LOGI(TAG, "provisioning complete: external network online, stopping BLE before full app start");
        BLEConfig_Stop();
        initFullApp();
        return;
    }

    if (!wifiConfigured()) {
        return;
    }

    const uint32_t now = nowMsApp();
    if (s_next_provision_wifi_attempt_ms != 0u &&
        static_cast<int32_t>(now - s_next_provision_wifi_attempt_ms) < 0) {
        return;
    }

    ESP_LOGI(TAG, "provisioning: saved WiFi found, connecting before full app start");
    const WifiConnResult result = connectSavedWifiLight();
    if (result == WIFI_CONN_OK || result == WIFI_ALREADY_ON_SSID || nrlWifiStaConnected()) {
        ESP_LOGI(TAG, "provisioning: WiFi connected, stopping BLE before full app start");
        BLEConfig_ReportWifiResult(true);
        // Give the final GOT_IP notification one connection interval to leave
        // the controller before deinitializing BLE for the full application.
        vTaskDelay(pdMS_TO_TICKS(300));
        BLEConfig_Stop();
        initFullApp();
        return;
    }

    BLEConfig_ReportWifiResult(false);
    s_next_provision_wifi_attempt_ms = nowMsApp() + 30000u;
    ESP_LOGW(TAG, "provisioning: WiFi connect failed (%u), retrying later",
             static_cast<unsigned>(result));
}

static void mainLoopTask(void *)
{
    // The address pins down which heap region the boot-tail 6 KB stack
    // allocation landed in (main SRAM vs the slow RTCRAM heap region); a
    // stack in slow memory drags every poll made from this task.
    int stack_probe = 0;
    ESP_LOGI(TAG, "main loop stack ~%p internal_largest=%u rtc_free=%u",
             static_cast<void *>(&stack_probe),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
#ifdef MALLOC_CAP_RTCRAM
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_RTCRAM))
#else
             0u
#endif
    );
    (void)stack_probe;
    while (true) {
        if (!s_loop_profile_enabled) {
            STATUS_IO_Poll();
            WifiConfigPortal_Poll();
            LAN_DISCOVERY_Poll();
            BLEConfig_Poll();
            pollProvisioningStart();
            if (s_full_app_started) {
                NRL_BtHfp_Poll();
            }
            NRLAudioBridge_PollSerialConsole();
#if defined(NRL_HAS_DISPLAY) && NRL_HAS_DISPLAY && !(defined(NRL_SKIP_DISPLAY_INIT) && NRL_SKIP_DISPLAY_INIT)
            if (s_display_started) {
                Display_Poll();
            }
#endif
        } else {
            uint32_t elapsed_us[MAIN_LOOP_STAGE_COUNT] = {};
            int64_t started_us = esp_timer_get_time();
            STATUS_IO_Poll();
            elapsed_us[MAIN_LOOP_STAGE_STATUS] =
                static_cast<uint32_t>(esp_timer_get_time() - started_us);

            started_us = esp_timer_get_time();
            WifiConfigPortal_Poll();
            LAN_DISCOVERY_Poll();
            elapsed_us[MAIN_LOOP_STAGE_WIFI_PORTAL] =
                static_cast<uint32_t>(esp_timer_get_time() - started_us);

            started_us = esp_timer_get_time();
            BLEConfig_Poll();
            elapsed_us[MAIN_LOOP_STAGE_BLE] =
                static_cast<uint32_t>(esp_timer_get_time() - started_us);

            pollProvisioningStart();

            started_us = esp_timer_get_time();
            if (s_full_app_started) {
                NRL_BtHfp_Poll();
            }
            elapsed_us[MAIN_LOOP_STAGE_BT] =
                static_cast<uint32_t>(esp_timer_get_time() - started_us);

            started_us = esp_timer_get_time();
            NRLAudioBridge_PollSerialConsole();
            elapsed_us[MAIN_LOOP_STAGE_SERIAL] =
                static_cast<uint32_t>(esp_timer_get_time() - started_us);

#if defined(NRL_HAS_DISPLAY) && NRL_HAS_DISPLAY && !(defined(NRL_SKIP_DISPLAY_INIT) && NRL_SKIP_DISPLAY_INIT)
            started_us = esp_timer_get_time();
            if (s_display_started) {
                Display_Poll();
            }
            elapsed_us[MAIN_LOOP_STAGE_DISPLAY] =
                static_cast<uint32_t>(esp_timer_get_time() - started_us);
#endif
            recordMainLoopProfile(elapsed_us);
        }
        vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));
    }
}

} // namespace

void MAIN_LOOP_PROFILE_SetEnabled(const bool enabled)
{
    portENTER_CRITICAL(&s_loop_profile_lock);
    if (enabled) {
        s_loop_profile_loops = 0u;
        memset(s_loop_profile_total_us, 0, sizeof(s_loop_profile_total_us));
        memset(s_loop_profile_max_us, 0, sizeof(s_loop_profile_max_us));
    }
    s_loop_profile_enabled = enabled;
    portEXIT_CRITICAL(&s_loop_profile_lock);
}

void MAIN_LOOP_PROFILE_Get(MainLoopProfileSnapshot *snapshot)
{
    if (snapshot == nullptr) {
        return;
    }
    portENTER_CRITICAL(&s_loop_profile_lock);
    snapshot->enabled = s_loop_profile_enabled;
    snapshot->loops = s_loop_profile_loops;
    memcpy(snapshot->total_us, s_loop_profile_total_us, sizeof(snapshot->total_us));
    memcpy(snapshot->max_us, s_loop_profile_max_us, sizeof(snapshot->max_us));
    portEXIT_CRITICAL(&s_loop_profile_lock);
}

// Static in .bss: this task is created at the very tail of boot, when the
// internal heap is down to its last fragments. A heap-allocated stack then
// lands in whatever region still has a hole -- on one production S31 that was
// the slow RTCRAM heap block (stack @0x2e007594), which made every poll this
// task runs ~4x slower and cost ~10% of core0. A fixed .bss buffer keeps the
// stack in main SRAM on every board, every boot.
constexpr size_t kMainLoopStackBytes = 8192;
static StackType_t s_main_loop_stack[kMainLoopStackBytes / sizeof(StackType_t)];
static StaticTask_t s_main_loop_tcb;

extern "C" void app_main(void)
{
    initSystem();
    initApp();
    // Detach the polling loop from the boot task so we can let app_main return
    // (IDF style). 6 KB stack is enough for the poll fan-out; bump if a
    // sub-system asks for more local stack down the line.
    //
    // Pinned to core 0 (where WiFi, lwIP TCPIP and bridgeTask live). Core 1
    // is reserved for the audio pipeline (es8311 passthrough + aec_fetch).
    // Keeping polls off core 1 stops Display_Poll / WifiConfigPortal_Poll
    // from stealing audio-task CPU and bunching outbound voice packets.
    xTaskCreateStaticPinnedToCore(mainLoopTask, "nrl_main_loop", kMainLoopStackBytes,
                                  nullptr, 5, s_main_loop_stack, &s_main_loop_tcb, 0);
}
