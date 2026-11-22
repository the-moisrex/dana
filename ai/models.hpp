#pragma once

// One table for every AI provider's model list and friendly-name aliases.
//
// Add, remove or update a model here: the providers read their `provider_info`
// from this file, `--list` prints it and `-m`/`--model` shell completion offers
// it. Nothing else in the tree holds a model list.
//
// `models` are the names `-m` offers and `--list` prints. `aliases` map a
// friendly label to the id the site expects; an empty target means "let the
// site pick its default" (Claude's `auto`/`default`). API providers whose
// models are discovered from the endpoint (`lms`, `openai`, `openrouter`) have
// empty lists.

#include "../core/strings.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dana::ai::models {

    struct [[nodiscard]] alias {
        std::string_view from;
        std::string_view to;
    };

    struct [[nodiscard]] entry {
        /// Canonical provider name (`dseek`, not `deepseek`).
        std::string_view       provider;
        std::string_view       default_model;
        text::string_views     models;
        std::span<alias const> aliases;
    };

    /// Every provider, in catalog order.
    std::span<entry const> all();

    /// The entry for a canonical (or alias) provider name, or nullptr.
    entry const* find(std::string_view provider);

    /// The provider's models as a vector (empty for an unknown provider).
    std::vector<std::string> list(std::string_view provider);

    /// The provider's default model (empty for an unknown provider).
    std::string_view default_model(std::string_view provider);

    /// The site id a friendly name maps to. `nullopt` means there is no alias
    /// entry; an empty value means the alias explicitly asks for the site default.
    std::optional<std::string_view> resolve_alias(std::string_view provider, std::string_view name);

} // namespace dana::ai::models
