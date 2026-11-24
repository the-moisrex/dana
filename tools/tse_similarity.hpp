#pragma once

// Fuzzy string scoring used by the instrument resolver. Mirrors the
// rapidfuzz metrics the historical resolver relied on: `ratio` is the
// normalized Indel similarity and `wratio` the weighted combination.

#include <string_view>

namespace dana::tools::tse {

    // Normalized Indel similarity (0..100).
    [[nodiscard]] double ratio(std::string_view a, std::string_view b) noexcept;

    // Optimal-alignment ratio of the shorter string inside the longer one.
    [[nodiscard]] double partial_ratio(std::string_view a, std::string_view b) noexcept;

    // Weighted ratio combining ratio, token and partial variants (0..100).
    [[nodiscard]] double wratio(std::string_view a, std::string_view b) noexcept;

} // namespace dana::tools::tse
