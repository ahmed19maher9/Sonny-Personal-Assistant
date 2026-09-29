#include "VectorsCrypto.h"

#include <windows.h>
#include <bcrypt.h>

#include "BrowserCrypto.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <random>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace Jarvis {
namespace Vectors {

namespace {

// BCryptCloseAlgorithmProvider takes (handle, flags) and BCryptDestroyHash takes
// (handle), so neither can be a unique_ptr deleter directly; these thin functors
// supply the right call shape. Names are unique to keep unity builds clean.
struct VectorsAlgCloser {
    void operator()(void* handle) const {
        if (handle) {
            BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(handle), 0);
        }
    }
};

struct VectorsHashCloser {
    void operator()(void* handle) const {
        if (handle) {
            BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(handle));
        }
    }
};

using VectorsAlgHandle = std::unique_ptr<void, VectorsAlgCloser>;
using VectorsHashHandle = std::unique_ptr<void, VectorsHashCloser>;

bool vectors_sha256(const unsigned char* data, size_t size, unsigned char out[32]) {
    BCRYPT_ALG_HANDLE raw_algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&raw_algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return false;
    }
    VectorsAlgHandle algorithm(raw_algorithm);

    DWORD object_size = 0;
    DWORD written = 0;
    if (BCryptGetProperty(algorithm.get(), BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &written,
                          0) != 0) {
        return false;
    }

    // The provider writes its working state into this buffer, so it must stay
    // alive for as long as the hash handle does.
    std::vector<unsigned char> object(object_size);

    BCRYPT_HASH_HANDLE raw_hash = nullptr;
    if (BCryptCreateHash(algorithm.get(), &raw_hash, object.data(), object_size, nullptr, 0, 0) !=
        0) {
        return false;
    }
    VectorsHashHandle hash(raw_hash);

    if (size > 0) {
        if (BCryptHashData(hash.get(), const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) != 0) {
            return false;
        }
    }
    if (BCryptFinishHash(hash.get(), out, 32, 0) != 0) return false;
    return true;
}

}  // namespace

std::string hmac_sha256_hex(const std::string& key, const std::string& message) {
    constexpr size_t kBlockSize = 64;

    std::vector<unsigned char> normalized(key.begin(), key.end());
    if (normalized.size() > kBlockSize) {
        unsigned char digest[32];
        if (!vectors_sha256(normalized.data(), normalized.size(), digest)) return std::string();
        normalized.assign(digest, digest + 32);
    }
    normalized.resize(kBlockSize, 0);

    std::vector<unsigned char> inner(normalized);
    for (size_t i = 0; i < kBlockSize; ++i) inner[i] ^= 0x36;
    inner.insert(inner.end(), message.begin(), message.end());

    unsigned char inner_digest[32];
    if (!vectors_sha256(inner.data(), inner.size(), inner_digest)) return std::string();

    std::vector<unsigned char> outer(normalized);
    for (size_t i = 0; i < kBlockSize; ++i) outer[i] ^= 0x5c;
    outer.insert(outer.end(), inner_digest, inner_digest + 32);

    unsigned char final_digest[32];
    if (!vectors_sha256(outer.data(), outer.size(), final_digest)) return std::string();

    static const char* kHexDigits = "0123456789abcdef";
    std::string hex;
    hex.reserve(64);
    for (unsigned char byte : final_digest) {
        hex.push_back(kHexDigits[byte >> 4]);
        hex.push_back(kHexDigits[byte & 0x0F]);
    }
    return hex;
}

bool secure_equals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char difference = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        difference |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return difference == 0;
}

std::string random_token() {
    unsigned char raw[16] = {0};
    if (BCryptGenRandom(nullptr, raw, sizeof(raw), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return Browser::Crypto::random_base64_key();
    }
    return Browser::Crypto::base64_encode(raw, sizeof(raw));
}

double add_gaussian_noise(double value, double sigma) {
    if (sigma <= 0.0) return value;

    // One generator per thread: the sync loop and the transport threads must
    // not share RNG state.
    static thread_local std::mt19937_64 generator([] {
        std::random_device device;
        return (static_cast<std::uint64_t>(device()) << 32) ^ static_cast<std::uint64_t>(device());
    }());
    static thread_local std::normal_distribution<double> distribution(0.0, 1.0);

    const double noisy = value + distribution(generator) * sigma;
    return noisy < 0.0 ? 0.0 : noisy;
}

int round_to_step(int value, int step) {
    if (step <= 1 || value <= 0) return value;
    return ((value + step / 2) / step) * step;
}

}  // namespace Vectors
}  // namespace Jarvis
