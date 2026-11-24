#pragma once

// The TSETMC screener: one bulk market-watch request (or a saved payload on
// stdin) parsed into instrument records, filtered/ranked locally by the CLI
// criteria and rendered as a table, JSON, JSONL, CSV, Markdown or a `.tse`
// snapshot.
//
// The argument surface mirrors the historical command line so a prompt can
// forward its arguments verbatim.

#include <string>
#include <vector>

namespace dana::tools::tse {

    struct [[nodiscard]] find_output {
        std::string out;
        std::string err;
        int         code = 0;
    };

    // Runs the screener and keeps stdout, stderr and the exit status apart.
    // `stdin_data` may carry a raw market-watch payload (an array of rows or an
    // object with a `marketwatch` array); when empty the live dataset is fetched.
    [[nodiscard]] find_output find_run_full(std::vector<std::string> const& args, std::string const& stdin_data = {});

    // Runs the screener and returns stdout. Throws `error` (carrying the
    // diagnostic message) when the run fails.
    [[nodiscard]] std::string find_run(std::vector<std::string> const& args, std::string const& stdin_data = {});

} // namespace dana::tools::tse
