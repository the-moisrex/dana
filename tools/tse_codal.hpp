#pragma once

// Codal.ir letters: recent announcements, the income-statement and
// balance-sheet rows embedded in decision reports, monthly fund portfolio
// reports, and the ratios derived from those extracted rows. Results are
// cached under `${XDG_CACHE_HOME:-~/.cache}/tse/codal` (or `CODAL_CACHE_DIR`)
// with a six-hour freshness window, so a live fetch is skipped while the
// entry is young and a stale entry still serves when the fetch keeps failing.

#include <string>

namespace dana::tools::tse {

    struct [[nodiscard]] codal_options {
        // Recent announcements to keep; 0 means the command-line default of 5.
        int  top     = 0;
        bool fund    = false;
        bool refresh = false;
    };

    // The prompt-ready Markdown dataset `tse.codal snapshot <symbol> --markdown`
    // prints, including its trailing newline. Throws `error` when Codal cannot
    // be reached and nothing usable is cached.
    [[nodiscard]] std::string codal_snapshot(std::string const& symbol, codal_options const& options = {});

} // namespace dana::tools::tse
