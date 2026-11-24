#pragma once

// Shared plumbing for the native TSETMC tools: HTTP, JSON row helpers, the
// Jalali calendar and identifier normalisation. Every `tools/tse_*` module
// builds on this instead of re-implementing the same parsing.

#include <cstdint>
#include <initializer_list>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace dana::tools::tse {

    using json = nlohmann::ordered_json;

    struct [[nodiscard]] error : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    inline constexpr std::string_view tsetmc_base = "https://cdn.tsetmc.com/api";
    inline constexpr std::string_view webgw_base =
      "https://webgw.tse.ir/InstrumentProvider/api/v1/Instrument/LiveInstrumentByIdQuery/fa";
    inline constexpr std::string_view world_bank_base = "https://api.worldbank.org/v2/country/IRN/indicator/";
    inline constexpr std::string_view user_agent =
      "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";

    // ------------------------------------------------------------------ values

    [[nodiscard]] std::string normal(std::string_view text);

    [[nodiscard]] json pick(json const& row, std::initializer_list<std::string_view> names);

    [[nodiscard]] json number(json const& value);

    [[nodiscard]] json price(json const& value);

    [[nodiscard]] json value_of(json const& row, std::initializer_list<std::string_view> names);

    [[nodiscard]] json unwrap(json const& payload, std::string_view key);

    [[nodiscard]] std::optional<double> as_double(json const& value);

    [[nodiscard]] std::optional<long long> as_int(json const& value);

    [[nodiscard]] json jnum(std::optional<double> value);

    [[nodiscard]] json jint(std::optional<long long> value);

    [[nodiscard]] json jint(long long value);

    [[nodiscard]] json jnum(double value);

    // Python's `round(x, digits)` for the JSON numbers this code produces.
    [[nodiscard]] double round_to(double value, int digits);

    // Python's `str()` for a JSON scalar, used by the CSV renderers.
    [[nodiscard]] std::string scalar_text(json const& value);

    // ------------------------------------------------------------------ calendar

    struct [[nodiscard]] jdate {
        int year  = 0;
        int month = 0;
        int day   = 0;
    };

    struct [[nodiscard]] gdate {
        int year  = 0;
        int month = 0;
        int day   = 0;
    };

    [[nodiscard]] jdate gregorian_to_jalali(int gy, int gm, int gd) noexcept;

    [[nodiscard]] gdate jalali_to_gregorian(int jy, int jm, int jd) noexcept;

    // A compact Jalali `YYYYMMDD` (file names).
    [[nodiscard]] std::string jalali_day(std::int64_t unix_seconds);

    // A Jalali `YYYY/MM/DD HH:MM:SS` stamp for a Unix timestamp.
    [[nodiscard]] std::string jalali_stamp(std::int64_t unix_seconds);

    // TSETMC `dEven`-style Gregorian YYYYMMDD (or YYYY-MM-DD) -> Jalali YYYY/MM/DD.
    [[nodiscard]] std::string trade_date(json const& value);

    // A Jalali `HHMMSS`-style trading time -> `HH:MM:SS`.
    [[nodiscard]] std::string trade_time(json const& value);

    // Parses a Jalali `YYYY/MM/DD HH:MM:SS` stamp or an ISO timestamp into a
    // Unix timestamp, or nullopt when it does not parse.
    [[nodiscard]] std::optional<std::int64_t> parse_moment(std::string_view text);

    [[nodiscard]] std::int64_t tehran_now() noexcept;

    [[nodiscard]] bool market_session_open(std::int64_t unix_seconds) noexcept;

    // ------------------------------------------------------------------ http

    class [[nodiscard]] client {
      public:
        client() = default;

        [[nodiscard]] std::string
        get(std::string const& url, std::string_view accept = "application/json", std::size_t max_bytes = 6'000'000);

        [[nodiscard]] json json_get(std::string const& url, std::size_t max_bytes = 6'000'000);

        [[nodiscard]] json api(std::string_view path, std::string_view key);

        // Kept for parity with the streaming fetchers: libcurl manages its own
        // connection pool, so there is nothing to drop.
        void drop(std::string const& url) noexcept {
            (void) url;
        }
    };

    // ------------------------------------------------------------------ identifiers

    [[nodiscard]] bool is_isin(std::string_view text) noexcept;

    [[nodiscard]] std::string normalize_input(std::string_view symbol);

    [[nodiscard]] json resolve_isin(client& http, std::string const& isin);

    [[nodiscard]] std::vector<json> search(client& http, std::string_view query);

    // Merge successive market-watch payloads until the kind codes (and the
    // order book, when requested) are present.
    [[nodiscard]] std::vector<json> fetch_market_watch(client& http, bool with_book = false);

    [[nodiscard]] std::string url_encode(std::string_view text);

    [[nodiscard]] std::string url_decode_plus(std::string_view text);

} // namespace dana::tools::tse
