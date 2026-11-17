#pragma once

// DeepSeek's proof-of-work gate (`DeepSeekHashV1`).  The site hands out a
// challenge and expects the nonce whose digest equals it; the solved nonce is
// packed into the base64 `x-ds-pow-response` header.  The hash itself lives in
// `crypto/keccak.hpp`.

#include <stdexcept>
#include <string>
#include <string_view>

namespace dana::ai::pow {

    // The challenge could not be solved within the nonce budget.
    struct [[nodiscard]] error :  std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    // The DeepSeekHashV1 digest of `data` as lowercase hex (the shape the
    // challenge itself uses).
    std::string deepseek_hash_hex(std::string_view data);

    // The nonce in `range(difficulty)` whose digest equals the hex `challenge`.
    // `workers` overrides the CPU count (0 = auto, `AI_POW_WORKERS` first).
    long long solve_deepseek(
      std::string const& salt,
      long long          expire_at,
      long long          difficulty,
      std::string const& challenge,
      int                workers = 0);

    // The base64 `x-ds-pow-response` header value for a solved challenge.
    std::string deepseek_response(
      std::string const& salt,
      std::string const& challenge,
      std::string const& signature,
      long long          answer,
      std::string const& target_path);

} // namespace dana::ai::pow
