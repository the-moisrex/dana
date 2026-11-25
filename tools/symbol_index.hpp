#pragma once

#include "../core/strings.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace dana::tools {

    // One declaration discovered by ctags or the lightweight C/C++ scanner. Paths
    // are repository-relative whenever the index was built from a repository.
    struct [[nodiscard]] symbol_entry {
        std::string name;
        std::string path;
        std::size_t line     = 0; // 1-based declaration line
        std::size_t end_line = 0; // 1-based closing `}`/`;` (0 when unknown)
        std::string kind;         // function, class, struct, enum, variable, ...
        std::string signature;
    };

    struct [[nodiscard]] dependency_options {
        bool        transitive  = false;
        std::size_t max_symbols = 32;
        std::size_t max_depth   = 8;
    };

    // The project's C/C++ symbol index. Built from ctags (when available) and the
    // `compress::cpp::scan` scanner in parallel, then cached under
    // `$XDG_CACHE_HOME/prompt/symbols`. Only files whose mtime/size changed are
    // rescanned, so repeated completions are cheap.
    [[nodiscard]] std::vector<symbol_entry> const& symbol_index(std::filesystem::path const& root);

    // Drop the in-process memo so the next `symbol_index` call revalidates the
    // on-disk cache. Only needed by long-lived callers (tests) that change files
    // between lookups; a normal prompt run builds the index once.
    void reset_symbol_index();

    [[nodiscard]] std::optional<symbol_entry> find_symbol(std::string_view name, std::filesystem::path const& root);

    // Every indexed declaration of `name` (overloads, a header declaration and its
    // source definition, ...). Empty when the name is unknown.
    [[nodiscard]] std::vector<symbol_entry> find_symbols(std::string_view name, std::filesystem::path const& root);

    // Fuzzy symbol candidates for `.spp <prefix>`, one `name<TAB>path:line` per
    // line. Names already present in `prior` are omitted.
    void complete_symbols(
      std::string_view             prefix,
      text::string_views           prior,
      std::filesystem::path const& root,
      std::ostream&                out);

    // Symbols referenced by `target`'s source range. The scan is heuristic: it
    // resolves identifiers in the range against the index, so it can over-include.
    [[nodiscard]] std::vector<symbol_entry>
    dependencies(symbol_entry const& target, std::filesystem::path const& root, dependency_options const& options = {});

    // Symbols whose source range contains a reference to `target`.
    [[nodiscard]] std::vector<symbol_entry>
    callers(symbol_entry const& target, std::filesystem::path const& root, dependency_options const& options = {});

} // namespace dana::tools
