#pragma once
// ============================================================================
// VectorsCrypto.h - the privacy primitives of the peer network.
//
//   * HMAC-SHA256 over the payload, so a peer can prove a contribution came
//     from someone holding the room secret.
//   * Gaussian noise, so published aggregates cannot be differenced back to a
//     single event stream.
//   * Bucketing, so exact counts are never disclosed.
//
// SHA-256 comes from the platform CNG provider (bcrypt.lib, already linked by
// the browser credential vault), so no third-party crypto is added.
// ============================================================================

#include <string>

namespace Jarvis {
namespace Vectors {

// HMAC-SHA256 (RFC 2104) returned as 64 lowercase hex characters. Returns an
// empty string if the platform provider is unavailable.
std::string hmac_sha256_hex(const std::string& key, const std::string& message);

// Length-independent, byte-wise comparison for signatures.
bool secure_equals(const std::string& a, const std::string& b);

// 16 random bytes, Base64 encoded. Used for node ids; never derived from
// hardware identifiers or user data.
std::string random_token();

// Zero-mean Gaussian noise (Box-Muller), clamped so the result stays >= 0.
// `sigma <= 0` returns the value unchanged.
double add_gaussian_noise(double value, double sigma);

// Rounds to the nearest multiple of `step` (step <= 1 returns the input).
int round_to_step(int value, int step);

}  // namespace Vectors
}  // namespace Jarvis
