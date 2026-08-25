#pragma once

#include "driver/board_pins.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum SignalingRoute : uint8_t {
    SIGNAL_ROUTE_RX_MIC = 0,
    SIGNAL_ROUTE_RX_NRL,
    SIGNAL_ROUTE_TX_NRL,
    SIGNAL_ROUTE_TX_SPEAKER,
};

struct SignalingConfig {
    bool ctcss_rx_mic;
    bool ctcss_rx_nrl;
    bool mdc_rx_mic;
    bool mdc_rx_nrl;
    bool cw_rx_mic;
    bool cw_rx_nrl;
    bool mdc_tx_nrl;
    bool mdc_tx_speaker;
    bool dtmf_rx_mic;
    bool dtmf_rx_nrl;
    bool dtmf_tx_nrl;
    bool dtmf_tx_speaker;
    uint8_t mdc_opcode;
    uint8_t mdc_argument;
    uint16_t mdc_unit_id;
    char dtmf_digits[17];
};

#if defined(NRL_HAS_SIGNALING) && NRL_HAS_SIGNALING

void SIGNALING_Init(void);
void SIGNALING_GetConfig(SignalingConfig *out);
bool SIGNALING_SetMdcRoute(SignalingRoute route, bool enabled);
bool SIGNALING_SetCwRoute(SignalingRoute route, bool enabled);
bool SIGNALING_SetDtmfRoute(SignalingRoute route, bool enabled);
bool SIGNALING_SetCtcssRoute(SignalingRoute route, bool enabled);
bool SIGNALING_SetMdcPacket(uint8_t opcode, uint8_t argument, uint16_t unit_id);
bool SIGNALING_SetDtmfDigits(const char *digits);

// Called by the NRL bridge at the two voice-tail boundaries. They only queue
// work; PCM generation happens in the signaling task, never in the bridge.
void SIGNALING_OnLocalPttReleased(void);
void SIGNALING_OnNetworkVoiceEnded(void);

// Raw 16 kHz MIC tap used only by CTCSS. Call before the optional 200 Hz
// speech high-pass filter so sub-audible PL tones are not removed.
void SIGNALING_FeedRawMic(const int16_t *samples, size_t sample_count);

uint32_t SIGNALING_GetRevision(void);
void SIGNALING_GetLastResult(char *out, size_t out_size);

#else

// Boards with NRL_HAS_SIGNALING 0 (no-display boards such as BH4TDV 3188 and
// the S31 Function CoreBoard) build without the CTCSS/CW/MDC1200/DTMF stack;
// every entry point compiles to a no-op so callers need no board guards.
inline void SIGNALING_Init(void) {}
inline void SIGNALING_GetConfig(SignalingConfig *out)
{
    if (out != nullptr) *out = SignalingConfig{};
}
inline bool SIGNALING_SetMdcRoute(SignalingRoute, bool) { return false; }
inline bool SIGNALING_SetCwRoute(SignalingRoute, bool) { return false; }
inline bool SIGNALING_SetDtmfRoute(SignalingRoute, bool) { return false; }
inline bool SIGNALING_SetCtcssRoute(SignalingRoute, bool) { return false; }
inline bool SIGNALING_SetMdcPacket(uint8_t, uint8_t, uint16_t) { return false; }
inline bool SIGNALING_SetDtmfDigits(const char *) { return false; }
inline void SIGNALING_OnLocalPttReleased(void) {}
inline void SIGNALING_OnNetworkVoiceEnded(void) {}
inline void SIGNALING_FeedRawMic(const int16_t *, size_t) {}
inline uint32_t SIGNALING_GetRevision(void) { return 0; }
inline void SIGNALING_GetLastResult(char *out, size_t out_size)
{
    if (out != nullptr && out_size > 0u) out[0] = '\0';
}

#endif
