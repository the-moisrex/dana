#pragma once

// Instrument resolution over a cached TSETMC market-watch index: exact
// Persian/finglish/ISIN/insCode matching, fuzzy scoring and shell completion.
// The instrument records are JSON objects so the cache file and the machine
// output keep the same shape the prompts already consume.

#include "tse_common.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dana::tools::tse {

    inline constexpr int         cache_version      = 4;
    inline constexpr int         default_ttl_days   = 7;
    inline constexpr double      default_min_score  = 62.0;
    inline constexpr double      fuzzy_accept_score = 76.0;
    inline constexpr double      ambiguity_margin   = 4.0;
    inline constexpr double      finglish_key_score = 85.0;
    inline constexpr std::size_t finglish_query_min = 3;

    struct [[nodiscard]] not_found : error {
        using error::error;
    };

    struct [[nodiscard]] ambiguous_error : error {
        explicit ambiguous_error(std::string const& message, std::vector<json> list)
          : error(message),
            candidates(std::move(list)) {}

        std::vector<json> candidates;
    };

    struct [[nodiscard]] resolve_options {
        int                      ttl_days   = default_ttl_days;
        bool                     refresh    = false;
        bool                     no_cache   = false;
        bool                     offline    = false;
        bool                     allow_live = true;
        double                   min_score  = default_min_score;
        std::size_t              limit      = 0;
        std::vector<std::string> types;
        std::vector<std::string> markets;
    };

    struct [[nodiscard]] index_info {
        std::vector<json> instruments;
        json              meta;
    };

    [[nodiscard]] std::filesystem::path default_cache_path();

    [[nodiscard]] std::optional<json> read_cache(std::filesystem::path const& path);

    [[nodiscard]] bool write_cache(
      std::filesystem::path const&      path,
      std::vector<json> const&          instruments,
      std::string_view                  source,
      std::optional<std::string> const& fetched_at = std::nullopt);

    [[nodiscard]] index_info get_index(
      client&                              http,
      resolve_options const&               options    = {},
      std::optional<std::filesystem::path> cache_path = std::nullopt);

    [[nodiscard]] std::vector<json> resolve_query(
      client&                              http,
      std::string const&                   query,
      resolve_options const&               options    = {},
      std::optional<std::filesystem::path> cache_path = std::nullopt,
      std::optional<std::vector<json>>     index      = std::nullopt);

    [[nodiscard]] json resolve_one(
      client&                              http,
      std::string const&                   query,
      resolve_options const&               options    = {},
      std::optional<std::filesystem::path> cache_path = std::nullopt);

    [[nodiscard]] std::vector<std::pair<std::string, std::string>>
    complete_query(std::string const& prefix, std::vector<json> const& index, std::size_t limit = 60);

    [[nodiscard]] std::string token_for(json const& instrument);

    [[nodiscard]] std::string description_for(json const& instrument);

    [[nodiscard]] json public_instrument(json const& instrument);

    [[nodiscard]] std::string format_candidates(std::vector<json> const& candidates);

    // A candidate is `{instrument, tier, score, market_rank, matched}`.
    [[nodiscard]] json make_candidate(json instrument, int tier, double score, std::string_view matched);

    [[nodiscard]] bool choose(std::vector<json> const& candidates, json& best);

} // namespace dana::tools::tse
