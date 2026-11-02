#ifndef DRIVER_PTT_OUTPUT_H
#define DRIVER_PTT_OUTPUT_H

#include <stdint.h>

constexpr int NrlPttOutputLevel(const bool active, const uint8_t active_level)
{
    return active == (active_level != 0u) ? 1 : 0;
}

#endif // DRIVER_PTT_OUTPUT_H
