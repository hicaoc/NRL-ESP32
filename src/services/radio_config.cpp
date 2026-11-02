#include "radio_config.h"

#include "config_notify.h"
#include "../app/driver/board_pins.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <nvs.h>

#if NRL_BOARD == NRL_BOARD_BH4TDV_RF
#include "../app/driver/bh4tdv_rf_io.h"
#include "../app/driver/sr110u.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace {

constexpr const char *kNvsNamespace = "radio";
constexpr const char *kNvsKey = "sr110u";
constexpr uint32_t kPersistMagic = 0x53523155u; // 'SR1U'
// v2: force defaults once. v1 could persist low_power=false (high power),
// and keying 1 W on an unverified 5 V rail browns the board out.
// v4: adds radio_type (RJ11 board YAESU/MOTO select).
constexpr uint8_t kPersistVersion = 4u;

// UHF modules (SR-110U) cover 400-480 MHz; the VHF sibling (SR-110V,
// version string "110V-...") covers 136-174 MHz. Accept both bands and let
// the module itself reject what its hardware cannot do.
constexpr uint32_t kUhfMinFreqHz = 400000000u;
constexpr uint32_t kUhfMaxFreqHz = 480000000u;
constexpr uint32_t kVhfMinFreqHz = 136000000u;
constexpr uint32_t kVhfMaxFreqHz = 174000000u;
constexpr uint32_t kFreqStepHz = 2500u; // datasheet: 6.25K or 2K5 multiples

bool freqInBand(const uint32_t hz)
{
    return (hz >= kUhfMinFreqHz && hz <= kUhfMaxFreqHz) ||
           (hz >= kVhfMinFreqHz && hz <= kVhfMaxFreqHz);
}

struct PersistBlob {
    uint32_t magic;
    uint8_t version;
    uint8_t enabled;
    uint8_t flags0;      // bit0 busy_lockout, bit1 narrowband, bit2 low_power
    uint8_t squelch;
    uint32_t rx_freq_hz;
    uint32_t tx_freq_hz;
    uint8_t rx_tone_type;
    uint8_t tx_tone_type;
    uint16_t rx_tone_value;
    uint16_t tx_tone_value;
    uint8_t mic_level;
    uint8_t tot;
    uint8_t scramble;
    uint8_t compander;
    uint8_t volume;
    uint8_t power_save;
    uint8_t vox;
    uint8_t radio_type;
    uint8_t reserved[7];
};

RadioModuleConfig s_config = {};
bool s_loaded = false;

// Incremental apply: each DMO command covers one parameter group. Set() marks
// the groups that actually changed; a group that the module rejects stays
// dirty and is retried on the next apply instead of blocking the others.
constexpr uint8_t kDirtyGroup = 1u << 0;  // DMOGRP: freq/tone/flags
constexpr uint8_t kDirtyFunc  = 1u << 1;  // DMOFUN: squelch/mic/tot/scramble/compander
constexpr uint8_t kDirtyVol   = 1u << 2;  // DMOVOL
constexpr uint8_t kDirtySav   = 1u << 3;  // DMOSAV
constexpr uint8_t kDirtyVox   = 1u << 4;  // DMOVOX
constexpr uint8_t kDirtyAll   = 0x1Fu;
uint8_t s_dirty = kDirtyAll;

void defaultConfig(RadioModuleConfig *config)
{
    *config = RadioModuleConfig{};
    config->enabled = true;
    // Module factory defaults: 450.05000 MHz, 67.0 Hz CTCSS both ways.
    config->rx_freq_hz = 450050000u;
    config->tx_freq_hz = 450050000u;
    config->rx_tone = {RADIO_TONE_CTCSS, 670u};
    config->tx_tone = {RADIO_TONE_CTCSS, 670u};
    config->busy_lockout = false;
    config->narrowband = false;
    config->low_power = true;
    config->squelch = 2u;
    config->mic_level = 5u;
    config->tot = 0u;
    config->scramble = 0u;
    config->compander = false;
    config->volume = 6u;
    // Module factory default is power saving ON (DMOSAV=0); match it so the
    // boot-time full sync does not silently change the module.
    config->power_save = true;
    config->vox = 0u;
    config->radio_type = 0u; // non-YAESU/MOTO radios: P1.4 low
}

bool toneValid(const RadioTone &tone)
{
    switch (tone.type) {
        case RADIO_TONE_NONE:
            return true;
        case RADIO_TONE_CTCSS:
            // 60.0..254.0 Hz, covering the standard 67.0..250.3 EIA list.
            return tone.value >= 600u && tone.value <= 2540u;
        case RADIO_TONE_CDCSS_N:
        case RADIO_TONE_CDCSS_I: {
            // Three octal-style digits, e.g. 23 = D023, 251 = D251.
            if (tone.value > 0777u) return false;
            const unsigned digits[3] = {tone.value % 10u,
                                        (tone.value / 10u) % 10u,
                                        (tone.value / 100u) % 10u};
            return digits[0] <= 7u && digits[1] <= 7u && digits[2] <= 7u;
        }
        default:
            return false;
    }
}

bool configValid(const RadioModuleConfig *config)
{
    if (config == nullptr) return false;
    const bool rx_ok = freqInBand(config->rx_freq_hz) &&
                       config->rx_freq_hz % kFreqStepHz == 0u;
    const bool tx_ok = freqInBand(config->tx_freq_hz) &&
                       config->tx_freq_hz % kFreqStepHz == 0u;
    return rx_ok && tx_ok &&
           toneValid(config->rx_tone) && toneValid(config->tx_tone) &&
           config->squelch <= 8u && config->mic_level <= 8u &&
           config->tot <= 9u && config->scramble <= 7u &&
           config->volume >= 1u && config->volume <= 9u &&
           config->vox <= 8u && config->radio_type <= 1u;
}

void saveConfig()
{
    PersistBlob blob = {};
    blob.magic = kPersistMagic;
    blob.version = kPersistVersion;
    blob.enabled = s_config.enabled ? 1u : 0u;
    blob.flags0 = (s_config.busy_lockout ? 1u : 0u) |
                  (s_config.narrowband ? 2u : 0u) |
                  (s_config.low_power ? 4u : 0u);
    blob.squelch = s_config.squelch;
    blob.rx_freq_hz = s_config.rx_freq_hz;
    blob.tx_freq_hz = s_config.tx_freq_hz;
    blob.rx_tone_type = s_config.rx_tone.type;
    blob.tx_tone_type = s_config.tx_tone.type;
    blob.rx_tone_value = s_config.rx_tone.value;
    blob.tx_tone_value = s_config.tx_tone.value;
    blob.mic_level = s_config.mic_level;
    blob.tot = s_config.tot;
    blob.scramble = s_config.scramble;
    blob.compander = s_config.compander ? 1u : 0u;
    blob.volume = s_config.volume;
    blob.power_save = s_config.power_save ? 1u : 0u;
    blob.vox = s_config.vox;
    blob.radio_type = s_config.radio_type;

    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) == ESP_OK) {
        (void)nvs_set_blob(nvs, kNvsKey, &blob, sizeof(blob));
        (void)nvs_commit(nvs);
        nvs_close(nvs);
    }
    CONFIG_NOTIFY_Bump();
}

void loadConfig()
{
    defaultConfig(&s_config);

    PersistBlob blob = {};
    size_t size = sizeof(blob);
    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    const esp_err_t err = nvs_get_blob(nvs, kNvsKey, &blob, &size);
    nvs_close(nvs);
    if (err != ESP_OK || size != sizeof(blob) ||
        blob.magic != kPersistMagic || blob.version != kPersistVersion) {
        return;
    }

    RadioModuleConfig loaded = {};
    loaded.enabled = blob.enabled != 0u;
    loaded.busy_lockout = (blob.flags0 & 1u) != 0u;
    loaded.narrowband = (blob.flags0 & 2u) != 0u;
    loaded.low_power = (blob.flags0 & 4u) != 0u;
    loaded.squelch = blob.squelch;
    loaded.rx_freq_hz = blob.rx_freq_hz;
    loaded.tx_freq_hz = blob.tx_freq_hz;
    loaded.rx_tone = {blob.rx_tone_type, blob.rx_tone_value};
    loaded.tx_tone = {blob.tx_tone_type, blob.tx_tone_value};
    loaded.mic_level = blob.mic_level;
    loaded.tot = blob.tot;
    loaded.scramble = blob.scramble;
    loaded.compander = blob.compander != 0u;
    loaded.volume = blob.volume;
    loaded.power_save = blob.power_save != 0u;
    loaded.vox = blob.vox;
    loaded.radio_type = blob.radio_type;
    // Never resurrect a corrupt blob: fall back to defaults instead.
    if (configValid(&loaded)) {
        s_config = loaded;
    }
}

#if NRL_BOARD == NRL_BOARD_BH4TDV_RF

constexpr const char *TAG = "RADIO_CFG";
bool s_module_powered = false;

// The module's reply carries the verb followed by a status digit, with
// inconsistent separators ("+DMOGRP:0", "+DMOVOL0", "+DMOVOX: 0").
bool replyOk(const char *response, const char *verb)
{
    const char *p = strstr(response, verb);
    if (p == nullptr) return false;
    p += strlen(verb);
    while (*p != '\0' && !isdigit(static_cast<unsigned char>(*p))) ++p;
    return *p == '0';
}

// RXCT/TXCT are two raw bytes inside an otherwise ASCII command line.
// None is the raw pair FF FF; CTCSS is the BCD of (Hz x10),
// low byte first; CDCSS packs the code as BCD with the D1 MSB nibble 8 (N)
// or C (I) plus the code's hundreds digit.
size_t encodeTone(uint8_t *out, const RadioTone &tone, const bool rx)
{
    if (tone.type == RADIO_TONE_NONE) {
        // Datasheet: "no tone set" is the raw pair FF FF; the "RR"/"TT"
        // in the text example are only placeholders for where the tone
        // bytes go, not literal wire content.
        (void)rx;
        out[0] = 0xFFu;
        out[1] = 0xFFu;
        return 2u;
    }
    const unsigned v = tone.value;
    out[0] = static_cast<uint8_t>((((v / 10u) % 10u) << 4u) | (v % 10u));
    if (tone.type == RADIO_TONE_CTCSS) {
        out[1] = static_cast<uint8_t>((((v / 1000u) % 10u) << 4u) |
                                      ((v / 100u) % 10u));
    } else {
        const uint8_t polarity =
            tone.type == RADIO_TONE_CDCSS_N ? 0x80u : 0xC0u;
        out[1] = static_cast<uint8_t>(polarity | ((v / 100u) % 10u));
    }
    return 2u;
}

bool sendGroup(const RadioModuleConfig *config)
{
    uint8_t wire[80] = {};
    char head[48] = {};
    char rx_freq[16] = {};
    char tx_freq[16] = {};
    RADIO_CONFIG_FormatFreqMHz(config->rx_freq_hz, rx_freq, sizeof(rx_freq));
    RADIO_CONFIG_FormatFreqMHz(config->tx_freq_hz, tx_freq, sizeof(tx_freq));
    int length = snprintf(head, sizeof(head), "AT+DMOGRP=%s,%s,", rx_freq, tx_freq);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(head)) return false;
    size_t used = static_cast<size_t>(length);
    memcpy(wire, head, used);
    used += encodeTone(wire + used, config->rx_tone, true);
    wire[used++] = ',';
    used += encodeTone(wire + used, config->tx_tone, false);
    const unsigned flag = (config->busy_lockout ? 1u : 0u) |
                          (config->narrowband ? 2u : 0u);
    const unsigned flag1 = config->low_power ? 1u : 0u;
    const size_t base_used = used; // freq+tone part; flags appended per variant

    // Some module firmwares reject the Flag/Flag1 parameters outright
    // ("+DMOGRP:1" even for ",0,0"). Fall back in steps: full command,
    // factory-default flags, then no flag parameters at all (the H/L pin
    // already carries the power selection on this board).
    struct GroupVariant { bool with_flags; unsigned flag; unsigned flag1; };
    const GroupVariant variants[3] = {
        {true, flag, flag1},
        {true, 0u, 0u},
        {false, 0u, 0u},
    };

    char response[48] = {};
    bool ok = false;
    int tried = 0;
    for (int v = 0; v < 3 && !ok; ++v) {
        if (v == 1 && flag == 0u && flag1 == 0u) continue; // same as variant 0
        used = base_used;
        if (variants[v].with_flags) {
            length = snprintf(reinterpret_cast<char *>(wire + used), sizeof(wire) - used,
                              ",%u,%u", variants[v].flag, variants[v].flag1);
            if (length <= 0 || used + static_cast<size_t>(length) >= sizeof(wire)) break;
            used += static_cast<size_t>(length);
        }
        ++tried;
        for (int attempt = 0; attempt < 2 && !ok; ++attempt) {
            response[0] = 0;
            ok = SR110U_CommandRaw(wire, used, response, sizeof(response), 800u) &&
                 replyOk(response, "DMOGRP");
            if (response[0] != 0) break;  // explicit reply (incl. :1); retry only silence
            ESP_LOGW(TAG, "DMOGRP no reply (variant %d, attempt %d), retrying",
                     v, attempt + 1);
        }
        vTaskDelay(pdMS_TO_TICKS(30));
        ESP_LOGD(TAG, "DMOGRP rx: '%s'", response);
        if (!ok && response[0] != 0) {
            ESP_LOGW(TAG, "DMOGRP variant %d rejected: '%s'%s", v, response,
                     v + 1 < 3 ? ", trying simpler form" : "");
        }
    }
    if (!ok) {
        static const char kHexE[] = "0123456789ABCDEF";
        char dump[3 * 80 + 1] = {};
        size_t n = 0u;
        for (size_t i = 0u; i < used; ++i) {
            dump[n++] = kHexE[wire[i] >> 4u];
            dump[n++] = kHexE[wire[i] & 0x0Fu];
            dump[n++] = ' ';
        }
        ESP_LOGE(TAG,
                 "DMOGRP rejected after %d variant(s): '%s' (rfv=%s tfv=%s rxt=%u/%u txt=%u/%u flag=%u flag1=%u) tx: %s",
                 tried, response, rx_freq, tx_freq,
                 static_cast<unsigned>(config->rx_tone.type),
                 static_cast<unsigned>(config->rx_tone.value),
                 static_cast<unsigned>(config->tx_tone.type),
                 static_cast<unsigned>(config->tx_tone.value),
                 flag, flag1, dump);
    }
    return ok;
}

bool sendSimple(const char *verb, const unsigned a, const unsigned b,
                const unsigned c, const unsigned d, const unsigned e,
                const bool five_args)
{
    char command[48] = {};
    if (five_args) {
        snprintf(command, sizeof(command), "AT+%s=%u,%u,%u,%u,%u", verb, a, b, c, d, e);
    } else {
        snprintf(command, sizeof(command), "AT+%s=%u", verb, a);
    }
    char response[48] = {};
    ESP_LOGD(TAG, "tx: %s", command);
    bool ok = false;
    for (int attempt = 0; attempt < 2 && !ok; ++attempt) {
        response[0] = 0;
        ok = SR110U_Command(command, response, sizeof(response), 800u) &&
             replyOk(response, verb);
        if (response[0] != 0) break;  // explicit reply (incl. :1); retry only silence
        ESP_LOGW(TAG, "%s no reply (attempt %d), retrying", verb, attempt + 1);
    }
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_LOGD(TAG, "%s rx: '%s'", verb, response);
    if (!ok) ESP_LOGE(TAG, "%s rejected by module: '%s' (cmd %s)",
                      verb, response, command);
    return ok;
}

#endif // NRL_BOARD == NRL_BOARD_BH4TDV_RF

} // namespace

void RADIO_CONFIG_Init(void)
{
    if (!s_loaded) {
        loadConfig();
        s_loaded = true;
    }
}

const RadioModuleConfig *RADIO_CONFIG_Get(void)
{
    RADIO_CONFIG_Init();
    return &s_config;
}

bool RADIO_CONFIG_Set(const RadioModuleConfig *config, const bool persist)
{
    if (!configValid(config)) return false;
    uint8_t changed = 0u;
    if (config->rx_freq_hz != s_config.rx_freq_hz ||
        config->tx_freq_hz != s_config.tx_freq_hz ||
        config->rx_tone.type != s_config.rx_tone.type ||
        config->rx_tone.value != s_config.rx_tone.value ||
        config->tx_tone.type != s_config.tx_tone.type ||
        config->tx_tone.value != s_config.tx_tone.value ||
        config->busy_lockout != s_config.busy_lockout ||
        config->narrowband != s_config.narrowband ||
        config->low_power != s_config.low_power) {
        changed |= kDirtyGroup;
    }
    // Radio type is a bare PCA9555 level, not a module command; marking the
    // group dirty just guarantees ApplyToModule() runs and updates the pin.
    if (config->radio_type != s_config.radio_type) changed |= kDirtyGroup;
    if (config->squelch != s_config.squelch ||
        config->mic_level != s_config.mic_level ||
        config->tot != s_config.tot ||
        config->scramble != s_config.scramble ||
        config->compander != s_config.compander) {
        changed |= kDirtyFunc;
    }
    if (config->volume != s_config.volume) changed |= kDirtyVol;
    if (config->power_save != s_config.power_save) changed |= kDirtySav;
    if (config->vox != s_config.vox) changed |= kDirtyVox;
    s_config = *config;
    s_loaded = true;
    s_dirty |= changed;
    if (persist) saveConfig();
    return true;
}

bool RADIO_CONFIG_PendingApply(void)
{
    RADIO_CONFIG_Init();
    return s_dirty != 0u;
}

bool RADIO_CONFIG_ApplyToModule(void)
{
#if NRL_BOARD == NRL_BOARD_BH4TDV_RF
    const RadioModuleConfig *config = RADIO_CONFIG_Get();

    // The YAESU/MOTO select is a plain DC level; follow the config even while
    // the module is powered down or transmitting.
    (void)BH4TDV_RF_IO_SetYaesuMoto(config->radio_type == 1u);
    // H/L power select must agree with the DMOGRP Flag1 power bit; it is a
    // plain pin level, so apply it unconditionally like the radio-type line.
    (void)BH4TDV_RF_IO_SetRadioLowPower(config->low_power);

    // The module rejects parameter commands while transmitting; never cut an
    // ongoing transmission just to push config. Callers report the deferred
    // apply and the stored values take effect on the next boot/save.
    if (BH4TDV_RF_IO_IsTransmitting()) {
        ESP_LOGW(TAG, "apply deferred: radio is transmitting");
        return false;
    }
    (void)BH4TDV_RF_IO_SetRadioPtt(false);

    if (!config->enabled) {
        const bool ok = BH4TDV_RF_IO_SetRadioPower(false);
        if (ok) {
            s_module_powered = false;
            SR110U_NotifyPowerCycled();
            ESP_LOGI(TAG, "module powered down");
        }
        return ok;
    }

    if (!BH4TDV_RF_IO_SetRadioPower(true)) return false;
    if (!s_module_powered) {
        // Fresh power-up: the previous UART handshake is gone with it.
        SR110U_NotifyPowerCycled();
        s_module_powered = true;
    }
    if (!SR110U_IsReady() && !SR110U_Init()) {
        ESP_LOGE(TAG, "module handshake failed; config not applied");
        return false;
    }

    // The H/L pin (P1.0) and the DMOGRP Flag1 power bit are both applied;
    // modules that reject Flag1 still get the power level via the H/L wire.
    bool ok = true;
    if (s_dirty & kDirtyGroup) {
        const bool r = sendGroup(config);
        if (r) s_dirty &= ~kDirtyGroup;
        ok &= r;
    }
    if (s_dirty & kDirtyFunc) {
        const bool r = sendSimple("DMOFUN", config->squelch, config->mic_level,
                                  config->tot, config->scramble,
                                  config->compander ? 1u : 0u, true);
        if (r) s_dirty &= ~kDirtyFunc;
        ok &= r;
    }
    if (s_dirty & kDirtyVol) {
        const bool r = sendSimple("DMOVOL", config->volume, 0u, 0u, 0u, 0u, false);
        if (r) s_dirty &= ~kDirtyVol;
        ok &= r;
    }
    if (s_dirty & kDirtySav) {
        // Datasheet V303: DMOSAV 0 = power saving ON (default),
        // 1 = power saving OFF -- note the inverted sense.
        const bool r = sendSimple("DMOSAV", config->power_save ? 0u : 1u,
                                  0u, 0u, 0u, 0u, false);
        if (r) s_dirty &= ~kDirtySav;
        ok &= r;
    }
    if (s_dirty & kDirtyVox) {
        const bool r = sendSimple("DMOVOX", config->vox, 0u, 0u, 0u, 0u, false);
        if (r) s_dirty &= ~kDirtyVox;
        ok &= r;
    }
    if (!ok) ESP_LOGE(TAG, "module rejected (part of) the configuration");
    return ok;
#else
    return true;
#endif
}

bool RADIO_CONFIG_ParseFreqMHz(const char *text, uint32_t *out_hz)
{
    if (text == nullptr || out_hz == nullptr || text[0] == '\0') return false;
    char *end = nullptr;
    const double mhz = strtod(text, &end);
    if (end == text || *end != '\0' || mhz <= 0.0) return false;
    const uint32_t hz = static_cast<uint32_t>(mhz * 1000000.0 + 0.5);
    if (!freqInBand(hz) || hz % kFreqStepHz != 0u) {
        return false;
    }
    *out_hz = hz;
    return true;
}

void RADIO_CONFIG_FormatFreqMHz(const uint32_t hz, char *out, const size_t out_size)
{
    if (out == nullptr || out_size == 0u) return;
    snprintf(out, out_size, "%lu.%05lu",
             static_cast<unsigned long>(hz / 1000000u),
             static_cast<unsigned long>((hz % 1000000u) / 10u));
}

bool RADIO_CONFIG_ParseTone(const char *text, RadioTone *out_tone)
{
    if (text == nullptr || out_tone == nullptr || text[0] == '\0') return false;
    if (strcasecmp(text, "OFF") == 0 || strcmp(text, "0") == 0) {
        *out_tone = {RADIO_TONE_NONE, 0u};
        return true;
    }
    const size_t len = strlen(text);
    if ((text[0] == 'D' || text[0] == 'd') && len >= 4u && len <= 5u) {
        const char last = text[len - 1u];
        if (last != 'N' && last != 'n' && last != 'I' && last != 'i') return false;
        char digits[4] = {};
        if (len - 2u > sizeof(digits) - 1u) return false;
        for (size_t i = 1u; i < len - 1u; ++i) {
            if (!isdigit(static_cast<unsigned char>(text[i]))) return false;
            digits[i - 1u] = text[i];
        }
        RadioTone tone = {
            static_cast<uint8_t>((last == 'N' || last == 'n')
                                     ? RADIO_TONE_CDCSS_N
                                     : RADIO_TONE_CDCSS_I),
            static_cast<uint16_t>(strtoul(digits, nullptr, 10)),
        };
        if (!toneValid(tone)) return false;
        *out_tone = tone;
        return true;
    }
    char *end = nullptr;
    const double hz = strtod(text, &end);
    if (end == text || *end != '\0' || hz <= 0.0) return false;
    RadioTone tone = {RADIO_TONE_CTCSS,
                      static_cast<uint16_t>(hz * 10.0 + 0.5)};
    if (!toneValid(tone)) return false;
    *out_tone = tone;
    return true;
}

void RADIO_CONFIG_FormatTone(const RadioTone *tone, char *out, const size_t out_size)
{
    if (tone == nullptr || out == nullptr || out_size == 0u) return;
    switch (tone->type) {
        case RADIO_TONE_CTCSS:
            snprintf(out, out_size, "%u.%u",
                     static_cast<unsigned>(tone->value / 10u),
                     static_cast<unsigned>(tone->value % 10u));
            break;
        case RADIO_TONE_CDCSS_N:
            snprintf(out, out_size, "D%03uN", static_cast<unsigned>(tone->value));
            break;
        case RADIO_TONE_CDCSS_I:
            snprintf(out, out_size, "D%03uI", static_cast<unsigned>(tone->value));
            break;
        case RADIO_TONE_NONE:
        default:
            snprintf(out, out_size, "OFF");
            break;
    }
}
