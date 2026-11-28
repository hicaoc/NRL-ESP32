#include "nrl_captive_dns.h"

#include <esp_log.h>
#include <fcntl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lwip/sockets.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "DNS";

namespace {

constexpr uint16_t kDnsPort = 53;
constexpr size_t kDnsBufferSize = 512;
constexpr int kMaxPacketsPerPoll = 16;
constexpr uint16_t kFlagResponse = 0x8000;
constexpr uint16_t kFlagAuthoritative = 0x0400;
constexpr uint16_t kFlagRD = 0x0100;
constexpr uint16_t kFlagRA = 0x0080;
constexpr uint16_t kQTypeA = 1;
constexpr uint16_t kQClassIN = 1;
constexpr uint32_t kAnswerTtlSeconds = 60;

portMUX_TYPE s_dns_init_spin = portMUX_INITIALIZER_UNLOCKED;
StaticSemaphore_t s_dns_mutex_buf;
SemaphoreHandle_t s_dns_mutex = nullptr;
int s_socket = -1;
uint32_t s_ap_ip_net = 0u;

static SemaphoreHandle_t getDnsMutex(void)
{
    if (s_dns_mutex == nullptr) {
        portENTER_CRITICAL(&s_dns_init_spin);
        if (s_dns_mutex == nullptr) {
            s_dns_mutex = xSemaphoreCreateMutexStatic(&s_dns_mutex_buf);
        }
        portEXIT_CRITICAL(&s_dns_init_spin);
    }
    return s_dns_mutex;
}

static uint16_t readBe16(const uint8_t *p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

static void writeBe16(uint8_t *p, uint16_t v)
{
    p[0] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[1] = static_cast<uint8_t>(v & 0xFFu);
}

static void writeBe32(uint8_t *p, uint32_t v)
{
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[3] = static_cast<uint8_t>(v & 0xFFu);
}

// Skip a DNS-encoded name (sequence of length-prefixed labels, terminated by
// a zero byte). Returns the offset past the name, or 0 on malformed input.
static size_t skipName(const uint8_t *buf, size_t off, size_t len)
{
    while (off < len) {
        const uint8_t b = buf[off];
        if (b == 0) {
            return off + 1u;
        }
        // Compressed pointer (high 2 bits set): 2-byte field, terminates.
        if ((b & 0xC0u) == 0xC0u) {
            return (off + 2u <= len) ? off + 2u : 0u;
        }
        if ((b & 0xC0u) != 0u) {
            return 0u; // reserved encoding
        }
        off += 1u + b;
    }
    return 0u;
}

// Build an A-record reply for the first question in the incoming query.
// Returns the response size, or 0 on malformed/unsupported input.
static size_t buildReply(const uint8_t *query, size_t qlen, uint8_t *out, size_t out_capacity)
{
    if (qlen < 12) {
        return 0u;
    }

    const uint16_t qd_count = readBe16(query + 4);
    if (qd_count != 1) {
        // Multi-question packets are rare; reject rather than guess.
        return 0u;
    }

    const size_t question_start = 12u;
    const size_t name_end = skipName(query, question_start, qlen);
    if (name_end == 0u || name_end + 4u > qlen) {
        return 0u;
    }
    const uint16_t qtype = readBe16(query + name_end);
    const uint16_t qclass = readBe16(query + name_end + 2u);
    const size_t question_end = name_end + 4u;
    if (question_end > out_capacity) {
        return 0u;
    }

    // Copy the request verbatim (header + question), patch the flags + counts,
    // and append the answer.
    memcpy(out, query, question_end);
    writeBe16(out + 2, kFlagResponse | kFlagAuthoritative | kFlagRD | kFlagRA);
    writeBe16(out + 4, 1); // QDCOUNT
    writeBe16(out + 6, (qtype == kQTypeA && qclass == kQClassIN) ? 1 : 0); // ANCOUNT
    writeBe16(out + 8, 0); // NSCOUNT
    writeBe16(out + 10, 0); // ARCOUNT

    size_t off = question_end;
    if (qtype != kQTypeA || qclass != kQClassIN) {
        // Echo the question with no answer for unsupported types. Most captive
        // probes only send A queries; this branch is a politeness path.
        return off;
    }

    // Answer record: name pointer to the question name (offset 12), type A,
    // class IN, TTL, RDLENGTH=4, RDATA=ap_ip (network byte order).
    if (off + 16u > out_capacity) {
        return 0u;
    }
    writeBe16(out + off, 0xC00Cu);    // pointer to offset 12 (question name)
    off += 2u;
    writeBe16(out + off, kQTypeA);
    off += 2u;
    writeBe16(out + off, kQClassIN);
    off += 2u;
    writeBe32(out + off, kAnswerTtlSeconds);
    off += 4u;
    writeBe16(out + off, 4);          // RDLENGTH
    off += 2u;
    // RDATA: ip in network byte order. s_ap_ip_net is already network byte order.
    memcpy(out + off, &s_ap_ip_net, 4u);
    off += 4u;
    return off;
}

} // namespace

extern "C" bool NRL_CaptiveDNS_Start(uint32_t ap_ip)
{
    SemaphoreHandle_t mtx = getDnsMutex();
    if (mtx != nullptr) {
        xSemaphoreTake(mtx, portMAX_DELAY);
    }

    if (s_socket >= 0) {
        s_ap_ip_net = ap_ip;
        if (mtx != nullptr) {
            xSemaphoreGive(mtx);
        }
        return true;
    }

    s_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_socket < 0) {
        ESP_LOGE(TAG, "socket() failed");
        if (mtx != nullptr) {
            xSemaphoreGive(mtx);
        }
        return false;
    }

    struct sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(kDnsPort);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s_socket, reinterpret_cast<struct sockaddr *>(&local), sizeof(local)) < 0) {
        ESP_LOGE(TAG, "bind(53) failed");
        close(s_socket);
        s_socket = -1;
        if (mtx != nullptr) {
            xSemaphoreGive(mtx);
        }
        return false;
    }

    const int flags = fcntl(s_socket, F_GETFL, 0);
    if (flags < 0 || fcntl(s_socket, F_SETFL, flags | O_NONBLOCK) < 0) {
        ESP_LOGE(TAG, "set O_NONBLOCK failed");
        close(s_socket);
        s_socket = -1;
        if (mtx != nullptr) {
            xSemaphoreGive(mtx);
        }
        return false;
    }

    s_ap_ip_net = ap_ip;
    if (mtx != nullptr) {
        xSemaphoreGive(mtx);
    }
    ESP_LOGI(TAG, "captive DNS listening on UDP/53");
    return true;
}

extern "C" int NRL_CaptiveDNS_Poll(void)
{
    SemaphoreHandle_t mtx = getDnsMutex();
    if (mtx != nullptr && xSemaphoreTake(mtx, 0) != pdTRUE) {
        return 0;
    }
    if (s_socket < 0) {
        if (mtx != nullptr) {
            xSemaphoreGive(mtx);
        }
        return 0;
    }

    int replies = 0;
    uint8_t rx[kDnsBufferSize];
    uint8_t tx[kDnsBufferSize];

    for (int handled = 0; handled < kMaxPacketsPerPoll; ++handled) {
        struct sockaddr_in from = {};
        socklen_t from_len = sizeof(from);
        const ssize_t got = recvfrom(s_socket,
                                     rx,
                                     sizeof(rx),
                                     MSG_DONTWAIT,
                                     reinterpret_cast<struct sockaddr *>(&from),
                                     &from_len);
        if (got <= 0) {
            break;
        }

        const size_t reply_size = buildReply(rx, static_cast<size_t>(got), tx, sizeof(tx));
        if (reply_size == 0u) {
            continue;
        }
        const ssize_t sent = sendto(s_socket,
                                    tx,
                                    reply_size,
                                    0,
                                    reinterpret_cast<struct sockaddr *>(&from),
                                    from_len);
        if (sent == static_cast<ssize_t>(reply_size)) {
            ++replies;
        }
    }
    if (mtx != nullptr) {
        xSemaphoreGive(mtx);
    }
    return replies;
}

extern "C" void NRL_CaptiveDNS_Stop(void)
{
    SemaphoreHandle_t mtx = getDnsMutex();
    if (mtx != nullptr) {
        xSemaphoreTake(mtx, portMAX_DELAY);
    }
    if (s_socket >= 0) {
        close(s_socket);
        s_socket = -1;
        ESP_LOGI(TAG, "captive DNS stopped");
    }
    if (mtx != nullptr) {
        xSemaphoreGive(mtx);
    }
}

extern "C" bool NRL_CaptiveDNS_IsRunning(void)
{
    SemaphoreHandle_t mtx = getDnsMutex();
    if (mtx != nullptr) {
        xSemaphoreTake(mtx, portMAX_DELAY);
    }
    const bool running = (s_socket >= 0);
    if (mtx != nullptr) {
        xSemaphoreGive(mtx);
    }
    return running;
}
