#pragma once

#include <string_view>

namespace dana::glob {

    // Tiny shell-style glob matcher supporting '*' and '?'. `*` matches any
    // sequence, including '/', so both `*.cpp` (a basename pattern) and
    // `src/*` (a path pattern) are useful; a caller that wants a basename match
    // passes the basename. Linear time, no allocation.
    inline bool match(std::string_view pattern, std::string_view text) {
        std::size_t p      = 0;
        std::size_t t      = 0;
        std::size_t star_p = std::string_view::npos;
        std::size_t star_t = 0;
        while (t < text.size()) {
            if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
                ++p;
                ++t;
            } else if (p < pattern.size() && pattern[p] == '*') {
                star_p = p++;
                star_t = t;
            } else if (star_p != std::string_view::npos) {
                p = star_p + 1;
                t = ++star_t;
            } else {
                return false;
            }
        }
        while (p < pattern.size() && pattern[p] == '*') {
            ++p;
        }
        return p == pattern.size();
    }

    // True when `pattern` contains a glob metacharacter.
    inline bool has_wildcard(std::string_view pattern) {
        return pattern.find_first_of("*?") != std::string_view::npos;
    }

} // namespace dana::glob
