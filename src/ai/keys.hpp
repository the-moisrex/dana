#pragma once

// Resolve an API key for an OpenAI-compatible provider.  Lookup order:
//   1. an explicit --api-key;
//   2. the provider's environment variables;
//   3. a .env file ($AI_ENV_FILE, then .env/.env.local walking up to the git
//      root/home, then $XDG_CONFIG_HOME/ai/.env and ~/.env);
//   4. the installed opencode's credentials (opencode2 auth export, falling
//      back to $XDG_DATA_HOME/opencode/auth.json);
//   5. options.apiKey of a provider in opencode.jsonc.

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace prompt::ai::keys {

struct resolved {
    std::string key;
    std::string source;
};

// A redacted preview (never the whole secret).
std::string mask(std::string_view value);

resolved resolve(std::string_view provider, std::span<std::string const> env_names = {},
                 std::optional<std::string> explicit_key = std::nullopt);

// (source, masked key) for a diagnostic listing.
std::pair<std::string, std::string> describe(std::string_view provider, std::span<std::string const> env_names = {});

} // namespace prompt::ai::keys
