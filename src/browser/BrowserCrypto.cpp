#include "BrowserCrypto.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdint>
#include <cstring>

#pragma comment(lib, "bcrypt.lib")

namespace Jarvis {
namespace Browser {
namespace Crypto {

namespace {

inline uint32_t rol(uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
}

// Compact SHA-1 (RFC 3174). Only used for the WebSocket handshake digest, which
// the protocol defines as SHA-1; this is not a security boundary.
void sha1(const unsigned char* data, size_t len, unsigned char out[20]) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};

    std::string buffer(reinterpret_cast<const char*>(data), len);
    buffer.push_back(static_cast<char>(0x80));
    while (buffer.size() % 64 != 56) buffer.push_back('\0');

    const uint64_t bit_length = static_cast<uint64_t>(len) * 8;
    for (int i = 7; i >= 0; --i) {
        buffer.push_back(static_cast<char>((bit_length >> (i * 8)) & 0xFF));
    }

    for (size_t offset = 0; offset < buffer.size(); offset += 64) {
        const unsigned char* block = reinterpret_cast<const unsigned char*>(buffer.data() + offset);
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
                   (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            uint32_t temp = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = temp;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    for (int i = 0; i < 5; ++i) {
        out[i * 4] = static_cast<unsigned char>((h[i] >> 24) & 0xFF);
        out[i * 4 + 1] = static_cast<unsigned char>((h[i] >> 16) & 0xFF);
        out[i * 4 + 2] = static_cast<unsigned char>((h[i] >> 8) & 0xFF);
        out[i * 4 + 3] = static_cast<unsigned char>(h[i] & 0xFF);
    }
}

const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

}  // anonymous namespace

std::string base64_encode(const unsigned char* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= len) {
        uint32_t triple = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
        out.push_back(kAlphabet[triple & 0x3F]);
        i += 3;
    }
    if (i + 1 == len) {
        uint32_t triple = uint32_t(data[i]) << 16;
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (i + 2 == len) {
        uint32_t triple = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

std::string base64_encode(const std::string& raw) {
    return base64_encode(reinterpret_cast<const unsigned char*>(raw.data()), raw.size());
}

bool base64_decode(const std::string& text, std::string& raw) {
    raw.clear();
    int values[256];
    for (int i = 0; i < 256; ++i) values[i] = -1;
    for (int i = 0; i < 64; ++i) {
        values[static_cast<unsigned char>(kAlphabet[i])] = i;
    }

    int accumulator = 0;
    int bits = 0;
    for (char c : text) {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ') continue;
        int value = values[static_cast<unsigned char>(c)];
        if (value < 0) return false;
        accumulator = (accumulator << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            raw.push_back(static_cast<char>((accumulator >> bits) & 0xFF));
        }
    }
    return true;
}

std::string sha1_base64(const std::string& input) {
    unsigned char digest[20];
    sha1(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest);
    return base64_encode(digest, sizeof(digest));
}

std::string random_base64_key() {
    unsigned char raw[16] = {0};
    // Prefer the platform CSPRNG; fall back to rand() only if it is unavailable.
    if (!BCryptGenRandom(nullptr, raw, sizeof(raw), BCRYPT_USE_SYSTEM_PREFERRED_RNG)) {
        for (size_t i = 0; i < sizeof(raw); ++i) {
            raw[i] = static_cast<unsigned char>(rand() & 0xFF);
        }
    }
    return base64_encode(raw, sizeof(raw));
}

}  // namespace Crypto
}  // namespace Browser
}  // namespace Jarvis