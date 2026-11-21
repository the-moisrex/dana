#pragma once

#include "strings.hpp"

#include <initializer_list>
#include <string>
#include <string_view>

namespace dana::git {

    // Run a git command and return stdout with trailing newlines stripped.
    std::string run(text::string_views args);

    std::string run(std::initializer_list<char const*> args);

    // Annotate ordinary unified diffs using destination-file line numbers.
    // Combined diffs (multiple prefix columns) are left unchanged.
    std::string add_line_numbers_to_diff(std::string_view diff);

    // Keep/drop whole-file blocks of a unified diff.
    //   * `includes` non-empty => only blocks whose path matches one of them are kept.
    //   * `excludes`          => blocks whose path matches any of them are dropped.
    // Paths are matched both verbatim and by basename, using `detail::glob_match`.
    std::string filter_diff_by_path(std::string_view diff, text::string_views includes, text::string_views excludes);
} // namespace dana::git
