#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dana::tools {

    // Where a symbol was found: a path (repository-relative when the caller works
    // from a repository) and the 1-based line of its declaration.
    struct [[nodiscard]] symbol_location {
        std::string path;
        std::size_t line = 0;
    };

    // One file's contribution to an expansion: the block clang printed and the
    // declaration line it starts at, so the caller can number it correctly.
    struct [[nodiscard]] expansion_part {
        symbol_location location;
        std::string     text;
    };

    // Every file that contributed to an expansion, each with its own block.
    struct [[nodiscard]] expansion {
        std::vector<expansion_part> parts;
    };

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
    // blocks are removed. The result ends with a newline when non-empty, and
    // `locations` names the declarations that produced it.
    [[nodiscard]] expansion expand_symbol(std::string_view function, spp_options const& options = {});

} // namespace dana::tools
