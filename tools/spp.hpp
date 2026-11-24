#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dana::tools {

    struct [[nodiscard]] spp_options {
        std::vector<std::string>   files;
        std::vector<std::string>   include_dirs;
        std::optional<std::string> language; // "c" or "c++"
        bool                       c_as_cxx = false;
        bool                       cxx_as_c = false;
        bool                       verbose  = false;
        unsigned                   jobs     = 4;
    };

    // Expand a C/C++ symbol into its full declaration/definition source using
    // clang's AST printer. Files are discovered with `git grep` when none are
    // given, headers are wrapped in a temporary translation unit, and duplicate
    // blocks are removed. The result ends with a newline when non-empty.
    [[nodiscard]] std::string expand_symbol(std::string_view function, spp_options const& options = {});

} // namespace dana::tools
