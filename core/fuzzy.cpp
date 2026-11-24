#include "fuzzy.hpp"

#include <string>

namespace dana::fuzzy {

    namespace {

        // Deliberately ASCII-only: predictable and independent of the process locale.
        std::string fold_ascii(std::string_view text) {
            std::string result;
            result.reserve(text.size());

            for (char const raw : text) {
                auto character = static_cast<unsigned char>(raw);
                if (character >= 'A' && character <= 'Z') {
                    character = static_cast<unsigned char>(character + ('a' - 'A'));
                }
                result.push_back(static_cast<char>(character));
            }

            return result;
        }

        // Counts skipped characters between matched characters, not before or after.
        // Linear time; no dynamic-programming matrix or external fuzzy finder.
        std::optional<std::size_t> subsequence_gaps(std::string_view text, std::string_view query) {
            std::size_t next     = 0;
            std::size_t previous = 0;
            std::size_t gaps     = 0;

            for (std::size_t i = 0; i < query.size(); ++i) {
                auto const found = text.find(query.at(i), next);
                if (found == std::string_view::npos) {
                    return std::nullopt;
                }

                if (i != 0) {
                    gaps += found - previous - 1;
                }

                previous = found;
                next     = found + 1;
            }

            return gaps;
        }

    } // namespace

    std::optional<rank> rank_path(std::string_view relative, std::string_view query) {
        std::string const folded       = fold_ascii(relative);
        std::string const folded_query = fold_ascii(query);

        auto const slash  = relative.find_last_of('/');
        auto const offset = slash == std::string_view::npos ? 0 : slash + 1;

        auto const filename        = relative.substr(offset);
        auto const folded_filename = std::string_view(folded).substr(offset);

        if (folded == folded_query) {
            return rank{
              .kind         = match_kind::exact_path,
              .case_penalty = relative == query ? 0 : 1,
              .gaps         = 0,
              .length       = relative.size(),
            };
        }

        if (folded_filename == folded_query) {
            return rank{
              .kind         = match_kind::exact_name,
              .case_penalty = filename == query ? 0 : 1,
              .gaps         = 0,
              .length       = relative.size(),
            };
        }

        if (folded_filename.contains(folded_query)) {
            return rank{
              .kind         = match_kind::name_substring,
              .case_penalty = filename.contains(query) ? 0 : 1,
              .gaps         = 0,
              .length       = relative.size(),
            };
        }

        if (folded.contains(folded_query)) {
            return rank{
              .kind         = match_kind::path_substring,
              .case_penalty = relative.contains(query) ? 0 : 1,
              .gaps         = 0,
              .length       = relative.size(),
            };
        }

        if (auto const gaps = subsequence_gaps(folded_filename, folded_query)) {
            return rank{
              .kind         = match_kind::name_subsequence,
              .case_penalty = 0,
              .gaps         = *gaps,
              .length       = relative.size(),
            };
        }

        if (auto const gaps = subsequence_gaps(folded, folded_query)) {
            return rank{
              .kind         = match_kind::path_subsequence,
              .case_penalty = 0,
              .gaps         = *gaps,
              .length       = relative.size(),
            };
        }

        return std::nullopt;
    }

} // namespace dana::fuzzy
