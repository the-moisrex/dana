#pragma once

#include <string_view>

namespace dana::glob {

    /**
     * Matches text against a shell-style glob pattern.
     *
     * Supports `*`, which matches any sequence of characters (including `/`),
     * and `?`, which matches exactly one character. Other characters are matched
     * literally. Pass a basename as `text` when the pattern should match only a
     * filename, or pass a path when the pattern should match a path.
     *
     * @param pattern A `std::string_view` containing the glob pattern.
     * @param text A `std::string_view` containing the text to match.
     * @return `true` if the entire text matches the pattern; otherwise `false`.
     * @throws No exceptions; this function is `noexcept`.
     */
    [[nodiscard]] bool match(std::string_view pattern, std::string_view text) noexcept;

    /**
     * Checks whether a pattern contains a supported glob metacharacter.
     *
     * @param pattern A `std::string_view` containing the pattern to inspect.
     * @return `true` if the pattern contains `*` or `?`; otherwise `false`.
     * @throws No exceptions; this function is `noexcept`.
     */
    [[nodiscard]] bool has_wildcard(std::string_view pattern) noexcept;

} // namespace dana::glob
