#pragma once

// Turn an oversized prompt into an uploaded text file.  Every site caps a
// single chat message, and the cap is a token count, so the usable character
// count depends on the language.  A prompt above a provider's inline budget is
// written to a temporary .txt and sent through the same upload path as --file.

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace dana::ai {

    /// Fallback inline budget for a provider that does not declare one.
    inline constexpr std::size_t kDefaultInlineLimit = 64UL * 1024UL;

    /// The prompt size the budgets are compared against (UTF-8 bytes).
    std::size_t prompt_size(std::string_view text);

    /// Render a byte count for a human-readable notice.
    std::string human_size(std::size_t count);

    /// The inline budget for a provider, honouring an explicit override.
    std::size_t limit_for(std::size_t provider_limit, std::optional<std::int64_t> override);

    /// Whether the prompt should be uploaded as a file.  `mode` is nullopt
    /// (automatic), true (always) or false (never).
    bool should_attach(std::string_view            prompt,
                       std::size_t                 inline_limit,
                       std::optional<bool>         mode,
                       std::optional<std::int64_t> override);

    /// Write the prompt to a temporary UTF-8 .txt and return its path.
    std::filesystem::path write_prompt_file(std::string_view prompt);

    /// Delete a temporary prompt file (best effort, but reported).
    void remove_prompt_file(std::filesystem::path const& path);

    /// The short message that points the model at the attached prompt file.
    std::string prompt_file_reference(std::filesystem::path const& path, std::size_t count);

    /// True when the message is a site complaining about the request size.
    bool looks_too_large(std::string_view message);

    /// Append a concrete remedy to a site's size-limit error.
    [[nodiscard]] std::string annotate_error(
      std::string_view            message,
      std::string_view            provider,
      std::string_view            label,
      std::string_view            prompt,
      bool                        used_file,
      bool                        disabled,
      bool                        has_upload,
      bool                        auto_attach,
      std::optional<std::int64_t> limit);

} // namespace dana::ai
