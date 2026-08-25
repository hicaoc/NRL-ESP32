#pragma once
// Minimal libsodium subset used by the host-compiled units under test
// (fmo_protocol.cpp). Implemented in the test main; NOT for firmware builds.

#include <stddef.h>
#include <stdint.h>

#define sodium_base64_VARIANT_ORIGINAL 1
#define sodium_base64_VARIANT_ORIGINAL_NO_PADDING 3
#define sodium_base64_VARIANT_URLSAFE 5
#define sodium_base64_VARIANT_URLSAFE_NO_PADDING 7

int sodium_init(void);
int sodium_base642bin(unsigned char *bin, size_t bin_maxlen, const char *b64,
                      size_t b64_len, const char *ignore, size_t *bin_len,
                      const char **b64_end, int variant);
int crypto_hash_sha256(unsigned char *out, const unsigned char *in,
                       unsigned long long inlen);
