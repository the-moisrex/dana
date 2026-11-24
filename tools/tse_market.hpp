#pragma once

// The full TSETMC dataset collector and its Markdown renderer: identity, quote,
// order book, money flow, history (raw and adjusted), technicals, corrections,
// fundamentals, market context and breadth.

#include "tse_common.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace dana::tools::tse {

    struct [[nodiscard]] market_options {
        int                   days     = 0;
        bool                  adjusted = true;
        std::set<std::string> skip;
    };

    [[nodiscard]] json resolve_symbol(client& http, std::string const& symbol);

    [[nodiscard]] json identity(client& http, std::string const& code);

    [[nodiscard]] json quote(client& http, std::string const& code);

    [[nodiscard]] json book(client& http, std::string const& code);

    [[nodiscard]] json flow(client& http, std::string const& code);

    [[nodiscard]] json client_type_history(client& http, std::string const& code);

    [[nodiscard]] json price_adjustments(client& http, std::string const& code);

    [[nodiscard]] json share_changes(client& http, std::string const& code);

    [[nodiscard]] json history(client& http, std::string const& code, int days, bool adjusted, json& errors);

    [[nodiscard]] json technicals(json const& rows);

    [[nodiscard]] json corrections(json const& rows);

    [[nodiscard]] json
    info(client& http, std::string const& code, json const& ident, json const& current, json& errors);

    [[nodiscard]] json economy(client& http);

    [[nodiscard]] json instrument_statistics(client& http, std::string const& code);

    [[nodiscard]] json market_overview(client& http);

    [[nodiscard]] json market_breadth(client& http, std::optional<std::string> const& code = std::nullopt);

    [[nodiscard]] json indexes(client& http);

    [[nodiscard]] json collect(client& http, std::string const& symbol, market_options const& options = {});

    [[nodiscard]] std::string markdown(json const& data);

    [[nodiscard]] std::string snapshot_meta_text(json const& meta, json const& data);

    // Reads a `.stock` snapshot (or stdin when `path` is "-") and returns the
    // four header lines (`snapshot_header`) or the rendered Markdown
    // (`snapshot_markdown`). Throws `error` with the historical wording.
    [[nodiscard]] std::string snapshot_header(std::filesystem::path const& path);

    [[nodiscard]] std::string snapshot_markdown(std::filesystem::path const& path);

    [[nodiscard]] std::string all_markdown(client& http, std::string const& symbol, market_options const& options = {});

} // namespace dana::tools::tse
