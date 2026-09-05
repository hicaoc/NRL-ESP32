#ifndef DRIVER_VOX_H
#define DRIVER_VOX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void VOX_Configure(bool enabled, int open_db, int close_db, uint16_t attack_ms, uint16_t hang_ms);
void VOX_ProcessFrame(const int16_t *samples, size_t count);
bool VOX_IsActive(void);
float VOX_CurrentLevelDb(void);

#ifdef __cplusplus
}
#endif

#endif // DRIVER_VOX_H
