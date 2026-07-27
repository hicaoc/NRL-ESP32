#include "lib/nrl_text_message.h"

#include <ctype.h>
#include <string.h>

namespace {

bool tagEquals(const char *tag, const char *expected)
{
    if (tag == nullptr || expected == nullptr) return false;
    while (*tag != '\0' && *expected != '\0') {
        if (tolower(static_cast<unsigned char>(*tag)) !=
            tolower(static_cast<unsigned char>(*expected))) {
            return false;
        }
        ++tag;
        ++expected;
    }
    return *tag == '\0' && *expected == '\0';
}

NrlTextMessageKind kindFromTag(const char *tag)
{
    if (tagEquals(tag, "text")) return NRL_TEXT_MESSAGE_TEXT;
    if (tagEquals(tag, "1oc") || tagEquals(tag, "loc")) return NRL_TEXT_MESSAGE_LOCATION;
    if (tagEquals(tag, "json")) return NRL_TEXT_MESSAGE_JSON;
    if (tagEquals(tag, "xml")) return NRL_TEXT_MESSAGE_XML;
    if (tagEquals(tag, "html")) return NRL_TEXT_MESSAGE_HTML;
    if (tagEquals(tag, "bin")) return NRL_TEXT_MESSAGE_BINARY;
    if (tagEquals(tag, "img")) return NRL_TEXT_MESSAGE_IMAGE;
    if (tagEquals(tag, "video")) return NRL_TEXT_MESSAGE_VIDEO;
    if (tagEquals(tag, "audio")) return NRL_TEXT_MESSAGE_AUDIO;
    return NRL_TEXT_MESSAGE_UNKNOWN;
}

size_t utf8SequenceLength(const uint8_t lead)
{
    if (lead < 0x80u) return 1u;
    if (lead >= 0xC2u && lead <= 0xDFu) return 2u;
    if (lead >= 0xE0u && lead <= 0xEFu) return 3u;
    if (lead >= 0xF0u && lead <= 0xF4u) return 4u;
    return 0u;
}

bool validUtf8Sequence(const uint8_t *source, const size_t source_size, const size_t length)
{
    if (source == nullptr || length < 2u || source_size < length) return false;
    for (size_t i = 1u; i < length; ++i) {
        if ((source[i] & 0xC0u) != 0x80u) return false;
    }
    // Reject overlong forms, UTF-16 surrogates and values above U+10FFFF.
    if (length == 3u && source[0] == 0xE0u && source[1] < 0xA0u) return false;
    if (length == 3u && source[0] == 0xEDu && source[1] >= 0xA0u) return false;
    if (length == 4u && source[0] == 0xF0u && source[1] < 0x90u) return false;
    if (length == 4u && source[0] == 0xF4u && source[1] >= 0x90u) return false;
    return true;
}

} // namespace

NrlTextMessageView NRL_TEXT_MESSAGE_Parse(const uint8_t *payload, const size_t payload_size)
{
    NrlTextMessageView result{};
    result.kind = NRL_TEXT_MESSAGE_TEXT;
    result.content = payload;
    result.content_size = payload_size;
    if (payload == nullptr || payload_size < 3u || payload[0] != '[') return result;

    size_t close = 1u;
    while (close < payload_size && close <= sizeof(result.tag) && payload[close] != ']') {
        ++close;
    }
    if (close >= payload_size || close > sizeof(result.tag) || payload[close] != ']' || close == 1u) {
        return result;
    }

    const size_t tag_size = close - 1u;
    for (size_t i = 0u; i < tag_size; ++i) {
        const uint8_t ch = payload[i + 1u];
        if (!isalnum(ch) && ch != '_' && ch != '-') return result;
    }
    memcpy(result.tag, payload + 1u, tag_size);
    result.tag[tag_size] = '\0';
    result.kind = kindFromTag(result.tag);
    result.content = payload + close + 1u;
    result.content_size = payload_size - close - 1u;
    return result;
}

const char *NRL_TEXT_MESSAGE_KindLabel(const NrlTextMessageKind kind)
{
    switch (kind) {
        case NRL_TEXT_MESSAGE_TEXT: return "TEXT";
        case NRL_TEXT_MESSAGE_LOCATION: return "LOC";
        case NRL_TEXT_MESSAGE_JSON: return "JSON";
        case NRL_TEXT_MESSAGE_XML: return "XML";
        case NRL_TEXT_MESSAGE_HTML: return "HTML";
        case NRL_TEXT_MESSAGE_BINARY: return "BIN";
        case NRL_TEXT_MESSAGE_IMAGE: return "IMG";
        case NRL_TEXT_MESSAGE_VIDEO: return "VIDEO";
        case NRL_TEXT_MESSAGE_AUDIO: return "AUDIO";
        case NRL_TEXT_MESSAGE_UNKNOWN: return "UNKNOWN";
        default: return "UNKNOWN";
    }
}

size_t NRL_TEXT_MESSAGE_CopySanitized(const uint8_t *source, const size_t source_size,
                                      char *destination, const size_t destination_size)
{
    if (destination == nullptr || destination_size == 0u) return 0u;
    destination[0] = '\0';
    if (source == nullptr || source_size == 0u) return 0u;

    size_t input = 0u;
    size_t output = 0u;
    while (input < source_size && output + 1u < destination_size) {
        const uint8_t byte = source[input];
        if (byte < 0x80u) {
            destination[output++] = (byte < 0x20u || byte == 0x7Fu) ? ' ' : static_cast<char>(byte);
            ++input;
            continue;
        }

        const size_t sequence = utf8SequenceLength(byte);
        if (sequence == 0u || !validUtf8Sequence(source + input, source_size - input, sequence)) {
            destination[output++] = '?';
            ++input;
            continue;
        }
        if (output + sequence >= destination_size) break;
        memcpy(destination + output, source + input, sequence);
        output += sequence;
        input += sequence;
    }
    while (output > 0u && destination[output - 1u] == ' ') --output;
    destination[output] = '\0';
    return output;
}
