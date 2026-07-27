#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum CwSource : uint8_t { CW_SOURCE_MIC = 0, CW_SOURCE_NRL = 1 };
enum CwElement : uint8_t { CW_ELEMENT_DIT = 0, CW_ELEMENT_DAH = 1 };

struct CwSnapshot {
    char rx_text[49];
    char rx_letters[97];
    char rx_code[97];
    char tx_text[33];
    char tx_letters[97];
    char tx_code[97];
    char current_pattern[8];
    char practice_target;
    uint16_t wpm;
    uint8_t accuracy_percent;
    uint16_t practice_attempts;
    uint32_t revision;
    bool practice_enabled;
    bool sending;
};

void CW_SERVICE_Init(void);
void CW_SERVICE_RecordReceived(CwSource source, char character,
                               const char *pattern, uint16_t wpm);
void CW_SERVICE_InputElement(CwElement element);
void CW_SERVICE_FinishCharacter(void);
void CW_SERVICE_Delete(void);
bool CW_SERVICE_Send(void);
void CW_SERVICE_Clear(void);
void CW_SERVICE_SetPractice(bool enabled);
void CW_SERVICE_GetSnapshot(CwSnapshot *out);
