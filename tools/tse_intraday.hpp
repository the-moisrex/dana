#pragma once

// Intraday (per-minute) OHLCV bars for a TSETMC instrument, rebuilt from the
// per-trade tape and rendered as the Markdown dataset the `tse.intraday`
// prompt prints.

#include <optional>
#include <string>

namespace dana::tools::tse {

    struct [[nodiscard]] intraday_options {
        int                        days = 0; // 0 means the default (one session)
        std::optional<std::string> date;
        std::optional<std::string> from;
        std::optional<std::string> to;
        std::optional<int>         interval; // nullopt means "auto"
        bool                       depth   = false;
        bool                       no_live = false;
    };

    // Returns the Markdown dataset exactly as `tse.intraday` prints it for the
    // symbol. Throws `dana::tools::tse::error` for argument and fetch failures.
    [[nodiscard]] std::string intraday_markdown(std::string const& symbol, intraday_options const& options = {});

} // namespace dana::tools::tse
