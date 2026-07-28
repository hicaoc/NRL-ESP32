#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum CwSource : uint8_t { CW_SOURCE_MIC = 0, CW_SOURCE_NRL = 1 };
enum CwElement : uint8_t { CW_ELEMENT_DIT = 0, CW_ELEMENT_DAH = 1 };
// Practice modes: TX shows a letter to key back; RX plays a letter (from the
// Koch-unlocked set) that must be copied and keyed back as the answer.
enum CwPracticeMode : uint8_t { CW_PRACTICE_OFF = 0, CW_PRACTICE_TX = 1, CW_PRACTICE_RX = 2 };

struct CwSnapshot {
    char rx_text[49];
    char rx_letters[97];
    char rx_code[97];
    char tx_text[33];
    char tx_letters[97];
    char tx_code[97];
    char current_pattern[8];
    char practice_target;       // TX: shown letter. RX: hidden answer -- never display it.
    uint16_t wpm;
    uint8_t accuracy_percent;
    uint8_t timing_percent;   // key-timing quality (dit:dah ratio), practice mode
    uint8_t practice_mode;    // CwPracticeMode
    uint8_t koch_unlocked;    // letters available in practice (2..36)
    bool copy_awaiting;       // RX: target played, waiting for the keyed answer
    char copy_revealed;       // RX: previous target revealed as feedback ('\0' = none)
    bool copy_last_correct;   // RX: previous answer result
    uint16_t practice_attempts;
    uint32_t revision;
    bool practice_enabled;
    bool sending;
};

void CW_SERVICE_Init(void);
void CW_SERVICE_RecordReceived(CwSource source, char character,
                               const char *pattern, uint16_t wpm);
void CW_SERVICE_InputElement(CwElement element);
// Straight-key input for touch UIs: KeyDown starts the sidetone immediately,
// KeyUp stops it and classifies the held duration into a dit/dah element
// (threshold follows the current WPM). Durations feed the practice-mode
// timing score. KeyUp without a preceding KeyDown is ignored.
void CW_SERVICE_KeyDown(void);
void CW_SERVICE_KeyUp(void);
// Single-paddle keyer for touch UIs: while held, the service emits a stream
// of the given element with standard 1:3:1 WPM timing and sidetone, like an
// electronic keyer. Starting with the other element mid-stream switches it
// (finger slide); PaddleStop finishes the current element plus space, then
// stops. Keyer elements score as ideal timing in practice mode.
void CW_SERVICE_PaddleStart(CwElement element);
void CW_SERVICE_PaddleStop(void);
void CW_SERVICE_FinishCharacter(void);
void CW_SERVICE_Delete(void);
bool CW_SERVICE_Send(void);
void CW_SERVICE_Clear(void);
void CW_SERVICE_SetPractice(bool enabled);   // legacy toggle: maps to TX / OFF
void CW_SERVICE_SetPracticeMode(CwPracticeMode mode);
// RX practice: play the current hidden target again (no-op outside RX).
void CW_SERVICE_ReplayTarget(void);
void CW_SERVICE_GetSnapshot(CwSnapshot *out);
