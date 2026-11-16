#pragma once

#include "provider.hpp"
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace prompt::ai {

bool register_provider(std::unique_ptr<provider> value);

provider* find_provider(std::string_view name);
std::vector<provider*> all_providers();

// `deepseek` -> `dseek`, `lmstudio` -> `lms`, `oai` -> `openai`, `pplx` ->
// `perplexity`; nullopt for an unknown name.
std::optional<std::string> canonical_provider(std::string_view name);

} // namespace prompt::ai
