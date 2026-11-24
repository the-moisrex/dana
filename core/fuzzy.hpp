#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <tuple>

namespace dana::fuzzy {

    // The match quality buckets, best first. A basename match beats a path match, and
    // a contiguous substring beats a subsequence.
    enum struct match_kind : std::uint8_t {
        exact_path,
        exact_name,
        name_substring,
        path_substring,
        name_subsequence,
        path_subsequence,
    };

    // How well a path matches a query. Lower keys rank first; `length` breaks ties
    // toward the shorter path.
    struct [[nodiscard]] rank {
        match_kind  kind;
        int         case_penalty;
        std::size_t gaps;
        std::size_t length;
    };

    [[nodiscard]] inline auto rank_key(rank const& value) noexcept {
        return std::tuple{value.kind, value.case_penalty, value.gaps, value.length};
    }

    // Rank `relative` (a `/`-separated path) against `query`, or nullopt when the
    // query is not a subsequence of it. Case-insensitive with an ASCII fold, so the
    // result does not depend on the process locale.
    [[nodiscard]] std::optional<rank> rank_path(std::string_view relative, std::string_view query);

} // namespace dana::fuzzy
