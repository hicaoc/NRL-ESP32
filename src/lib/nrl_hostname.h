#ifndef SRC_LIB_NRL_HOSTNAME_H
#define SRC_LIB_NRL_HOSTNAME_H

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

inline void NrlMakeHostname(const char *callsign, const uint8_t ssid,
                            char *out, const size_t out_size)
{
    if (out == nullptr || out_size == 0u) return;

    char token[12] = {};
    size_t pos = 0u;
    bool last_was_dash = false;
    if (callsign != nullptr) {
        for (size_t i = 0; callsign[i] != '\0' && pos < sizeof(token) - 1u; ++i) {
            const unsigned char ch = static_cast<unsigned char>(callsign[i]);
            if (isalnum(ch)) {
                token[pos++] = static_cast<char>(tolower(ch));
                last_was_dash = false;
            } else if (!last_was_dash && pos > 0u) {
                token[pos++] = '-';
                last_was_dash = true;
            }
        }
    }
    while (pos > 0u && token[pos - 1u] == '-') {
        token[--pos] = '\0';
    }
    snprintf(out, out_size, "%s-%u", pos > 0u ? token : "nocall",
             static_cast<unsigned>(ssid));
}

#endif // SRC_LIB_NRL_HOSTNAME_H
