#pragma once

// The one cryptographic primitive the sites' anti-bot gates need: DeepSeek's
// `DeepSeekHashV1`, which is SHA3-256 with the first Keccak-f[1600] round
// skipped.  The port lives here (self-contained, no OpenSSL) so the proof-of-
// work solver stays a plain function of the challenge bytes.

#include <array>
#include <cstdint>
#include <string_view>

namespace prompt::ai::crypto {

// The 32-byte DeepSeekHashV1 digest of `data`.
std::array<std::uint8_t, 32> deepseek_hash_v1(std::string_view data);

} // namespace prompt::ai::crypto
