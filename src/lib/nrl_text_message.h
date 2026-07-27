#pragma once

#include <stddef.h>
#include <stdint.h>

enum NrlTextMessageKind : uint8_t {
    NRL_TEXT_MESSAGE_TEXT = 0,
    NRL_TEXT_MESSAGE_LOCATION,
    NRL_TEXT_MESSAGE_JSON,
    NRL_TEXT_MESSAGE_XML,
    NRL_TEXT_MESSAGE_HTML,
    NRL_TEXT_MESSAGE_BINARY,
    NRL_TEXT_MESSAGE_IMAGE,
    NRL_TEXT_MESSAGE_VIDEO,
    NRL_TEXT_MESSAGE_AUDIO,
    NRL_TEXT_MESSAGE_UNKNOWN,
};

struct NrlTextMessageView {
    NrlTextMessageKind kind;
    const uint8_t *content;
    size_t content_size;
    char tag[9];
};

// Parses an NRL type-5 payload. A payload without a valid [tag] prefix is
// plain text. Both the protocol spelling [1oc] (digit one) and [loc] are
// accepted for coordinates.
NrlTextMessageView NRL_TEXT_MESSAGE_Parse(const uint8_t *payload, size_t payload_size);

const char *NRL_TEXT_MESSAGE_KindLabel(NrlTextMessageKind kind);

// Copies payload bytes into a NUL-terminated display/log string. ASCII control
// characters are replaced with spaces and UTF-8 is validated without cutting
// a multi-byte character at the destination boundary.
size_t NRL_TEXT_MESSAGE_CopySanitized(const uint8_t *source, size_t source_size,
                                      char *destination, size_t destination_size);
