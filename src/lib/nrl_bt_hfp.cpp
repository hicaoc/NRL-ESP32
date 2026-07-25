#include "nrl_bt_hfp.h"

#include "sdkconfig.h"

#if defined(CONFIG_BT_HFP_AG_ENABLE)

#include "ble_config.h"
#include "nrl_audio_bridge.h"
#include "nrl_psram.h"
#include "../app/driver/external_radio.h"
#include "../app/driver/status_io.h"

#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_bt_device.h>
#include <esp_gap_bt_api.h>
#include <esp_hf_ag_api.h>
#include <esp_hf_defs.h>
#include <esp_heap_caps.h>
#if defined(CONFIG_BT_A2DP_ENABLE)
#include <esp_a2dp_api.h>
#endif
#include <esp_coexist.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <nvs.h>

#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/ringbuf.h>
#include <freertos/task.h>

#include <string.h>
#include <cstddef>

#if defined(CONFIG_BT_A2DP_ENABLE)
// --- Direct Bluedroid SBC encoder ------------------------------------------
// The esp_sbc_enc wrapper (esp_audio_codec) is unusable for A2DP source on this
// build: esp_sbc_enc_open ignores cfg.bitpool (sizes its internal packet buffer
// as if bitpool==0) and leaves pu8NextPacket NULL, so encoding store-faults. We
// instead drive Bluedroid's own SBC_Encoder (already linked -- HFP-AG mSBC uses
// it) with an app-owned packet buffer we place in INTERNAL RAM. That matters:
// the encoder emits the packet with byte-wise stores, and byte stores to a PSRAM
// buffer fault while the BT radio is active (word-aligned memcpy to PSRAM is
// fine, which is how we hand the frame to the A2DP buffer below).
//
// Layout copied verbatim from components/bt/.../external/sbc/encoder/include/
// sbc_encoder.h with that component's default macros (SBC_MAX_NUM_FRAME=1,
// SBC_NO_PCM_CPY_OPTION=FALSE, SBC_JOINT_STE_INCLUDED=TRUE). The static_asserts
// pin the two field offsets the crash disassembly confirmed (pu8NextPacket @1676,
// FrameHeader @1680); a layout drift fails the build instead of crashing.
extern "C" {
struct NrlSbcEncParams {
    int16_t  s16SamplingFreq;
    int16_t  s16ChannelMode;
    int16_t  s16NumOfSubBands;
    int16_t  s16NumOfChannels;
    int16_t  s16NumOfBlocks;
    int16_t  s16AllocationMethod;
    int16_t  s16BitPool;
    uint16_t u16BitRate;
    uint8_t  sbc_mode;
    uint8_t  u8NumPacketToEncode;
    int16_t  as16Join[8];        // SBC_MAX_NUM_OF_SUBBANDS
    int16_t  s16MaxBitNeed;
    int16_t  as16ScaleFactor[16]; // channels(2) * subbands(8)
    int16_t *ps16NextPcmBuffer;
    int16_t  as16PcmBuffer[256];  // 1 frame * 16 blocks * 2 ch * 8 subbands
    int16_t  s16ScratchMemForBitAlloc[16];
    int32_t  s32SbBuffer[256];    // 2 ch * 8 subbands * 16 blocks
    int16_t  as16Bits[16];        // channels(2) * subbands(8)
    uint8_t *pu8Packet;
    uint8_t *pu8NextPacket;
    uint16_t FrameHeader;
    uint16_t u16PacketLength;
};
extern void SBC_Encoder(NrlSbcEncParams *strEncParams);
extern void SBC_Encoder_Init(NrlSbcEncParams *strEncParams);
}
static_assert(offsetof(NrlSbcEncParams, pu8NextPacket) == 1676,
              "NrlSbcEncParams layout must match Bluedroid SBC_ENC_PARAMS");
static_assert(offsetof(NrlSbcEncParams, FrameHeader) == 1680,
              "NrlSbcEncParams layout must match Bluedroid SBC_ENC_PARAMS");
// SBC #defines we need (from sbc_encoder.h).
static constexpr int16_t kSbcSf44100     = 2;
static constexpr int16_t kSbcJointStereo = 3;
static constexpr int16_t kSbcLoudness    = 0;
static constexpr uint8_t kSbcModeStd     = 0;
#endif // CONFIG_BT_A2DP_ENABLE

namespace {

const char *TAG = "BTHFP";

constexpr uint32_t kAudioRetryIntervalMs = 2000;
// Minimum LARGEST-CONTIGUOUS internal block required before bringing the Classic
// BT stack up. Enabling BT allocates the controller's internal-only pools
// (EMS/OLM/AFH), and at least one of those is a single large contiguous block --
// so the predictor of success is the largest free block, NOT total free. If it
// is too small the controller pool malloc fails and the controller ABORTS
// internally ("orca_ems_env_init failed, err:257" -> load fault), which no
// return code can catch. Observed: the stack comes up cleanly when the largest
// internal block is ~32 KB+, and crashes at ~17 KB even with 54 KB total free
// (a fragmented heap). So Poll gates on the largest block and defers (retrying
// each Poll) until the heap is contiguous enough; steady state reaches this once
// the boot allocation churn settles.
constexpr size_t kMinInternalBlockForBtUp = 30u * 1024u;
// If the largest internal block never reaches the threshold within this long
// after an enable request, give up and leave BT off (instead of spinning the UI
// switch on "switching..." forever). On a heavily loaded build the app can hold
// internal RAM so fragmented that the BT controller can never get its contiguous
// pool; failing cleanly is better UX than an endless "connecting".
constexpr uint32_t kBtEnableGiveUpMs = 6000u;
// How often to retry reconnecting to the chosen headset after a drop.
constexpr uint32_t kReconnectIntervalMs = 4000;
// Abandon a stuck in-flight SLC attempt after this long (then a retry is allowed).
constexpr uint32_t kConnectTimeoutMs = 12000;
// Inquiry length in 1.28 s units (~10 s) and unlimited responses.
constexpr uint8_t kInquiryLen = 8;

// Discovered-device table for the on-screen pick list.
constexpr size_t kMaxDevices = 12;
// HFP speaker gain is a 0..15 scale. Each headset remembers its own last gain
// (spk_volume), persisted with the saved list, so reconnecting restores it.
constexpr uint8_t kSpkVolumeMax = 15;
constexpr uint8_t kDefaultSpkVolume = 9;  // ~60 %, adjustable both ways
struct BtDevice {
    esp_bd_addr_t bda;
    char name[32];
    uint8_t spk_volume;  // saved HFP speaker gain (0..15) for this headset
};
BtDevice s_devices[kMaxDevices];
size_t s_device_count = 0;

// Saved (previously paired) headsets, persisted to NVS so they auto-reconnect
// after a reboot and can be deleted by the user. Most-recent first.
constexpr size_t kMaxSaved = 4;
constexpr const char *kNvsNamespace = "nrlbt";
constexpr const char *kNvsKeySaved = "saved";
BtDevice s_saved[kMaxSaved];
size_t s_saved_count = 0;

// Defined below stackUp(); used from the GAP auth-complete callback above it.
void addSaved(const esp_bd_addr_t bda, const char *name);
// Per-headset volume helpers, also used from the HFP callback above their defs.
int findSavedIndex(const esp_bd_addr_t bda);
void savePeerVolume();
void applyPeerVolume();

// Pre-reserved contiguous internal-RAM block for the BT controller. The
// controller's internal-only pools (EMS/OLM/AFH) need a large *contiguous*
// block, but once the app is fully loaded (AEC's ~50 KB AFE alloc, SMB mount,
// media/曲库) the internal heap is shattered to ~1.5 KB largest free block --
// so a steady-state BT enable can never get its pool and the controller aborts.
// We grab this block in NRL_BtHfp_Init(), while the heap is still whole, and
// free it in stackUp() immediately before esp_bt_controller_init() so the
// controller allocates into the freshly-freed contiguous region.
void *s_bt_reserve = nullptr;
constexpr size_t kBtReserveBytes = 40u * 1024u;

bool s_enabled = false;        // actual on/off of the stack (applied by the task)
bool s_stack_up = false;       // Bluedroid + controller actually running

// Bringing the stack up/down is slow (controller + Bluedroid init, and a teardown
// that waits up to ~1 s for a clean headset disconnect). Doing that inside the UI
// touch callback froze the screen, so the switch never repainted -- it felt
// unresponsive and users tapped repeatedly. Instead SetEnabled just records the
// desired state here; the next NRL_BtHfp_Poll() (main loop) applies it, so the
// touch callback returns immediately and LVGL can repaint. Rapid toggles coalesce
// to the latest request; the one-time stack transition still runs on the main
// loop but only once per toggle (not every frame).
volatile bool s_request_pending = false;   // a SetEnabled request is queued
volatile bool s_request_enabled = false;   // its desired state
volatile bool s_transitioning = false;     // Poll is mid stack up/down
bool s_ble_mem_released = false;
bool s_connected = false;      // service-level connection to a headset
bool s_audio_active = false;   // SCO/voice link up
esp_bd_addr_t s_peer_bda = {};
char s_peer_name[32] = {};

// Connected headset's speaker gain (0..15). Loaded from the saved entry on
// connect, pushed to the headset, and updated live when the headset's own volume
// buttons report a change (+VGS / ESP_HF_VOLUME_CONTROL_EVT). s_peer_volume_dirty
// defers the NVS write to Poll (out of the BT callback context). During the
// restore window after connect, incoming +VGS reports are ignored so the
// headset's auto-reported volume can't clobber the value we are restoring.
uint8_t s_peer_volume = kDefaultSpkVolume;
volatile bool s_peer_volume_dirty = false;
uint32_t s_volume_restore_until_ms = 0;

// Downlink jitter buffer: a plain lock-free single-producer/single-consumer ring
// of int16 PCM samples. (A FreeRTOS byte-ring mangled the audio into static on
// this path.) PushPlayback (network task) writes; the SCO outgoing callback
// reads one frame per request, padding silence on underrun.
constexpr size_t kPcmCap = 8192;       // ~1 s @ 8 kHz (headroom for the prime
                                       // depth plus a bursty MAX_MODEM RX gap)
NRL_PSRAM_BSS int16_t s_pcm[kPcmCap];
volatile size_t s_pcm_head = 0;        // write index (PushPlayback)
volatile size_t s_pcm_tail = 0;        // read index (SCO callback)
// SCO TX (HFP-AG-over-HCI) only sends when the app calls
// esp_hf_ag_outgoing_data_ready(). We drive that from the SCO RX callback
// (scoIncomingCb), which the controller calls once per eSCO interval, so our TX
// rate stays locked to the link rate. CVSD = 7.5 ms / 120-byte (60-sample) frame.
// Downlink jitter buffer: wait until this many samples are queued before playing
// (~200 ms @ 8 kHz), so bursty/jittery network packets -- WiFi runs MAX_MODEM
// power-save while BT is up, which delivers UDP voice in bursts -- don't underrun
// the SCO stream. Trades latency for not having to insert silence. The buffer also
// re-primes whenever it drains fully (see scoOutgoingCb), so each transmission
// after a pause rebuilds this cushion instead of resuming with none.
bool s_pb_priming = true;
constexpr size_t kPrimeSamples = 1600u;
// Uplink gain: the headset mic (CVSD) comes in quiet; multiply before sending.
constexpr int32_t kUplinkGain = 4;

// Headset PTT button (HFP voice-recognition button) -> applied from Poll so the
// STATUS_IO update never runs inside the BT host callback context.
volatile bool s_headset_ptt = false;
volatile bool s_headset_ptt_dirty = false;

bool s_discovering = false;
bool s_have_peer = false;        // user has selected a headset to (re)connect to
bool s_connecting = false;       // an SLC attempt is in flight (don't start another)
uint32_t s_connect_started_ms = 0;
uint32_t s_last_audio_retry_ms = 0;
uint32_t s_last_reconnect_ms = 0;
// Auto-reconnect backoff: when the chosen headset is absent/unreachable, each
// SLC attempt biases the shared radio to BT (coexPreferBt) and page-scans, which
// starves Wi-Fi TCP (SMB music times out) if it repeats every 4 s forever. So
// grow the retry interval on each failure (4 s -> 8 s -> ... capped), and reset
// it once actually connected.
uint32_t s_reconnect_interval_ms = kReconnectIntervalMs;
constexpr uint32_t kReconnectBackoffMaxMs = 30000;

// On-demand SCO: the voice (SCO) link is only opened while there is actual voice
// to carry -- network downlink arriving or the local PTT held. When neither has
// happened for kVoiceHangoverMs, the SCO is dropped so the ACL can enter sniff
// (power save) while the SLC connection stays up. s_last_voice_ms is refreshed by
// PushPlayback (downlink) and by PTT in Poll.
uint32_t s_last_voice_ms = 0;
bool s_call_active = false;       // our "voice burst in progress" notion
// Keep the SCO up for 10 s after the last voice so a whole conversation (gaps
// under this) never re-incurs the ~0.3-0.5 s SCO setup / first-syllable loss;
// only a >10 s silence drops it back to the SLC-only (sniff) power-saving state.
constexpr uint32_t kVoiceHangoverMs = 10000;

inline uint32_t nowMs()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

// Bias the shared radio toward Bluetooth while a connection/SLC handshake is in
// progress (heavy Wi-Fi voice traffic otherwise starves the RFCOMM AT exchange
// and the SLC times out). Back to balanced once the link is up.
void coexPreferBt(bool prefer)
{
    esp_coex_preference_set(prefer ? ESP_COEX_PREFER_BT : ESP_COEX_PREFER_BALANCE);
}

// ---- SCO data path (PCM over HCI) ------------------------------------------

// Headset microphone -> network uplink. PCM16 mono 8 kHz. sendVoiceFrame (via
// the bridge) is already PTT-gated and handles G.711 packetisation + UDP send.
void scoIncomingCb(const uint8_t *buf, uint32_t len)
{
    if (buf == nullptr || len < 2u) {
        return;
    }
    const int16_t *mic = reinterpret_cast<const int16_t *>(buf);
    size_t mic_n = len / 2u;
    int16_t gained[256];
    if (mic_n > 256u) {
        mic_n = 256u;  // SCO frames are ~60 samples; cap for safety
    }
    for (size_t i = 0; i < mic_n; ++i) {
        // Boost the quiet headset mic before uplink (with clipping).
        int32_t v = static_cast<int32_t>(mic[i]) * kUplinkGain;
        if (v > 32767) {
            v = 32767;
        } else if (v < -32768) {
            v = -32768;
        }
        gained[i] = static_cast<int16_t>(v);
    }
    NRLAudioBridge_FeedExternalMic(gained, mic_n);

    // Clock SCO TX off SCO RX: the controller delivers one RX frame per eSCO
    // interval, so requesting one outgoing frame here keeps our TX rate locked to
    // the link rate -- no free-running-timer drift. Rate-limit to ~one frame per
    // eSCO interval so a burst of RX frames (e.g. coming out of sniff) can't queue
    // several TX frames at once and overflow the controller's SCO xmit queue.
    if (s_audio_active) {
        static int64_t last_tx_signal_us = 0;
        const int64_t now_us = esp_timer_get_time();
        if (now_us - last_tx_signal_us >= 6500) {  // eSCO interval is ~7.5 ms
            last_tx_signal_us = now_us;
            esp_hf_ag_outgoing_data_ready();
        }
    }
}

// Network downlink -> headset speaker. Fill `len` bytes from the playback ring,
// pad the remainder with silence, and always return `len` so the SCO frame is
// well-formed.
uint32_t scoOutgoingCb(uint8_t *buf, uint32_t len)
{
    if (buf == nullptr || len == 0u) {
        return 0u;
    }
    int16_t *out = reinterpret_cast<int16_t *>(buf);
    const size_t want = len / 2u;
    const size_t head = s_pcm_head;          // snapshot (producer side)
    size_t tail = s_pcm_tail;
    const size_t avail = (head + kPcmCap - tail) % kPcmCap;
    // Jitter buffer: wait until enough is queued before starting playback.
    if (s_pb_priming) {
        if (avail < kPrimeSamples) {
            memset(buf, 0, len);
            return len;
        }
        s_pb_priming = false;
    }
    size_t got = 0u;
    while (got < want && tail != head) {
        out[got++] = s_pcm[tail];
        tail = (tail + 1u) % kPcmCap;
    }
    // Drift compensation (adaptive playout): the network source (8 kHz) and the
    // SCO sink run on independent clocks, so a sub-percent mismatch slowly drains
    // or fills the buffer and eventually warbles/overflows even with no packet
    // loss. When the backlog has drifted deep, drop one sample this frame; when it
    // has drifted shallow (but isn't empty), give one back (repeat). One 0.125 ms
    // sample is inaudible, and ~1 sample/frame corrects up to ~1.6% drift, keeping
    // the fill near kPrimeSamples without periodic re-prime glitches.
    if (got == want) {
        const size_t remaining = (head + kPcmCap - tail) % kPcmCap;
        if (remaining > 2u * kPrimeSamples) {
            tail = (tail + 1u) % kPcmCap;             // too deep: skip a sample
        } else if (remaining > 0u && remaining < kPrimeSamples / 4u) {
            tail = (tail + kPcmCap - 1u) % kPcmCap;   // too shallow: repeat a sample
        }
    }
    s_pcm_tail = tail;
    // Fully drained: no samples at all were available -- a real gap (between
    // transmissions, or a burst long enough to empty the buffer). Output silence
    // and re-prime so the next burst rebuilds the jitter cushion before playing,
    // instead of resuming with zero headroom and warbling on the first jittery
    // packet. (A partial underrun, got > 0, just holds the last sample -- silence
    // gaps on *every* underrun were the original "spring" warble.)
    if (got == 0u) {
        memset(buf, 0, len);
        s_pb_priming = true;
        return len;
    }
    if (got < want) {
        const int16_t last = out[got - 1];
        for (; got < want; ++got) {
            out[got] = last;
        }
    }
    return len;
}

// ---- HFP AG + GAP callbacks -------------------------------------------------

void hfAgCb(esp_hf_cb_event_t event, esp_hf_cb_param_t *param)
{
    if (param == nullptr) {
        return;
    }
    switch (event) {
    case ESP_HF_PROF_STATE_EVT:
        // AG profile init finished -> register the SCO (voice) data callbacks.
        esp_hf_ag_register_data_callback(scoIncomingCb, scoOutgoingCb);
        ESP_LOGI(TAG, "HFP AG profile ready");
        break;
    case ESP_HF_CONNECTION_STATE_EVT:
        if (param->conn_stat.state == ESP_HF_CONNECTION_STATE_SLC_CONNECTED) {
            memcpy(s_peer_bda, param->conn_stat.remote_bda, sizeof(s_peer_bda));
            s_connected = true;
            s_connecting = false;
            s_reconnect_interval_ms = kReconnectIntervalMs;  // reset backoff
            // Restore this headset's saved speaker volume and push it now. Ignore
            // any +VGS the headset auto-reports for the next couple of seconds so
            // it can't overwrite the value we are restoring.
            {
                const int idx = findSavedIndex(s_peer_bda);
                s_peer_volume = (idx >= 0) ? s_saved[idx].spk_volume : kDefaultSpkVolume;
                if (s_peer_volume > kSpkVolumeMax) {
                    s_peer_volume = kSpkVolumeMax;
                }
            }
            s_volume_restore_until_ms = nowMs() + 2000u;
            applyPeerVolume();
            // Linked but idle: let the radio share fairly and let the link sniff
            // for power save. The SCO voice link is opened on demand (see Poll),
            // and only then do we bias the radio toward BT.
            coexPreferBt(false);
            s_call_active = false;
            ESP_LOGI(TAG, "headset SLC connected (idle; SCO opens on demand)");
        } else if (param->conn_stat.state == ESP_HF_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(TAG, "headset disconnected");
            s_connected = false;
            s_connecting = false;
            s_audio_active = false;
            s_call_active = false;
        }
        // CONNECTED (RFCOMM up, SLC handshake in progress) keeps s_connecting set
        // so Poll won't fire a second slc_connect and collide.
        break;
    case ESP_HF_AUDIO_STATE_EVT:
        if (param->audio_stat.state == ESP_HF_AUDIO_STATE_CONNECTED ||
            param->audio_stat.state == ESP_HF_AUDIO_STATE_CONNECTED_MSBC) {
            s_audio_active = true;
            // Drop whatever buffered downlink piled up during SCO setup so
            // playback starts fresh (low latency) instead of carrying the setup
            // backlog as permanent delay. The jitter buffer re-primes from here.
            s_pcm_tail = s_pcm_head;
            s_pb_priming = true;
            // Re-assert this headset's saved speaker volume -- a headset can reset
            // its gain (sometimes to 0) when the "call" SCO opens, which would make
            // downlink network voice inaudible.
            esp_hf_ag_volume_control(param->audio_stat.remote_addr,
                                     ESP_HF_VOLUME_CONTROL_TARGET_SPK, s_peer_volume);
            ESP_LOGI(TAG, "SCO audio up (%s) -- voice routes through the headset",
                     param->audio_stat.state == ESP_HF_AUDIO_STATE_CONNECTED_MSBC ? "mSBC" : "CVSD");
        } else if (param->audio_stat.state == ESP_HF_AUDIO_STATE_DISCONNECTED) {
            s_audio_active = false;
            ESP_LOGI(TAG, "SCO audio down");
        }
        break;
    case ESP_HF_BVRA_RESPONSE_EVT:
        // Headset multifunction / voice-dial button. Map its enabled/disabled
        // toggle straight onto soft-PTT (applied from Poll).
        s_headset_ptt = (param->vra_rep.value == ESP_HF_VR_STATE_ENABLED);
        s_headset_ptt_dirty = true;
        ESP_LOGI(TAG, "headset button -> PTT %s", s_headset_ptt ? "ON" : "OFF");
        break;
    case ESP_HF_VOLUME_CONTROL_EVT:
        // The headset's own volume buttons (+VGS speaker / +VGM mic). Track the
        // speaker gain so the on-screen readout follows it and it is saved for
        // this headset. Skip reports during the post-connect restore window so the
        // headset's auto-report can't clobber the value we just restored.
        if (param->volume_control.type == ESP_HF_VOLUME_TYPE_SPK &&
            nowMs() >= s_volume_restore_until_ms) {
            int v = param->volume_control.volume;
            if (v < 0) {
                v = 0;
            } else if (v > kSpkVolumeMax) {
                v = kSpkVolumeMax;
            }
            s_peer_volume = static_cast<uint8_t>(v);
            s_peer_volume_dirty = true;  // persist from Poll (not in this context)
            ESP_LOGI(TAG, "headset volume -> %u/15", (unsigned)s_peer_volume);
        }
        break;
    // The following replies are required to complete the HFP service-level
    // connection (SLC) handshake -- without them Bluedroid waits on the AT
    // exchange and the link times out after ~5 s. We're a voice bridge with no
    // cellular state, so report "idle, in service".
    case ESP_HF_CIND_RESPONSE_EVT:
        esp_hf_ag_cind_response(param->cind_rep.remote_addr,
                                static_cast<esp_hf_call_status_t>(1),         // call active (keep SCO)
                                static_cast<esp_hf_call_setup_status_t>(0),   // no call setup
                                static_cast<esp_hf_network_state_t>(1),       // service available
                                4,                                           // signal strength
                                static_cast<esp_hf_roaming_status_t>(0),      // not roaming
                                4,                                           // battery level
                                static_cast<esp_hf_call_held_status_t>(0));   // no held call
        break;
    case ESP_HF_COPS_RESPONSE_EVT:
        esp_hf_ag_cops_response(param->cops_rep.remote_addr, const_cast<char *>("NRL"));
        break;
    case ESP_HF_UNAT_RESPONSE_EVT:
        // Acknowledge any unsupported AT command so the handshake can proceed.
        ESP_LOGI(TAG, "headset AT cmd (unknown)");
        esp_hf_ag_unknown_at_send(param->unat_rep.remote_addr, nullptr);
        break;
    case ESP_HF_ATA_RESPONSE_EVT:
        ESP_LOGI(TAG, "headset button: ANSWER (ATA)");
        break;
    case ESP_HF_CHUP_RESPONSE_EVT:
        ESP_LOGI(TAG, "headset button: HANGUP (CHUP)");
        break;
    case ESP_HF_VTS_RESPONSE_EVT:
        ESP_LOGI(TAG, "headset button: DTMF key");
        break;
    default:
        // Surface any other headset action (button presses show up here) to help
        // map which HFP event a given headset sends.
        ESP_LOGI(TAG, "HFP evt %d", (int)event);
        break;
    }
}

void gapCb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    if (param == nullptr) {
        return;
    }
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
        // Collect each discovered device into the pick list (the user chooses one
        // on screen). Parse Class-of-Device to keep audio devices (headsets /
        // speakers) and pull a display name from the BDNAME prop or EIR.
        uint32_t cod = 0;
        const char *name = nullptr;
        int name_len = 0;
        uint8_t *eir = nullptr;
        for (int i = 0; i < param->disc_res.num_prop; ++i) {
            esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];
            if (p->val == nullptr) {
                continue;
            }
            if (p->type == ESP_BT_GAP_DEV_PROP_COD) {
                cod = *reinterpret_cast<uint32_t *>(p->val);
            } else if (p->type == ESP_BT_GAP_DEV_PROP_BDNAME) {
                name = reinterpret_cast<const char *>(p->val);
                name_len = p->len;
            } else if (p->type == ESP_BT_GAP_DEV_PROP_EIR) {
                eir = reinterpret_cast<uint8_t *>(p->val);
            }
        }
        // Only list audio devices (headsets/speakers) to keep the list relevant.
        const bool is_audio = (esp_bt_gap_get_cod_major_dev(cod) == ESP_BT_COD_MAJOR_DEV_AV) ||
                              (esp_bt_gap_get_cod_srvc(cod) & ESP_BT_COD_SRVC_AUDIO);
        if (!is_audio) {
            break;
        }
        // De-dup by address.
        bool exists = false;
        for (size_t i = 0; i < s_device_count; ++i) {
            if (memcmp(s_devices[i].bda, param->disc_res.bda, sizeof(esp_bd_addr_t)) == 0) {
                exists = true;
                break;
            }
        }
        if (exists || s_device_count >= kMaxDevices) {
            break;
        }
        BtDevice &dev = s_devices[s_device_count];
        memcpy(dev.bda, param->disc_res.bda, sizeof(esp_bd_addr_t));
        // Resolve a name: BDNAME prop -> EIR complete/short name -> address.
        if ((name == nullptr || name_len == 0) && eir != nullptr) {
            uint8_t rlen = 0;
            uint8_t *rname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &rlen);
            if (rname == nullptr || rlen == 0) {
                rname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &rlen);
            }
            name = reinterpret_cast<const char *>(rname);
            name_len = rlen;
        }
        if (name != nullptr && name_len > 0) {
            const size_t n = (static_cast<size_t>(name_len) < sizeof(dev.name) - 1u)
                                 ? static_cast<size_t>(name_len) : sizeof(dev.name) - 1u;
            memcpy(dev.name, name, n);
            dev.name[n] = '\0';
        } else {
            const uint8_t *a = param->disc_res.bda;
            snprintf(dev.name, sizeof(dev.name), "%02X:%02X:%02X:%02X:%02X:%02X",
                     a[0], a[1], a[2], a[3], a[4], a[5]);
        }
        ++s_device_count;
        ESP_LOGI(TAG, "found device %u: %s", (unsigned)s_device_count, dev.name);
        break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        s_discovering = (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED);
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        // Legacy pairing: most headsets use PIN "0000".
        ESP_LOGI(TAG, "PIN requested -> replying 0000");
        if (param->pin_req.min_16_digit) {
            esp_bt_pin_code_t pin16 = {'0', '0', '0', '0', '0', '0', '0', '0',
                                       '0', '0', '0', '0', '0', '0', '0', '0'};
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, pin16);
        } else {
            esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin);
        }
        break;
    }
    case ESP_BT_GAP_CFM_REQ_EVT:
        // Simple-Secure-Pairing numeric confirmation -> auto-accept (no keypad).
        ESP_LOGI(TAG, "SSP confirm (val=%u) -> accept", (unsigned)param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "SSP passkey: %06u", (unsigned)param->key_notif.passkey);
        break;
    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGI(TAG, "SSP passkey requested -> replying 0");
        esp_bt_gap_ssp_passkey_reply(param->key_req.bda, true, 0);
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            snprintf(s_peer_name, sizeof(s_peer_name), "%.*s",
                     static_cast<int>(sizeof(s_peer_name) - 1u),
                     reinterpret_cast<const char *>(param->auth_cmpl.device_name));
            ESP_LOGI(TAG, "paired with '%s'", s_peer_name);
            // Remember it so it auto-reconnects after a reboot.
            addSaved(param->auth_cmpl.bda, s_peer_name);
        } else {
            ESP_LOGW(TAG, "pairing failed, status=%d", param->auth_cmpl.stat);
        }
        break;
    default:
        break;
    }
}

void loadSaved()
{
    s_saved_count = 0;
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_saved);
    if (nvs_get_blob(h, kNvsKeySaved, s_saved, &len) == ESP_OK) {
        s_saved_count = len / sizeof(BtDevice);
        if (s_saved_count > kMaxSaved) {
            s_saved_count = kMaxSaved;
        }
        // Clamp volumes from older/garbled blobs into the valid 0..15 range.
        for (size_t i = 0; i < s_saved_count; ++i) {
            if (s_saved[i].spk_volume > kSpkVolumeMax) {
                s_saved[i].spk_volume = kDefaultSpkVolume;
            }
        }
    }
    nvs_close(h);
}

void persistSaved()
{
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_blob(h, kNvsKeySaved, s_saved, s_saved_count * sizeof(BtDevice));
    nvs_commit(h);
    nvs_close(h);
}

int findSavedIndex(const esp_bd_addr_t bda)
{
    for (size_t i = 0; i < s_saved_count; ++i) {
        if (memcmp(s_saved[i].bda, bda, sizeof(esp_bd_addr_t)) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Persist the connected headset's current speaker volume back to its saved entry.
void savePeerVolume()
{
    const int idx = findSavedIndex(s_peer_bda);
    if (idx >= 0 && s_saved[idx].spk_volume != s_peer_volume) {
        s_saved[idx].spk_volume = s_peer_volume;
        persistSaved();
    }
}

// Push the connected headset's speaker gain to it over the (RFCOMM) SLC link.
void applyPeerVolume()
{
    if (s_connected) {
        esp_hf_ag_volume_control(s_peer_bda, ESP_HF_VOLUME_CONTROL_TARGET_SPK, s_peer_volume);
    }
}

// Insert/refresh a paired device at the front of the saved list (most recent).
void addSaved(const esp_bd_addr_t bda, const char *name)
{
    // Carry the saved per-headset volume across a re-pair (default for new ones).
    uint8_t saved_volume = kDefaultSpkVolume;
    // Drop any existing entry for this address.
    for (size_t i = 0; i < s_saved_count; ++i) {
        if (memcmp(s_saved[i].bda, bda, sizeof(esp_bd_addr_t)) == 0) {
            saved_volume = s_saved[i].spk_volume;
            for (size_t j = i; j + 1 < s_saved_count; ++j) {
                s_saved[j] = s_saved[j + 1];
            }
            --s_saved_count;
            break;
        }
    }
    // Shift down and insert at front (cap at kMaxSaved).
    if (s_saved_count >= kMaxSaved) {
        s_saved_count = kMaxSaved - 1;
    }
    for (size_t j = s_saved_count; j > 0; --j) {
        s_saved[j] = s_saved[j - 1];
    }
    memcpy(s_saved[0].bda, bda, sizeof(esp_bd_addr_t));
    s_saved[0].spk_volume = saved_volume;
    if (name != nullptr && name[0] != '\0') {
        snprintf(s_saved[0].name, sizeof(s_saved[0].name), "%s", name);
    } else {
        snprintf(s_saved[0].name, sizeof(s_saved[0].name),
                 "%02X:%02X:%02X:%02X:%02X:%02X", bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    }
    ++s_saved_count;
    persistSaved();
}

#if defined(CONFIG_BT_A2DP_ENABLE)
// ---- A2DP source: music streaming (IDF6 app-encoded-SBC model) -------------
// The endpoint advertises exactly one configuration -- 44.1 kHz joint stereo,
// 16 blocks, 8 subbands, loudness -- so codec negotiation is deterministic
// and the SBC encoder config never depends on the peer's answer beyond the
// bitpool ceiling. The music player writes 44.1 kHz stereo PCM into a PSRAM
// ring; a paced TX task encodes and ships MTU-sized SBC batches.

// A2DP local stream-endpoint index. The esp_a2d API requires 0 <= seid <
// ESP_A2D_MAX_SEPS (= CONFIG_BT_A2DP_SEP_NUM_MAX, default 1), so with one endpoint
// the only valid value is 0. It was 1, which esp_a2d_source_register_stream_endpoint
// rejected as INVALID_ARG (surfaced as "UNKNOWN ERROR" -- the err-name table is
// trimmed), so A2DP never registered and music never reached the headset.
constexpr uint8_t kA2dSeid = 0;
constexpr uint16_t kA2dBitpool = 53;                  // A2DP "high quality" for joint stereo
constexpr size_t kA2dPcmRingSamples = 96 * 1024;      // int16: 192 KB PSRAM ~= 1.1 s
constexpr size_t kA2dSbcFrameSamplesPerCh = 16 * 8;   // blocks * subbands = 128

bool s_a2d_connected = false;
bool s_a2dp_registered = false;  // A2DP source endpoint successfully registered
volatile bool s_a2d_media_started = false;
volatile bool s_a2d_want_stream = false;
esp_a2d_conn_hdl_t s_a2d_conn_hdl = 0;
uint16_t s_a2d_mtu = 0;
NrlSbcEncParams *s_sbc_params = nullptr;  // encoder state (INTERNAL RAM)
uint8_t *s_sbc_packet = nullptr;          // encoder output packet (INTERNAL RAM)
int s_sbc_in_bytes = 0;   // PCM bytes per SBC frame (128 samples * 2ch * 2B = 512)
int s_sbc_out_bytes = 0;  // encoded bytes per SBC frame
NRL_PSRAM_BSS int16_t s_a2d_ring_storage[kA2dPcmRingSamples];
int16_t *s_a2d_ring = s_a2d_ring_storage;
volatile size_t s_a2d_head = 0; // producer (NRL_BtA2dp_Write)
volatile size_t s_a2d_tail = 0; // consumer (TX task)
TaskHandle_t s_a2d_tx_task = nullptr;
volatile bool s_a2d_tx_run = false;
uint32_t s_a2d_timestamp = 0;

size_t a2dRingUsed()
{
    return (s_a2d_head + kA2dPcmRingSamples - s_a2d_tail) % kA2dPcmRingSamples;
}

bool a2dOpenEncoder()
{
    if (s_sbc_params != nullptr) {
        return true;
    }
    // All encoder memory in INTERNAL RAM (calloc: SBC_Encoder_Init assumes zeroed
    // state). The packet buffer is generously sized for one worst-case frame.
    s_sbc_params = static_cast<NrlSbcEncParams *>(
        heap_caps_calloc(1, sizeof(NrlSbcEncParams), MALLOC_CAP_INTERNAL));
    s_sbc_packet = static_cast<uint8_t *>(heap_caps_malloc(256u, MALLOC_CAP_INTERNAL));
    if (s_sbc_params == nullptr || s_sbc_packet == nullptr) {
        ESP_LOGE(TAG, "A2DP SBC encoder alloc failed (internal RAM)");
        if (s_sbc_params != nullptr) { heap_caps_free(s_sbc_params); s_sbc_params = nullptr; }
        if (s_sbc_packet != nullptr) { heap_caps_free(s_sbc_packet); s_sbc_packet = nullptr; }
        return false;
    }
    s_sbc_params->s16SamplingFreq    = kSbcSf44100;
    s_sbc_params->s16ChannelMode     = kSbcJointStereo;
    s_sbc_params->s16NumOfSubBands   = 8;
    s_sbc_params->s16NumOfChannels   = 2;
    s_sbc_params->s16NumOfBlocks     = 16;
    s_sbc_params->s16AllocationMethod = kSbcLoudness;
    // SBC_Encoder_Init DERIVES s16BitPool from u16BitRate (kbps) and clamps a 0
    // rate to bitpool 0 -> ~14-byte (near-silent) frames. 328 kbps (bitpool 53) is
    // "high quality" but the negotiated ACL link here carries only ~229 kbps
    // (Bluedroid's BTC_A2DP_NON_EDR_MAX_RATE) -- pushing 328 kbps overflowed the
    // controller TX queue (l2cap congestion). 220 kbps (bitpool ~33) fits under
    // that ceiling with margin and is still standard "middle" SBC quality.
    s_sbc_params->u16BitRate         = 220;
    s_sbc_params->s16BitPool         = static_cast<int16_t>(kA2dBitpool); // recomputed by Init
    s_sbc_params->sbc_mode           = kSbcModeStd;
    s_sbc_params->u8NumPacketToEncode = 1;
    s_sbc_params->pu8Packet          = s_sbc_packet;
    SBC_Encoder_Init(s_sbc_params);
    ESP_LOGI(TAG, "A2DP SBC_Encoder_Init -> bitpool=%d", (int)s_sbc_params->s16BitPool);

    s_sbc_in_bytes = static_cast<int>(kA2dSbcFrameSamplesPerCh) * 2 * 2; // 128*2ch*2B = 512
    // SBC joint-stereo frame length:
    //   4 (header) + (4*subbands*channels)/8 (scale factors)
    //             + ceil((subbands + blocks*bitpool)/8) (audio samples)
    s_sbc_out_bytes = 4 + (4 * 8 * 2) / 8 + ((8 + 16 * static_cast<int>(kA2dBitpool)) + 7) / 8;
    ESP_LOGI(TAG, "A2DP SBC up (direct SBC_Encoder): 44.1k joint-stereo bp%u, frame %dB->%dB",
             (unsigned)kA2dBitpool, s_sbc_in_bytes, s_sbc_out_bytes);
    return true;
}

void a2dTxTask(void *)
{
    // Pace by the 44.1 kHz sample clock, staying a small lead ahead so the
    // sink's jitter buffer keeps filled without us bursting the ACL.
    const size_t frame_samples = kA2dSbcFrameSamplesPerCh * 2u; // interleaved L+R
    const int64_t start_us = esp_timer_get_time();
    uint64_t sent_frames = 0; // SBC frames sent
    uint32_t dbg_pkt = 0; int dbg_len = 0; int dbg_pcm = 0;

    int16_t *pcm = static_cast<int16_t *>(heap_caps_malloc(frame_samples * sizeof(int16_t),
                                                           MALLOC_CAP_INTERNAL));
    while (s_a2d_tx_run && pcm != nullptr && s_sbc_params != nullptr) {
        // Frames per packet bounded by the negotiated MTU (RTP/AVDTP header
        // overhead is handled by the stack below the buff API).
        size_t fpp = (s_a2d_mtu > 32u) ? ((s_a2d_mtu - 32u) / static_cast<size_t>(s_sbc_out_bytes)) : 4u;
        if (fpp == 0u) {
            fpp = 1u;
        }
        if (fpp > 8u) {
            fpp = 8u;
        }

        // Underrun guard: if the ring can't fill a whole packet, wait for the
        // decoder instead of busy-encoding silence (that spun the CPU to 100%).
        if (a2dRingUsed() < fpp * frame_samples) {
            vTaskDelay(pdMS_TO_TICKS(8));
            continue;
        }

        esp_a2d_audio_buff_t *buff = esp_a2d_audio_buff_alloc(
            static_cast<uint16_t>(fpp * static_cast<size_t>(s_sbc_out_bytes)));
        if (buff == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        const int64_t enc_t0 = esp_timer_get_time();
        size_t encoded_frames = 0;
        uint16_t data_len = 0;
        for (size_t f = 0; f < fpp; ++f) {
            // Pull one SBC frame's worth of PCM; zero-fill on underrun so the
            // stream clock keeps running (brief silence beats a stall).
            size_t got = 0;
            while (got < frame_samples) {
                if (s_a2d_tail == s_a2d_head) {
                    break;
                }
                pcm[got++] = s_a2d_ring[s_a2d_tail];
                s_a2d_tail = (s_a2d_tail + 1u) % kA2dPcmRingSamples;
            }
            if (got < frame_samples) {
                memset(pcm + got, 0, (frame_samples - got) * sizeof(int16_t));
            }

            // Stop before the buffer can be overrun: only encode if a full
            // worst-case SBC frame still fits this packet.
            if (data_len + static_cast<uint16_t>(s_sbc_out_bytes) > buff->buff_size) {
                break;
            }
            // Encode one frame with Bluedroid's SBC_Encoder into our INTERNAL packet
            // buffer (byte-wise stores, safe in internal RAM), then word-aligned
            // memcpy the finished frame into the PSRAM A2DP buff. Reset the working
            // pointers each frame (SBC_Encoder advances them past the emitted frame).
            memcpy(s_sbc_params->as16PcmBuffer, pcm, frame_samples * sizeof(int16_t));
            s_sbc_params->pu8NextPacket = s_sbc_params->pu8Packet;
            s_sbc_params->ps16NextPcmBuffer = s_sbc_params->as16PcmBuffer;
            SBC_Encoder(s_sbc_params);
            const uint16_t frame_len = s_sbc_params->u16PacketLength;
            if (frame_len == 0u || data_len + frame_len > buff->buff_size) {
                break;
            }
            memcpy(buff->data + data_len, s_sbc_packet, frame_len);
            data_len = static_cast<uint16_t>(data_len + frame_len);
            ++encoded_frames;
            dbg_len = frame_len;
            dbg_pcm = static_cast<int>(pcm[0]) + static_cast<int>(pcm[1]);
        }

        if (encoded_frames == 0u) {
            esp_a2d_audio_buff_free(buff);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        const int enc_us = static_cast<int>(esp_timer_get_time() - enc_t0);
        buff->number_frame = static_cast<uint16_t>(encoded_frames);
        buff->data_len = data_len;
        buff->timestamp = s_a2d_timestamp;
        s_a2d_timestamp += static_cast<uint32_t>(encoded_frames * kA2dSbcFrameSamplesPerCh);

        // One send, a couple of quick retries on transient queue-full, then drop.
        // (Long retry loops fight the pacing below; a dropped packet is a tiny gap.)
        bool sent = false;
        for (int attempt = 0; attempt < 4 && s_a2d_tx_run; ++attempt) {
            const esp_err_t err = esp_a2d_source_audio_data_send(s_a2d_conn_hdl, buff);
            if (err == ESP_OK) {
                sent = true;
                break;
            }
            if (err != ESP_FAIL) { // not a transient queue-full -> drop
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(3));
        }
        if (!sent) {
            esp_a2d_audio_buff_free(buff);
        }
        sent_frames += encoded_frames;
        if ((++dbg_pkt % 128u) == 0u) {
            ESP_LOGI(TAG, "A2DP tx: pkts=%u frames=%llu ring=%u frameLen=%d pcm01=%d sent=%d enc8us=%d",
                     (unsigned)dbg_pkt, (unsigned long long)sent_frames,
                     (unsigned)a2dRingUsed(), dbg_len, dbg_pcm, (int)sent, enc_us);
        }

        // Pace production to real time on an ABSOLUTE schedule: delay whenever the
        // produced audio leads the wall clock at all. This keeps only ~1 packet in
        // the controller TX queue instead of bursting a prefill that overflows it
        // (queue warning at 28 frames + L2CAP congestion -> stutter/silence).
        const int64_t audio_us = static_cast<int64_t>(sent_frames) * kA2dSbcFrameSamplesPerCh * 1000000LL / 44100LL;
        const int64_t wall_us = esp_timer_get_time() - start_us;
        if (audio_us - wall_us > 5000LL) {
            vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>((audio_us - wall_us) / 1000LL)));
        }
    }

    if (pcm != nullptr) {
        heap_caps_free(pcm);
    }
    s_a2d_tx_task = nullptr;
    vTaskDelete(nullptr);
}

void a2dStopTx()
{
    s_a2d_tx_run = false;
    for (int i = 0; i < 100 && s_a2d_tx_task != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    s_a2d_media_started = false;
}

void a2dStartTxIfReady()
{
    if (!s_a2d_want_stream || s_a2d_tx_task != nullptr) {
        return;
    }
    if (!a2dOpenEncoder()) {
        return;
    }
    s_a2d_head = 0;
    s_a2d_tail = 0;
    s_a2d_timestamp = 0;
    s_a2d_tx_run = true;
    // Core 1 with the other audio work; priority under the voice passthrough.
    // Pin to CORE 0. The music decoder + resampler run on core 1; putting the
    // encoder there starved them (ring emptied, silence). Core 0 keeps the ring
    // fed. Internal 6 KB stack. (enc8us in the tx log tells us if the encode
    // itself is the throughput bottleneck.)
    if (xTaskCreatePinnedToCore(a2dTxTask, "a2dp_tx", 6144, nullptr, 7,
                                &s_a2d_tx_task, 0) != pdPASS) {
        s_a2d_tx_run = false;
        s_a2d_tx_task = nullptr;
        ESP_LOGE(TAG, "A2DP tx task create failed");
        return;
    }
    s_a2d_media_started = true;
    ESP_LOGI(TAG, "A2DP streaming started (mtu=%u)", (unsigned)s_a2d_mtu);
}

void a2dpCb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            s_a2d_connected = true;
            s_a2d_conn_hdl = param->conn_stat.conn_hdl;
            s_a2d_mtu = param->conn_stat.audio_mtu;
            ESP_LOGI(TAG, "A2DP connected (mtu=%u)", (unsigned)s_a2d_mtu);
            if (s_a2d_want_stream) {
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
            }
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(TAG, "A2DP disconnected");
            s_a2d_connected = false;
            a2dStopTx();
        }
        break;
    case ESP_A2D_AUDIO_CFG_EVT:
        // Endpoint advertises a single configuration, so the negotiated
        // codec always matches the encoder we open; just log it.
        ESP_LOGI(TAG, "A2DP codec configured (type=%d)", param->audio_cfg.mcc.type);
        break;
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
        if (param->media_ctrl_stat.status != ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
            ESP_LOGW(TAG, "A2DP media ctrl cmd=%d nack=%d",
                     param->media_ctrl_stat.cmd, param->media_ctrl_stat.status);
            break;
        }
        if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY && s_a2d_want_stream) {
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
        } else if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_START) {
            a2dStartTxIfReady();
        } else if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_SUSPEND) {
            a2dStopTx();
        }
        break;
    case ESP_A2D_AUDIO_STATE_EVT:
        if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_SUSPEND) {
            a2dStopTx();
        }
        break;
    default:
        break;
    }
}

bool a2dRegisterProfile()
{
    esp_err_t err = esp_a2d_register_callback(a2dpCb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "A2DP callback register failed: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_a2d_source_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "A2DP source init failed: %s (0x%x)", esp_err_to_name(err), (unsigned)err);
        return false;
    }
    esp_a2d_mcc_t mcc = {};
    mcc.type = ESP_A2D_MCT_SBC;
    mcc.cie.sbc_info.samp_freq = ESP_A2D_SBC_CIE_SF_44K;
    mcc.cie.sbc_info.ch_mode = ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO;
    mcc.cie.sbc_info.block_len = ESP_A2D_SBC_CIE_BLOCK_LEN_16;
    mcc.cie.sbc_info.num_subbands = ESP_A2D_SBC_CIE_NUM_SUBBANDS_8;
    mcc.cie.sbc_info.alloc_mthd = ESP_A2D_SBC_CIE_ALLOC_MTHD_LOUDNESS;
    mcc.cie.sbc_info.min_bitpool = 2;
    mcc.cie.sbc_info.max_bitpool = kA2dBitpool;
    err = esp_a2d_source_register_stream_endpoint(kA2dSeid, &mcc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "A2DP endpoint register failed: %s (0x%x)", esp_err_to_name(err), (unsigned)err);
        esp_a2d_source_deinit();
        return false;
    }
    return true;
}

void a2dTearDown()
{
    s_a2d_want_stream = false;
    a2dStopTx();
    if (s_a2d_connected) {
        esp_a2d_source_disconnect(s_peer_bda);
        for (int i = 0; i < 50 && s_a2d_connected; ++i) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    esp_a2d_source_deinit();
    if (s_sbc_params != nullptr) {
        heap_caps_free(s_sbc_params);
        s_sbc_params = nullptr;
    }
    if (s_sbc_packet != nullptr) {
        heap_caps_free(s_sbc_packet);
        s_sbc_packet = nullptr;
    }
}
#endif // CONFIG_BT_A2DP_ENABLE

bool failStackUp(bool hfp_inited, bool a2dp_inited)
{
#if defined(CONFIG_BT_A2DP_ENABLE)
    if (a2dp_inited) {
        a2dTearDown();
    }
#else
    (void)a2dp_inited;
#endif
    if (hfp_inited) {
        esp_hf_ag_deinit();
    }
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED) {
        esp_bluedroid_disable();
    }
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_INITIALIZED) {
        esp_bluedroid_deinit();
    }
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        esp_bt_controller_disable();
    }
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED) {
        esp_bt_controller_deinit();
    }
    s_connected = false;
    s_connecting = false;
    s_audio_active = false;
    s_have_peer = false;
    s_device_count = 0;
    s_peer_name[0] = '\0';
    coexPreferBt(false);
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGE(TAG, "BT HFP AG start failed; free heap=%u internal=%u largest_internal=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return false;
}

bool stackUp()
{
    if (s_stack_up) {
        return true;
    }

    ESP_LOGI(TAG, "BT start requested; free heap=%u internal=%u largest_internal=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    // One-shot diagnostic: dump every task's stack headroom so we can see which
    // stacks are over-provisioned in internal RAM (candidates to trim or move to
    // a PSRAM stack) before Classic BT competes for that RAM. min_free is the
    // smallest free the stack ever had (uxTaskGetStackHighWaterMark); a large
    // min_free means the allocated stack is much bigger than the task uses.
    {
        const UBaseType_t n = uxTaskGetNumberOfTasks();
        TaskStatus_t *st = static_cast<TaskStatus_t *>(
            heap_caps_malloc(n * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM));
        if (st != nullptr) {
            const UBaseType_t got = uxTaskGetSystemState(st, n, nullptr);
            for (UBaseType_t i = 0; i < got; ++i) {
                ESP_LOGI(TAG, "TASKMEM %-16s core=%d min_free=%u B",
                         st[i].pcTaskName,
                         (int)st[i].xCoreID,
                         (unsigned)(st[i].usStackHighWaterMark * sizeof(StackType_t)));
            }
            heap_caps_free(st);
        }
    }

    // BR/EDR only: hand the BLE controller memory back to the heap once.
    // (Skipped in BTDM dual-mode where BLE provisioning may need it.)
#if !defined(CONFIG_BTDM_CTRL_MODE_BTDM)
    if (!s_ble_mem_released) {
        esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
        s_ble_mem_released = true;
    }
#endif

    // If BLE provisioning currently owns the controller, stop it first.
    if (BLEConfig_IsControllerUp()) {
        ESP_LOGI(TAG, "BLE provisioning active; stopping before HFP stackUp");
        BLEConfig_Stop();
    }

    // Hand the pre-reserved contiguous internal block back to the heap right
    // before the controller allocates its pools, so it lands in that fresh
    // contiguous region instead of the shattered steady-state heap. No task
    // switch happens between here and esp_bt_controller_init(), so nothing else
    // can grab it first.
    if (s_bt_reserve != nullptr) {
        heap_caps_free(s_bt_reserve);
        s_bt_reserve = nullptr;
        ESP_LOGI(TAG, "released BT RAM reserve; largest_internal now=%u",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }

    // The controller-config default macro only sets a subset of this large
    // struct (the rest is intentionally zero-initialised); silence the project's
    // -Werror=missing-field-initializers just for it.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
#pragma GCC diagnostic pop
    esp_err_t err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "controller init: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_bt_controller_enable(ESP_BT_MODE_BTDM);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "controller enable: %s", esp_err_to_name(err));
        esp_bt_controller_deinit();
        return false;
    }
    err = esp_bluedroid_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid init: %s; internal=%u largest_internal=%u",
                 esp_err_to_name(err),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        return false;
    }
    err = esp_bluedroid_enable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid enable: %s", esp_err_to_name(err));
        esp_bluedroid_deinit();
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        return false;
    }

    // Device name: NRL-<callsign> so it's recognisable in the headset's pair list.
    char name[32];
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    if (cfg != nullptr && cfg->callsign[0] != '\0') {
        snprintf(name, sizeof(name), "NRL-%s", cfg->callsign);
    } else {
        snprintf(name, sizeof(name), "NRL-AUDIO");
    }
    err = esp_bt_gap_set_device_name(name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BT device name failed: %s", esp_err_to_name(err));
        return failStackUp(false, false);
    }
    err = esp_bt_gap_register_callback(gapCb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GAP callback register failed: %s", esp_err_to_name(err));
        return failStackUp(false, false);
    }

    err = esp_hf_ag_register_callback(hfAgCb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HFP AG callback register failed: %s", esp_err_to_name(err));
        return failStackUp(false, false);
    }
    err = esp_hf_ag_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HFP AG init failed: %s", esp_err_to_name(err));
        return failStackUp(false, false);
    }
    // The SCO data-path callbacks are registered once the AG profile reports it
    // is up (ESP_HF_PROF_STATE_EVT), matching the IDF hfp_ag example.

#if defined(CONFIG_BT_A2DP_ENABLE)
    // A2DP source rides the same ACL as HFP; music streaming starts on
    // demand (NRL_BtA2dp_RequestStart) once a headset is connected. A2DP
    // registration is the most memory-hungry step and this chip's internal RAM
    // is tight, so treat a failure as non-fatal: degrade to HFP-only (the core
    // headset voice bridge) rather than tearing the whole stack back down.
    // Tearing down here while the just-initialised BTC/HFP state machine is
    // still dispatching async events raced into a use-after-free (load-access
    // fault); a2dRegisterProfile already cleanly deinits its own partial state
    // on failure, so leaving HFP up is safe. NRL_BtA2dp_* stay inert because
    // s_a2d_connected never becomes true.
    // A2DP is the memory/radio-hungry profile and cannot coexist with Wi-Fi on the
    // single radio, so only register it when the Wi-Fi master switch is OFF (radio
    // + RAM freed). A clean, single registration attempt here avoids the dirty
    // re-init that repeated in-flight retries hit. To switch into A2DP music mode
    // the user turns Wi-Fi off then toggles Bluetooth, so stackUp runs with Wi-Fi
    // already down.
    {
        const ExternalRadioConfig *rcfg = EXTERNAL_RADIO_GetConfig();
        const bool wifi_off = (rcfg != nullptr) && !rcfg->wifi_enabled;
        if (wifi_off) {
            s_a2dp_registered = a2dRegisterProfile();
            ESP_LOGI(TAG, "A2DP %s (Wi-Fi off)", s_a2dp_registered ? "registered" : "register FAILED");
        } else {
            s_a2dp_registered = false;
            ESP_LOGI(TAG, "A2DP skipped (Wi-Fi on -- HFP voice mode)");
        }
    }
#endif

    // Legacy pairing: variable PIN; we answer "0000" on a PIN request. (Match
    // the IDF example -- do NOT force an IO capability via set_security_param,
    // which broke SSP/pairing with some headsets.)
    esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
    err = esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 4, pin);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BT PIN setup failed: %s", esp_err_to_name(err));
        return failStackUp(true, true);
    }

    // Be visible + connectable so a headset can pair/reconnect.
    err = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BT scan mode failed: %s", esp_err_to_name(err));
        return failStackUp(true, true);
    }

    // Let WiFi yield the shared radio between beacons. The app otherwise runs
    // WIFI_PS_NONE (WiFi never sleeps) for low-latency voice, which starves the
    // BT controller's coex scheduler (OLC sched failed) and the BT ACL gets no
    // slots -> 16 s supervision timeout. MAX_MODEM keeps coex stable. (MIN_MODEM
    // was tried to help SMB/TCP bulk transfer under BT, but it made WiFi too
    // greedy -> "OLC C:1 sched failed" storms AND SMB still timed out, so the
    // single radio simply can't sustain an active BT link + TCP bulk at once;
    // low-bandwidth UDP radio voice coexists fine, SMB bulk does not.)
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);

    // Restore saved headsets and auto-reconnect to the most recent one.
    loadSaved();
    if (s_saved_count > 0u) {
        memcpy(s_peer_bda, s_saved[0].bda, sizeof(s_peer_bda));
        s_have_peer = true;
        s_last_reconnect_ms = 0;  // let Poll attempt promptly
        ESP_LOGI(TAG, "auto-reconnecting saved headset '%s'", s_saved[0].name);
    }

    // Reset the downlink jitter buffer (SCO TX is clocked from the RX callback).
    s_pcm_head = 0;
    s_pcm_tail = 0;
    s_pb_priming = true;

    s_stack_up = true;
    ESP_LOGI(TAG, "BT HFP AG up as '%s' (discoverable); saved=%u; free heap=%u",
             name, (unsigned)s_saved_count, (unsigned)esp_get_free_heap_size());
    return true;
}

void stackDown()
{
    if (!s_stack_up) {
        return;
    }
    if (s_discovering) {
        esp_bt_gap_cancel_discovery();
        s_discovering = false;
    }
    // Stop accepting new links, then gracefully drop any active one and WAIT for
    // it to finish before deinit. Tearing the stack down with a connection (and
    // its HCI disconnect command) still in flight crashes Bluedroid's HCI layer
    // (use-after-free in transmit_command).
    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    s_have_peer = false;  // don't let Poll re-arm a reconnect during teardown
#if defined(CONFIG_BT_A2DP_ENABLE)
    a2dTearDown();
#endif
    if (s_connected || s_connecting) {
        esp_hf_ag_slc_disconnect(s_peer_bda);
        for (int i = 0; i < 50 && (s_connected || s_connecting); ++i) {
            vTaskDelay(pdMS_TO_TICKS(20));  // up to ~1 s for the disconnect event
        }
    }
    vTaskDelay(pdMS_TO_TICKS(50));  // let any final HCI traffic drain
    esp_hf_ag_deinit();
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    // Don't re-grab the reserve: in the fully-loaded steady state the freed
    // controller pool region is no longer one contiguous 40 KB block, so the
    // re-grab fails anyway -- and holding it would starve music/other features.
    // Turning BT off is a one-way door until reboot (a later enable defers/
    // aborts via the gate), which matches the reserve model.
    coexPreferBt(false);
    esp_wifi_set_ps(WIFI_PS_NONE);  // restore the app's low-latency WiFi setting
    s_stack_up = false;
    s_connected = false;
    s_connecting = false;
    s_audio_active = false;
    s_have_peer = false;
    s_device_count = 0;
    s_peer_name[0] = '\0';
#if defined(CONFIG_BT_A2DP_ENABLE)
    s_a2dp_registered = false;
#endif
    ESP_LOGI(TAG, "BT HFP AG down; free heap=%u", (unsigned)esp_get_free_heap_size());
}

// Bring the stack to match `enabled`. Slow (see stackUp/stackDown); only ever
// called from the BT task so it never blocks the UI or main loop.
void applyEnabled(bool enabled)
{
    if (enabled == s_enabled) {
        return;
    }
    s_enabled = enabled;
    if (enabled) {
        if (!stackUp()) {
            s_enabled = false;
        }
    } else {
        stackDown();
    }
}

} // namespace

// ---- Public API -------------------------------------------------------------

void NRL_BtHfp_Init(void)
{
    // Reserve the BT controller's contiguous internal RAM NOW, while the heap is
    // still whole. This runs (main.cpp) before ES8311/AEC_Init's ~50 KB AFE
    // alloc, the SMB mount and the media/曲库 load fragment internal RAM down to
    // ~1.5 KB largest free block -- after which the controller could never get
    // its pool. stackUp() frees this right before esp_bt_controller_init().
    //
    // Only reserve when BT is actually enabled in config. Holding the idle
    // block through the AEC/audio/signaling inits squeezed one production
    // board so hard that the boot-tail main-loop stack fell into the slow
    // RTCRAM heap region (~4x slower polls; see main.cpp). The first Poll()
    // releases an idle reserve anyway, so an OFF boot never kept it -- a later
    // toggle-on was already best-effort, and stays that way.
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    if (cfg == nullptr || !cfg->bt_enabled) {
        ESP_LOGI(TAG, "BT disabled in config; skipping the %u B RAM reserve",
                 (unsigned)kBtReserveBytes);
        return;
    }
    if (s_bt_reserve == nullptr) {
        s_bt_reserve = heap_caps_malloc(kBtReserveBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        ESP_LOGI(TAG, "BT RAM reserve %s (%u B); largest_internal now=%u",
                 s_bt_reserve ? "ok" : "FAILED", (unsigned)kBtReserveBytes,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    // Enable/disable requests are applied from NRL_BtHfp_Poll() on the main loop.
}

void NRL_BtHfp_SetEnabled(bool enabled)
{
    // Non-blocking: just record the desired state. The BT task brings the stack
    // up/down (slow), so the caller -- a UI touch callback or boot -- never
    // blocks. Rapid toggles coalesce to the most recent request.
    s_request_enabled = enabled;
    s_request_pending = true;
}

bool NRL_BtHfp_TogglePending(void) { return s_request_pending || s_transitioning; }

bool NRL_BtHfp_IsEnabled(void) { return s_enabled; }

void NRL_BtHfp_StartScan(void)
{
    if (!s_stack_up || s_discovering) {
        return;
    }
    s_device_count = 0;
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, kInquiryLen, 0);
}

bool NRL_BtHfp_IsScanning(void) { return s_discovering; }

size_t NRL_BtHfp_GetDeviceCount(void) { return s_device_count; }

bool NRL_BtHfp_GetDeviceName(size_t index, char *out, size_t out_size)
{
    if (out == nullptr || out_size == 0u || index >= s_device_count) {
        return false;
    }
    snprintf(out, out_size, "%s", s_devices[index].name);
    return true;
}

void NRL_BtHfp_ConnectIndex(size_t index)
{
    if (!s_stack_up || index >= s_device_count) {
        return;
    }
    if (s_discovering) {
        esp_bt_gap_cancel_discovery();
    }
    memcpy(s_peer_bda, s_devices[index].bda, sizeof(s_peer_bda));
    s_have_peer = true;
    s_connecting = true;
    s_connect_started_ms = nowMs();
    s_last_reconnect_ms = nowMs();
    coexPreferBt(true);
    ESP_LOGI(TAG, "connecting to '%s'", s_devices[index].name);
    esp_hf_ag_slc_connect(s_peer_bda);
}

size_t NRL_BtHfp_GetSavedCount(void) { return s_saved_count; }

bool NRL_BtHfp_GetSavedName(size_t index, char *out, size_t out_size)
{
    if (out == nullptr || out_size == 0u || index >= s_saved_count) {
        return false;
    }
    snprintf(out, out_size, "%s", s_saved[index].name);
    return true;
}

void NRL_BtHfp_ConnectSaved(size_t index)
{
    if (!s_stack_up || index >= s_saved_count) {
        return;
    }
    if (s_discovering) {
        esp_bt_gap_cancel_discovery();
    }
    memcpy(s_peer_bda, s_saved[index].bda, sizeof(s_peer_bda));
    s_have_peer = true;
    s_connecting = true;
    s_connect_started_ms = nowMs();
    s_last_reconnect_ms = nowMs();
    coexPreferBt(true);
    ESP_LOGI(TAG, "connecting saved '%s'", s_saved[index].name);
    esp_hf_ag_slc_connect(s_peer_bda);
}

void NRL_BtHfp_RemoveSaved(size_t index)
{
    if (index >= s_saved_count) {
        return;
    }
    esp_bd_addr_t bda;
    memcpy(bda, s_saved[index].bda, sizeof(bda));
    // If we're forgetting the active/target headset, drop the link + target.
    if (s_have_peer && memcmp(s_peer_bda, bda, sizeof(bda)) == 0) {
        s_have_peer = false;
        if (s_connected) {
            esp_hf_ag_audio_disconnect(bda);
        }
    }
    for (size_t j = index; j + 1 < s_saved_count; ++j) {
        s_saved[j] = s_saved[j + 1];
    }
    --s_saved_count;
    persistSaved();
    if (s_stack_up) {
        esp_bt_gap_remove_bond_device(bda);
    }
    ESP_LOGI(TAG, "forgot saved headset");
}

bool NRL_BtHfp_IsConnected(void) { return s_connected; }

bool NRL_BtHfp_IsAudioActive(void) { return s_stack_up && s_audio_active; }

size_t NRL_BtHfp_GetPeerName(char *out, size_t out_size)
{
    if (out == nullptr || out_size == 0u) {
        return 0u;
    }
    const size_t n = strnlen(s_peer_name, sizeof(s_peer_name));
    const size_t copy = (n < out_size - 1u) ? n : out_size - 1u;
    memcpy(out, s_peer_name, copy);
    out[copy] = '\0';
    return copy;
}

int NRL_BtHfp_GetVolumePercent(void)
{
    if (!s_connected) {
        return -1;
    }
    return (static_cast<int>(s_peer_volume) * 100 + kSpkVolumeMax / 2) / kSpkVolumeMax;
}

void NRL_BtHfp_AdjustVolume(int direction)
{
    if (!s_connected || direction == 0) {
        return;
    }
    int v = static_cast<int>(s_peer_volume) + (direction > 0 ? 1 : -1);
    if (v < 0) {
        v = 0;
    } else if (v > kSpkVolumeMax) {
        v = kSpkVolumeMax;
    }
    if (static_cast<uint8_t>(v) == s_peer_volume) {
        return;  // already at the limit
    }
    s_peer_volume = static_cast<uint8_t>(v);
    applyPeerVolume();
    savePeerVolume();
    ESP_LOGI(TAG, "NRL volume key -> headset %u/15", (unsigned)s_peer_volume);
}

void NRL_BtHfp_SetVolumePercent(int percent)
{
    if (!s_connected) {
        return;
    }
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    const uint8_t gain = static_cast<uint8_t>((percent * kSpkVolumeMax + 50) / 100);
    if (gain == s_peer_volume) {
        return;
    }
    s_peer_volume = gain;
    applyPeerVolume();
    savePeerVolume();
    ESP_LOGI(TAG, "NRL slider -> headset %u/15", (unsigned)s_peer_volume);
}

void NRL_BtHfp_PushPlayback(const int16_t *samples, size_t sample_count)
{
    if (samples == nullptr || sample_count == 0u) {
        return;
    }
    // Downlink network voice is arriving -> a voice burst is live. Record this
    // even when the SCO is not up yet, so Poll opens it on demand. Without this,
    // receive-only audio (no local PTT) would never trigger the SCO and the
    // headset would stay silent until PTT was pressed.
    s_last_voice_ms = nowMs();
    if (!s_audio_active) {
        // SCO not up yet; nothing to buffer (the jitter buffer re-primes when the
        // SCO connects, so anything queued now would just be discarded).
        return;
    }
    size_t head = s_pcm_head;
    const size_t tail = s_pcm_tail;  // snapshot (consumer side)
    for (size_t i = 0; i < sample_count; ++i) {
        const size_t next = (head + 1u) % kPcmCap;
        if (next == tail) {
            break;  // buffer full -> drop the rest (headset is behind)
        }
        s_pcm[head] = samples[i];
        head = next;
    }
    s_pcm_head = head;
}

void NRL_BtHfp_Poll(void)
{
    // Apply a pending enable/disable request first (this runs on the BT task, so
    // the slow stack up/down never blocks the UI or the main loop).
    if (s_request_pending) {
        // Gate only a bring-up on free internal RAM (teardown and the
        // already-up/idempotent cases apply immediately). Leaving the request
        // pending re-checks on the next Poll, so BT comes up automatically once
        // the boot allocation peak clears. TogglePending() stays true meanwhile,
        // so the UI reads "connecting" rather than a spurious off.
        // When we hold the pre-reserved contiguous block, stackUp() frees it right
        // before the controller inits, guaranteeing the pool -- so don't gate on
        // the (shattered) live largest block. Only gate as a fallback when the
        // reserve failed to allocate at boot.
        const bool want_up = s_request_enabled && !s_stack_up;
        const size_t largest_int =
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        static uint32_t s_defer_start_ms = 0;  // when the current defer began (0 = not deferring)
        if (want_up && s_bt_reserve == nullptr && largest_int < kMinInternalBlockForBtUp) {
            const uint32_t now = nowMs();
            if (s_defer_start_ms == 0u) {
                s_defer_start_ms = now;
            }
            static uint32_t s_last_defer_log = 0;
            if (now - s_last_defer_log > 3000u) {
                s_last_defer_log = now;
                ESP_LOGW(TAG, "deferring BT enable: largest_internal=%u < %u (heap too fragmented)",
                         (unsigned)largest_int, (unsigned)kMinInternalBlockForBtUp);
            }
            // Bounded wait: if the heap never gets contiguous enough, abort the
            // enable so the UI switch resolves to off instead of spinning forever.
            if (now - s_defer_start_ms > kBtEnableGiveUpMs) {
                ESP_LOGE(TAG, "BT enable aborted: internal RAM too fragmented (largest=%u); leaving BT off",
                         (unsigned)largest_int);
                s_request_pending = false;
                s_request_enabled = false;
                s_enabled = false;
                s_defer_start_ms = 0;
            }
        } else {
            s_defer_start_ms = 0;
            s_request_pending = false;
            s_transitioning = true;
            applyEnabled(s_request_enabled);
            s_transitioning = false;
        }
    }
    // BT is off (and not being brought up): hand the pre-reserved 40 KB back to
    // the app. Music read-ahead + decode tasks need contiguous internal RAM too;
    // holding the reserve idle while BT is off starves them -- the SMB read-ahead
    // task fails to create and network playback falls back to unbuffered direct
    // reads (audible stutter). We can't re-grab the block once the heap is
    // fragmented, so this makes BT effectively boot-time-only when it starts off
    // -- consistent with the reserve model (a later enable defers/aborts anyway).
    if (s_bt_reserve != nullptr && !s_stack_up && !s_request_enabled && !s_request_pending) {
        heap_caps_free(s_bt_reserve);
        s_bt_reserve = nullptr;
        ESP_LOGI(TAG, "BT off; released %u B reserve to the app (largest_internal now=%u)",
                 (unsigned)kBtReserveBytes,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (!s_enabled || !s_stack_up) {
        return;
    }
#if defined(CONFIG_BT_A2DP_ENABLE)
    // Once A2DP is registered (Wi-Fi-off mode) and the headset's HFP link is up,
    // open the A2DP media connection to it so music can stream. One-shot.
    if (s_a2dp_registered && s_connected && !s_a2d_connected) {
        static uint32_t s_last_a2d_connect_ms = 0;
        const uint32_t now_a2d = nowMs();
        if (now_a2d - s_last_a2d_connect_ms > 3000u) {
            s_last_a2d_connect_ms = now_a2d;
            esp_a2d_source_connect(s_peer_bda);
        }
    }
#endif
    // Apply a pending headset-button PTT toggle outside the BT callback context.
    if (s_headset_ptt_dirty) {
        s_headset_ptt_dirty = false;
        STATUS_IO_SetSoftPtt(s_headset_ptt);
    }
    // Persist a headset-reported volume change (NVS write kept out of the callback).
    if (s_peer_volume_dirty) {
        s_peer_volume_dirty = false;
        savePeerVolume();
    }
    const uint32_t now = nowMs();
    // Let a stuck in-flight attempt expire so a fresh retry is allowed.
    if (s_connecting && (now - s_connect_started_ms >= kConnectTimeoutMs)) {
        s_connecting = false;
        // The attempt stalled (headset absent/unreachable): drop the BT radio
        // bias we set for it so Wi-Fi/SMB isn't starved between retries, and back
        // the retry cadence off so we're not page-scanning every 4 s.
        coexPreferBt(false);
        s_reconnect_interval_ms = (s_reconnect_interval_ms * 2u < kReconnectBackoffMaxMs)
                                      ? (s_reconnect_interval_ms * 2u)
                                      : kReconnectBackoffMaxMs;
    }
    if (!s_connected) {
        // Auto-reconnect only to the headset the user explicitly chose, and never
        // while a scan or another connect attempt is already in flight (a second
        // slc_connect mid-handshake collides and tears the link down).
        if (s_have_peer && !s_connecting && !s_discovering &&
            (now - s_last_reconnect_ms >= s_reconnect_interval_ms)) {
            s_last_reconnect_ms = now;
            s_connecting = true;
            s_connect_started_ms = now;
            coexPreferBt(true);
            esp_hf_ag_slc_connect(s_peer_bda);
        }
    } else {
        // Linked. Open the SCO voice link on demand and tear it down when idle,
        // so the headset can sleep (sniff) between transmissions.
        if (STATUS_IO_IsSqlActive()) {
            s_last_voice_ms = now;  // local PTT held -> a voice burst is live
        }
        const bool call_wanted =
            (s_last_voice_ms != 0u) && (now - s_last_voice_ms < kVoiceHangoverMs);

        if (call_wanted) {
            if (!s_call_active) {
                // Voice burst starting: tell the headset a call is active (so it
                // keeps SCO up) and open the SCO now. We leave coex on BALANCE --
                // biasing to BT did not reduce the warble and only made the WiFi
                // UDP voice jittery.
                s_call_active = true;
                esp_hf_ag_ciev_report(s_peer_bda, ESP_HF_IND_TYPE_CALL, 1);
                esp_hf_ag_ciev_report(s_peer_bda, ESP_HF_IND_TYPE_CALLSETUP, 0);
                s_last_audio_retry_ms = now;
                esp_hf_ag_audio_connect(s_peer_bda);
            } else if (!s_audio_active && (now - s_last_audio_retry_ms >= kAudioRetryIntervalMs)) {
                s_last_audio_retry_ms = now;
                esp_hf_ag_audio_connect(s_peer_bda);
            }
        } else if (s_call_active) {
            // Idle past the hangover: end the call so the link can sniff/save power.
            s_call_active = false;
            if (s_audio_active) {
                esp_hf_ag_audio_disconnect(s_peer_bda);
            }
            esp_hf_ag_ciev_report(s_peer_bda, ESP_HF_IND_TYPE_CALL, 0);
        }
    }
}

// ---- A2DP source public API -------------------------------------------------
#if defined(CONFIG_BT_A2DP_ENABLE)

bool NRL_BtA2dp_RequestStart(void)
{
    if (!s_enabled || !s_stack_up || !s_have_peer) {
        return false;
    }
    s_a2d_want_stream = true;
    if (!s_a2d_connected) {
        // Ride the existing ACL to the chosen headset; the connection event
        // continues the CHECK_SRC_RDY -> START ladder.
        return esp_a2d_source_connect(s_peer_bda) == ESP_OK;
    }
    if (!s_a2d_media_started) {
        return esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY) == ESP_OK;
    }
    return true;
}

void NRL_BtA2dp_RequestStop(void)
{
    s_a2d_want_stream = false;
    if (s_a2d_media_started) {
        (void)esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
    }
    a2dStopTx();
}

bool NRL_BtA2dp_IsStreaming(void)
{
    return s_a2d_media_started;
}

size_t NRL_BtA2dp_Write(const int16_t *stereo, size_t frames)
{
    if (stereo == nullptr || !s_a2d_media_started || s_a2d_ring == nullptr) {
        return 0u;
    }
    // Producer blocks while the ring is full: that back-pressure paces the
    // music decode loop the same way the I2S DMA does for the speaker path.
    size_t written = 0;
    int waits = 0;
    const size_t total = frames * 2u; // interleaved samples
    while (written < total && s_a2d_media_started) {
        const size_t next = (s_a2d_head + 1u) % kA2dPcmRingSamples;
        if (next == s_a2d_tail) {
            if (++waits > 40) { // ~400 ms: give up rather than wedge the player
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        s_a2d_ring[s_a2d_head] = stereo[written++];
        s_a2d_head = next;
    }
    return written / 2u;
}

#else // HFP without A2DP in this configuration

bool NRL_BtA2dp_RequestStart(void) { return false; }
void NRL_BtA2dp_RequestStop(void) {}
bool NRL_BtA2dp_IsStreaming(void) { return false; }
size_t NRL_BtA2dp_Write(const int16_t *, size_t) { return 0u; }

#endif // CONFIG_BT_A2DP_ENABLE

#else // !CONFIG_BT_HFP_AG_ENABLE -- non-S31 boards: no Bluetooth HFP.

#include <string.h>

void NRL_BtHfp_Init(void) {}
void NRL_BtHfp_SetEnabled(bool) {}
bool NRL_BtHfp_IsEnabled(void) { return false; }
bool NRL_BtHfp_TogglePending(void) { return false; }
void NRL_BtHfp_Poll(void) {}
void NRL_BtHfp_StartScan(void) {}
bool NRL_BtHfp_IsScanning(void) { return false; }
size_t NRL_BtHfp_GetDeviceCount(void) { return 0u; }
bool NRL_BtHfp_GetDeviceName(size_t, char *out, size_t out_size)
{
    if (out != nullptr && out_size != 0u) {
        out[0] = '\0';
    }
    return false;
}
void NRL_BtHfp_ConnectIndex(size_t) {}
size_t NRL_BtHfp_GetSavedCount(void) { return 0u; }
bool NRL_BtHfp_GetSavedName(size_t, char *out, size_t out_size)
{
    if (out != nullptr && out_size != 0u) {
        out[0] = '\0';
    }
    return false;
}
void NRL_BtHfp_ConnectSaved(size_t) {}
void NRL_BtHfp_RemoveSaved(size_t) {}
bool NRL_BtHfp_IsConnected(void) { return false; }
bool NRL_BtHfp_IsAudioActive(void) { return false; }
size_t NRL_BtHfp_GetPeerName(char *out, size_t out_size)
{
    if (out != nullptr && out_size != 0u) {
        out[0] = '\0';
    }
    return 0u;
}
void NRL_BtHfp_PushPlayback(const int16_t *, size_t) {}
int NRL_BtHfp_GetVolumePercent(void) { return -1; }
void NRL_BtHfp_AdjustVolume(int) {}
void NRL_BtHfp_SetVolumePercent(int) {}
bool NRL_BtA2dp_RequestStart(void) { return false; }
void NRL_BtA2dp_RequestStop(void) {}
bool NRL_BtA2dp_IsStreaming(void) { return false; }
size_t NRL_BtA2dp_Write(const int16_t *, size_t) { return 0u; }

#endif // CONFIG_BT_HFP_AG_ENABLE
