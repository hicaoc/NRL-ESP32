#ifndef DRIVER_SR110U_H
#define DRIVER_SR110U_H

#include <stdbool.h>
#include <stddef.h>

bool SR110U_Init(void);
bool SR110U_IsReady(void);
bool SR110U_Command(const char *command, char *response, size_t response_size,
                    unsigned timeout_ms);
int SR110U_ReadRssi(void);

#endif // DRIVER_SR110U_H
