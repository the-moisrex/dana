#include "./glob.hpp"

namespace dana::glob {

    bool match(std::string_view const pattern, std::string_view const text) noexcept try {
        // Fast path: no wildcards means an exact match.
        if (!has_wildcard(pattern)) {
            return pattern == text;
        }

        std::size_t pattern_pos       = 0;
        std::size_t text_pos          = 0;
        std::size_t retry_pattern_pos = std::string_view::npos;
        std::size_t retry_text_pos    = 0;

        while (text_pos < text.size()) {
            if (pattern_pos < pattern.size()) {
                char const pattern_char = pattern.at(pattern_pos);

                if (pattern_char == '*') {
                    // Collapse consecutive stars.
                    while (pattern_pos < pattern.size() && pattern.at(pattern_pos) == '*') {
                        ++pattern_pos;
                    }

                    // Save the position after the stars for backtracking.
                    retry_pattern_pos = pattern_pos;
                    retry_text_pos    = text_pos;
                    continue;
                }

                if (pattern_char == '?' || pattern_char == text.at(text_pos)) {
                    ++pattern_pos;
                    ++text_pos;
                    continue;
                }
            }

            // On mismatch, let the most recent '*' consume one more character.
            if (retry_pattern_pos == std::string_view::npos || retry_text_pos == text.size()) {
                return false;
            }

            ++retry_text_pos;
            text_pos    = retry_text_pos;
            pattern_pos = retry_pattern_pos;
        }

        // Ignore trailing stars.
        while (pattern_pos < pattern.size() && pattern.at(pattern_pos) == '*') {
            ++pattern_pos;
        }

        return pattern_pos == pattern.size();
    } catch (...) {
        return false;
    }

    bool has_wildcard(std::string_view const pattern) noexcept {
        return pattern.find_first_of("*?") != std::string_view::npos;
    }

} // namespace dana::glob
