#pragma once

#include <string>
#include <cstddef>

namespace Jarvis {
namespace Browser {
namespace Crypto {

// ---------------------------------------------------------------------------
// The two primitives the browser subsystem needs, kept in one place so the
// WebSocket handshake (RFC 6455 requires SHA-1 + Base64) and the credential
// vault (DPAPI blobs are stored Base64-encoded) share a single implementation.
// ---------------------------------------------------------------------------

std::string base64_encode(const unsigned char* data, size_t len);
std::string base64_encode(const std::string& raw);
// Returns false when `text` is not valid Base64.
bool base64_decode(const std::string& text, std::string& raw);

// Base64(SHA1(input)) - the Sec-WebSocket-Accept computation.
std::string sha1_base64(const std::string& input);

// 16 cryptographically-unremarkable random bytes, Base64 encoded.
std::string random_base64_key();

}  // namespace Crypto
}  // namespace Browser
}  // namespace Jarvis