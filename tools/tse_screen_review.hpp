#pragma once

// Helpers for reviewing screener output: classifying piped text, converting
// JSON rows to CSV, and reading a saved screen snapshot's metadata. These are
// the native equivalents of the small text-processing snippets the review
// prompt used to shell out to.

#include "tse_common.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace dana::tools::tse {

    // One of "empty", "doc", "jsonl", "array", "table", "nl".
    [[nodiscard]] std::string classify_screen_text(std::string_view text);

    // Converts a screen document/array/JSONL payload to CSV with a header row.
    // Returns an empty string when there are no columns. Throws `error` with the
    // historical wording on malformed rows.
    [[nodiscard]] std::string screen_rows_to_csv(std::string_view text);

    struct screen_snapshot_info {
        std::string screened;
        std::string counts;
        std::string command;
    };

    // Reads `meta.screened_at`, a matched/total count line and the command from a
    // saved `.tse` screen snapshot. Throws `error` when the payload is not one.
    [[nodiscard]] screen_snapshot_info screen_snapshot_meta(std::string_view text);

    // Number of rows in a JSON array payload. Throws `error` on malformed JSON.
    [[nodiscard]] std::size_t screen_row_count(std::string_view text);

} // namespace dana::tools::tse
