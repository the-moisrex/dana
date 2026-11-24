#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dana::tools {

    struct [[nodiscard]] paths_options {
        bool                                 check_exists   = true;
        bool                                 strip_location = false;
        std::filesystem::path                base           = std::filesystem::current_path();
        std::optional<std::filesystem::path> repo_root;
    };

    // Extract existing file paths (optionally with :line or :line:column) from
    // compiler output, Markdown, unified patches and prose. Results preserve
    // first-seen order and are deduplicated by resolved identity and location.
    [[nodiscard]] std::vector<std::string> extract_paths(std::string_view input, paths_options const& options = {});

} // namespace dana::tools
