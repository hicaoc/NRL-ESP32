#include "lib/nrl_text_message.h"

#include <assert.h>
#include <string.h>

namespace {

NrlTextMessageView parse(const char *text)
{
    return NRL_TEXT_MESSAGE_Parse(reinterpret_cast<const uint8_t *>(text), strlen(text));
}

void expect(const char *payload, const NrlTextMessageKind kind, const char *content)
{
    const NrlTextMessageView message = parse(payload);
    assert(message.kind == kind);
    assert(message.content_size == strlen(content));
    assert(memcmp(message.content, content, message.content_size) == 0);
}

} // namespace

int main()
{
    expect("hello", NRL_TEXT_MESSAGE_TEXT, "hello");
    expect("[text]hello", NRL_TEXT_MESSAGE_TEXT, "hello");
    expect("[1oc]31.379459,120.602816", NRL_TEXT_MESSAGE_LOCATION,
           "31.379459,120.602816");
    expect("[LOC]31,120", NRL_TEXT_MESSAGE_LOCATION, "31,120");
    expect("[json]{\"ok\":true}", NRL_TEXT_MESSAGE_JSON, "{\"ok\":true}");
    expect("[xml]<x/>", NRL_TEXT_MESSAGE_XML, "<x/>");
    expect("[html]<b>x</b>", NRL_TEXT_MESSAGE_HTML, "<b>x</b>");
    expect("[bin]https://oss/a.bin", NRL_TEXT_MESSAGE_BINARY, "https://oss/a.bin");
    expect("[img]https://oss/a.jpg", NRL_TEXT_MESSAGE_IMAGE, "https://oss/a.jpg");
    expect("[video]https://oss/a.mp4", NRL_TEXT_MESSAGE_VIDEO, "https://oss/a.mp4");
    expect("[audio]https://oss/a.mp3", NRL_TEXT_MESSAGE_AUDIO, "https://oss/a.mp3");
    expect("[custom]data", NRL_TEXT_MESSAGE_UNKNOWN, "data");
    expect("[missing", NRL_TEXT_MESSAGE_TEXT, "[missing");

    char output[32];
    const uint8_t controls[] = {'a', '\n', 'b', 0u, 'c', '\r'};
    assert(NRL_TEXT_MESSAGE_CopySanitized(controls, sizeof(controls), output, sizeof(output)) == 5u);
    assert(strcmp(output, "a b c") == 0);

    const uint8_t chinese[] = {0xE4u, 0xB8u, 0xADu}; // U+4E2D
    char too_small[3];
    assert(NRL_TEXT_MESSAGE_CopySanitized(chinese, sizeof(chinese), too_small,
                                          sizeof(too_small)) == 0u);
    char exact[4];
    assert(NRL_TEXT_MESSAGE_CopySanitized(chinese, sizeof(chinese), exact, sizeof(exact)) == 3u);
    assert(memcmp(exact, chinese, sizeof(chinese)) == 0 && exact[3] == '\0');

    const uint8_t invalid_utf8[] = {0xFFu, 'x'};
    assert(NRL_TEXT_MESSAGE_CopySanitized(invalid_utf8, sizeof(invalid_utf8), output,
                                          sizeof(output)) == 2u);
    assert(strcmp(output, "?x") == 0);
    return 0;
}
