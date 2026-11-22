#pragma once

// ChatGPT's sentinel gate: the requirements token (a base64 browser
// fingerprint), the `zvt` proof-of-work and the turnstile `dx` stack machine.
// A faithful port of `bin/ai/sentinel.py`; self-contained (base64 and the hash
// live here, so the gate is a plain function of the challenge bytes).

#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace dana::ai::sentinel {

    // The sentinel gate rejected us, or a puzzle could not be solved.
    class error : public std::runtime_error {
      public:
        using std::runtime_error::runtime_error;
    };

    /// `gAAAAAC` — the requirements token prefix.
    inline constexpr std::string_view kRequestPrefix = "gAAAAAC";
    /// `gAAAAAB` — the proof token prefix.
    inline constexpr std::string_view kProofPrefix   = "gAAAAAB";
    /// The proof-of-work hash the server currently verifies.
    inline constexpr std::string_view kProofHash     = "zvt";

    // The 25-element array the client serialises into the `p` token.
    nlohmann::json build_fingerprint(
      std::string_view           user_agent,
      std::optional<std::string> script_source,
      std::string_view           data_build);

    // `gAAAAAC` + base64(json(fingerprint)) — the value sent as `{"p": …}`.
    std::string requirements_token(nlohmann::json const& fingerprint);

    // The client's current proof-of-work hash (FNV-1a + murmur finaliser), as eight
    // lowercase hex digits.
    std::string zvt(std::string_view text);

    // The `gAAAAAB…` proof token for `seed` / `difficulty`.
    std::string solve_proof(
      std::string_view    seed,
      std::string_view    difficulty,
      nlohmann::json const& fingerprint,
      std::string_view      algo  = kProofHash,
      int                   limit = 500'000);

    // Decode and run a `dx` program; return its base64 result (or nullopt).
    std::optional<std::string> solve_turnstile(std::string_view dx, std::string_view key);

    // Standard base64 (the VM's atob/btoa and the token encoding).
    std::string                encode_base64(std::string_view data);
    std::optional<std::string> decode_base64(std::string_view data);

} // namespace dana::ai::sentinel
