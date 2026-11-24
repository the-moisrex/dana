#include "tse_intraday.hpp"

#include "tse_common.hpp"
#include "tse_resolver.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <format>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace dana::tools::tse {

    namespace {

        // ------------------------------------------------------------------ constants

        constexpr std::size_t        max_bytes                  = std::size_t{64} * 1024U * 1024U;
        constexpr std::array<int, 9> interval_ladder            = {1, 2, 3, 5, 6, 10, 15, 30, 60};
        constexpr int                default_max_rows           = 2000;
        constexpr int                default_collapse_threshold = 120;
        constexpr int                lag_warn_minutes           = 1;
        constexpr int                max_days                   = 30;
        constexpr int                depth_levels               = 5;
        constexpr int                depth_cutoff               = 123'000; // 12:30:00
        constexpr int                depth_cover_slack_minutes  = 15;
        constexpr int                market_pre_open            = 80'000;  // 08:00:00
        constexpr int                live_skew_seconds          = 180;     // 3 minutes
        constexpr std::array<int, 3> session_open_ranges        = {5, 15, 30};
        constexpr double             value_area_share           = 0.70;
        constexpr int                breadth_auto_max_days      = 5;

        constexpr std::array<std::string_view, 9> bar_column_names =
          {"from", "to", "open_rial", "high_rial", "low_rial", "close_rial", "vwap_rial", "volume_shares", "trades"};
        constexpr std::array<std::string_view, 8> depth_columns = {
          "bid_rial",
          "bid_shares",
          "ask_rial",
          "ask_shares",
          "spread_rial",
          "bid_depth_shares",
          "ask_depth_shares",
          "depth_imbalance_pct"};
        constexpr std::array<std::string_view, 7> extra_names =
          {"book", "orders", "opening", "stats", "index", "breadth", "flow"};

        // ------------------------------------------------------------------ formatting

        [[nodiscard]] std::string group_digits(std::string text) {
            std::string sign;
            if (!text.empty() && (text.front() == '-' || text.front() == '+')) {
                sign = text.substr(0, 1);
                text.erase(0, 1);
            }
            auto const  dot       = text.find('.');
            auto const  int_part  = dot == std::string::npos ? text : text.substr(0, dot);
            auto const  frac_part = dot == std::string::npos ? std::string{} : text.substr(dot);
            std::string out;
            out.reserve(int_part.size() + int_part.size() / 3 + frac_part.size() + sign.size());
            int count = 0;
            for (auto it = int_part.rbegin(); it != int_part.rend(); ++it) {
                if (count != 0 && count % 3 == 0) {
                    out.push_back(',');
                }
                out.push_back(*it);
                ++count;
            }
            std::reverse(out.begin(), out.end());
            return sign + out + frac_part;
        }

        [[nodiscard]] std::string fmt_int(long long value) {
            return group_digits(std::format("{}", value));
        }

        [[nodiscard]] std::string fmt_signed(long long value) {
            return group_digits(std::format("{:+}", value));
        }

        [[nodiscard]] std::string fmt_fixed(double value, int digits) {
            std::array<char, 1024> buffer{};
            auto const             result =
              std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::fixed, digits);
            return group_digits(std::string(buffer.data(), result.ptr));
        }

        [[nodiscard]] std::string fmt_signed_fixed(double value, int digits) {
            auto text = fmt_fixed(value, digits);
            if (!std::signbit(value)) {
                text.insert(text.begin(), '+');
            }
            return text;
        }

        [[nodiscard]] std::string fmt_whole(double value) {
            return group_digits(std::format("{:.0f}", value));
        }

        [[nodiscard]] std::string fmt_minute(int minute) {
            return std::format("{:02d}:{:02d}", minute / 60, minute % 60);
        }

        [[nodiscard]] std::string format_shares(std::optional<double> count) {
            if (!count) {
                return "?";
            }
            double const magnitude = std::abs(*count);

            struct scale {
                double           size;
                std::string_view suffix;
            };

            static constexpr std::array<scale, 3> scales = {
              {{1e9, "bn"}, {1e6, "m"}, {1e3, "k"}}
            };
            for (auto const& entry : scales) {
                if (magnitude >= entry.size) {
                    return fmt_fixed(*count / entry.size, 2) + " " + std::string(entry.suffix);
                }
            }
            return fmt_whole(*count);
        }

        [[nodiscard]] std::string format_rial(std::optional<double> amount) {
            if (!amount) {
                return "?";
            }
            double const magnitude = std::abs(*amount);

            struct scale {
                double           size;
                std::string_view suffix;
            };

            static constexpr std::array<scale, 4> scales = {
              {{1e15, "e15"}, {1e12, "T"}, {1e9, "bn"}, {1e6, "m"}}
            };
            for (auto const& entry : scales) {
                if (magnitude >= entry.size) {
                    return fmt_fixed(*amount / entry.size, 2) + std::string(entry.suffix);
                }
            }
            return fmt_whole(*amount);
        }

        [[nodiscard]] std::string join(std::vector<std::string> const& parts, std::string_view separator) {
            std::string out;
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (i != 0) {
                    out += separator;
                }
                out += parts[i];
            }
            return out;
        }

        // ------------------------------------------------------------------ json helpers

        [[nodiscard]] json get_or_null(json const& object, std::string_view key) {
            if (!object.is_object()) {
                return nullptr;
            }
            auto const it = object.find(std::string(key));
            return it == object.end() ? json(nullptr) : *it;
        }

        [[nodiscard]] bool truthy(json const& value) {
            if (value.is_null()) {
                return false;
            }
            if (value.is_boolean()) {
                return value.get<bool>();
            }
            if (value.is_number_integer()) {
                return value.get<long long>() != 0;
            }
            if (value.is_number_unsigned()) {
                return value.get<unsigned long long>() != 0;
            }
            if (value.is_number_float()) {
                return value.get<double>() != 0.0;
            }
            if (value.is_string()) {
                return !value.get<std::string>().empty();
            }
            if (value.is_array() || value.is_object()) {
                return !value.empty();
            }
            return false;
        }

        [[nodiscard]] json to_json(std::optional<long long> value) {
            return value ? json(*value) : json(nullptr);
        }

        [[nodiscard]] std::string dump_compact(json const& value) {
            return value.dump(-1, ' ', false);
        }

        // Python's `str()` for a JSON scalar, with `None` for null.
        [[nodiscard]] std::string py_str(json const& value) {
            return value.is_null() ? std::string("None") : scalar_text(value);
        }

        // ------------------------------------------------------------------ time helpers

        [[nodiscard]] bool all_digits(std::string_view text) noexcept {
            return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
                return c >= '0' && c <= '9';
            });
        }

        [[nodiscard]] bool digits_between(std::string_view text, std::size_t low, std::size_t high) noexcept {
            return text.size() >= low && text.size() <= high && all_digits(text);
        }

        [[nodiscard]] int minute_of(int hhmmss) noexcept {
            return (hhmmss / 10'000) * 60 + (hhmmss / 100) % 100;
        }

        [[nodiscard]] int hhmmss_seconds(int hhmmss) noexcept {
            return (hhmmss / 10'000) * 3600 + ((hhmmss / 100) % 100) * 60 + hhmmss % 100;
        }

        [[nodiscard]] int minute_hhmmss(int minute) noexcept {
            return (minute / 60) * 10'000 + (minute % 60) * 100 + 59;
        }

        [[nodiscard]] int text_hhmmss(std::string_view text) noexcept {
            auto const colon = text.find(':');
            if (colon == std::string_view::npos) {
                return 0;
            }
            int hour   = 0;
            int minute = 0;
            for (char const c : text.substr(0, colon)) {
                if (c >= '0' && c <= '9') {
                    hour = hour * 10 + (c - '0');
                }
            }
            for (char const c : text.substr(colon + 1)) {
                if (c >= '0' && c <= '9') {
                    minute = minute * 10 + (c - '0');
                }
            }
            return hour * 10'000 + minute * 100 + 59;
        }

        struct [[nodiscard]] tehran_clock {
            int year    = 0;
            int month   = 0;
            int day     = 0;
            int hour    = 0;
            int minute  = 0;
            int second  = 0;
            int weekday = 0; // 0 = Sunday .. 6 = Saturday
        };

        [[nodiscard]] tehran_clock tehran_parts(std::int64_t unix_seconds) noexcept {
            std::time_t const shifted = static_cast<std::time_t>(unix_seconds + 3LL * 3600 + 30LL * 60);
            std::tm           out{};
            ::gmtime_r(&shifted, &out);
            return {out.tm_year + 1900, out.tm_mon + 1, out.tm_mday, out.tm_hour, out.tm_min, out.tm_sec, out.tm_wday};
        }

        void sleep_seconds(double seconds) {
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long long>(seconds * 1000.0)));
        }

        [[nodiscard]] long long round_ll(double value) noexcept {
            return static_cast<long long>(std::nearbyint(value));
        }

        // ------------------------------------------------------------------ payload rows

        [[nodiscard]] std::vector<json> payload_rows(json const& payload, std::string_view key) {
            std::vector<json> out;
            if (payload.is_array()) {
                out.assign(payload.begin(), payload.end());
                return out;
            }
            if (!payload.is_object()) {
                return out;
            }
            auto const rows = get_or_null(payload, key);
            if (rows.is_array()) {
                out.assign(rows.begin(), rows.end());
                return out;
            }
            if (payload.size() == 1) {
                auto const& only = *payload.begin();
                if (only.is_array()) {
                    out.assign(only.begin(), only.end());
                }
            }
            return out;
        }

        // ------------------------------------------------------------------ ticks and bars

        struct [[nodiscard]] tick_sample {
            int                      secs     = 0;
            long long                trade_no = 0;
            std::optional<long long> price;
            long long                volume = 0;
        };

        struct [[nodiscard]] bar {
            int                      minute = 0;
            std::optional<long long> open;
            std::optional<long long> high;
            std::optional<long long> low;
            std::optional<long long> close;
            long long                volume = 0;
            std::optional<long long> trades;
            std::optional<long long> turnover;
        };

        struct [[nodiscard]] collapsed_bar {
            int                      from = 0;
            int                      to   = 0;
            std::optional<long long> open;
            std::optional<long long> high;
            std::optional<long long> low;
            std::optional<long long> close;
            long long                volume = 0;
            std::optional<long long> trades;
            std::optional<long long> turnover;
            long long                count = 1;
        };

        [[nodiscard]] std::optional<long long> merge_turnover(std::optional<long long> total,
                                                              std::optional<long long> extra) noexcept {
            if (!total || !extra) {
                return std::nullopt;
            }
            return *total + *extra;
        }

        [[nodiscard]] std::vector<json> dedup_ticks(std::vector<json> const& rows, bool include_canceled) {
            std::set<std::string> seen;
            std::vector<json>     kept;
            kept.reserve(rows.size());
            for (auto const& row : rows) {
                if (!row.is_object()) {
                    continue;
                }
                if (!include_canceled && truthy(value_of(row, {"canceled"}))) {
                    continue;
                }
                auto const key = normal(scalar_text(pick(row, {"nTran"})));
                if (!key.empty()) {
                    if (seen.contains(key)) {
                        continue;
                    }
                    seen.insert(key);
                }
                kept.push_back(row);
            }
            return kept;
        }

        [[nodiscard]] std::vector<tick_sample> tick_samples(std::vector<json> const& rows, bool include_canceled) {
            std::vector<tick_sample> samples;
            auto const               clean = dedup_ticks(rows, include_canceled);
            samples.reserve(clean.size());
            for (auto const& row : clean) {
                auto const raw = normal(scalar_text(pick(row, {"hEven"})));
                if (!digits_between(raw, 1, 6)) {
                    continue;
                }
                tick_sample sample;
                sample.secs     = std::stoi(raw);
                sample.trade_no = as_int(value_of(row, {"nTran"})).value_or(0);
                sample.price    = as_int(price(pick(row, {"pTran"})));
                sample.volume   = as_int(value_of(row, {"qTitTran"})).value_or(0);
                samples.push_back(sample);
            }
            std::sort(samples.begin(), samples.end(), [](tick_sample const& a, tick_sample const& b) {
                if (a.secs != b.secs) {
                    return a.secs < b.secs;
                }
                return a.trade_no < b.trade_no;
            });
            return samples;
        }

        [[nodiscard]] std::vector<bar> samples_to_bars(std::vector<tick_sample> const& samples) {
            std::map<int, bar> bars;
            for (auto const& sample : samples) {
                int const minute = minute_of(sample.secs);
                auto      it     = bars.find(minute);
                if (it == bars.end()) {
                    bar fresh;
                    fresh.minute   = minute;
                    fresh.open     = sample.price;
                    fresh.high     = sample.price;
                    fresh.low      = sample.price;
                    fresh.close    = sample.price;
                    fresh.volume   = 0;
                    fresh.trades   = 0;
                    fresh.turnover = 0;
                    it             = bars.emplace(minute, fresh).first;
                }
                auto& current = it->second;
                if (sample.price) {
                    if (!current.high) {
                        current.high = sample.price;
                    } else {
                        current.high = std::max(*current.high, *sample.price);
                    }
                    if (!current.low) {
                        current.low = sample.price;
                    } else {
                        current.low = std::min(*current.low, *sample.price);
                    }
                    current.close = sample.price;
                    if (!current.open) {
                        current.open = sample.price;
                    }
                }
                current.volume   += sample.volume;
                current.turnover  = current.turnover.value_or(0LL) + sample.price.value_or(0) * sample.volume;
                current.trades    = current.trades.value_or(0) + 1;
            }
            std::vector<bar> out;
            out.reserve(bars.size());
            for (auto& [minute, value] : bars) {
                (void) minute;
                out.push_back(value);
            }
            return out;
        }

        [[nodiscard]] std::vector<bar> merge_bars(std::vector<bar> const& bars, int interval) {
            if (interval <= 1) {
                return bars;
            }
            std::map<int, bar> merged;
            for (auto const& current : bars) {
                int const key = (current.minute / interval) * interval;
                auto      it  = merged.find(key);
                if (it == merged.end()) {
                    bar copy    = current;
                    copy.minute = key;
                    merged.emplace(key, copy);
                    continue;
                }
                auto& target = it->second;
                if (current.high) {
                    if (!target.high) {
                        target.high = current.high;
                    } else {
                        target.high = std::max(*target.high, *current.high);
                    }
                }
                if (current.low) {
                    if (!target.low) {
                        target.low = current.low;
                    } else {
                        target.low = std::min(*target.low, *current.low);
                    }
                }
                if (current.close) {
                    target.close = current.close;
                }
                target.volume   = target.volume + current.volume;
                target.turnover = merge_turnover(target.turnover, current.turnover);
                if (current.trades) {
                    target.trades = target.trades.value_or(0) + *current.trades;
                }
            }
            std::vector<bar> out;
            out.reserve(merged.size());
            for (auto& [key, value] : merged) {
                (void) key;
                out.push_back(value);
            }
            return out;
        }

        [[nodiscard]] std::vector<collapsed_bar> collapse_bars(std::vector<bar> const& bars) {
            std::vector<collapsed_bar> collapsed;
            collapsed.reserve(bars.size());
            for (auto const& current : bars) {
                if (!collapsed.empty()) {
                    auto& previous = collapsed.back();
                    if (previous.open == current.open && previous.high == current.high && previous.low == current.low &&
                        previous.close == current.close)
                    {
                        previous.to        = current.minute;
                        previous.volume   += current.volume;
                        previous.turnover  = merge_turnover(previous.turnover, current.turnover);
                        ++previous.count;
                        if (previous.trades) {
                            previous.trades = *previous.trades + current.trades.value_or(0);
                        }
                        continue;
                    }
                }
                collapsed_bar fresh;
                fresh.from     = current.minute;
                fresh.to       = current.minute;
                fresh.open     = current.open;
                fresh.high     = current.high;
                fresh.low      = current.low;
                fresh.close    = current.close;
                fresh.volume   = current.volume;
                fresh.trades   = current.trades;
                fresh.turnover = current.turnover;
                fresh.count    = 1;
                collapsed.push_back(fresh);
            }
            return collapsed;
        }

        [[nodiscard]] int choose_interval(std::vector<std::vector<bar>> const& day_bars, int max_rows, bool collapse) {
            for (int const interval : interval_ladder) {
                long long total = 0;
                for (auto const& bars : day_bars) {
                    auto const merged  = merge_bars(bars, interval);
                    total             += collapse ? static_cast<long long>(collapse_bars(merged).size())
                                                  : static_cast<long long>(merged.size());
                }
                if (total <= max_rows) {
                    return interval;
                }
            }
            return interval_ladder.back();
        }

        [[nodiscard]] std::optional<long long> bar_vwap(bar const& value) noexcept {
            if (!value.turnover || value.volume == 0) {
                return std::nullopt;
            }
            return round_ll(static_cast<double>(*value.turnover) / static_cast<double>(value.volume));
        }

        [[nodiscard]] std::optional<long long> session_vwap(std::vector<bar> const& bars) noexcept {
            long long volume   = 0;
            long long turnover = 0;
            for (auto const& value : bars) {
                if (!value.turnover) {
                    return std::nullopt;
                }
                volume   += value.volume;
                turnover += *value.turnover;
            }
            return volume != 0
                     ? std::optional<long long>(round_ll(static_cast<double>(turnover) / static_cast<double>(volume)))
                     : std::nullopt;
        }

        // ------------------------------------------------------------------ price structure

        [[nodiscard]] json value_area(std::map<long long, long long> const& levels) {
            if (levels.empty()) {
                return nullptr;
            }
            long long total = 0;
            for (auto const& [price, volume] : levels) {
                (void) price;
                total += volume;
            }
            if (total == 0) {
                return nullptr;
            }
            std::vector<long long> prices;
            prices.reserve(levels.size());
            for (auto const& [price, volume] : levels) {
                (void) volume;
                prices.push_back(price);
            }
            std::size_t poc_index = 0;
            long long   poc_value = levels.at(prices[0]);
            for (std::size_t i = 1; i < prices.size(); ++i) {
                auto const value = levels.at(prices[i]);
                if (value > poc_value) {
                    poc_value = value;
                    poc_index = i;
                }
            }
            std::size_t  low     = poc_index;
            std::size_t  high    = poc_index;
            long long    covered = levels.at(prices[poc_index]);
            double const target  = static_cast<double>(total) * value_area_share;
            while (static_cast<double>(covered) < target && (low > 0 || high + 1 < prices.size())) {
                long long const below = low > 0 ? levels.at(prices[low - 1]) : -1;
                long long const above = high + 1 < prices.size() ? levels.at(prices[high + 1]) : -1;
                if (above >= below) {
                    ++high;
                    covered += levels.at(prices[high]);
                } else {
                    --low;
                    covered += levels.at(prices[low]);
                }
            }
            json out         = json::object();
            out["poc_rial"]  = prices[poc_index];
            out["val_rial"]  = prices[low];
            out["vah_rial"]  = prices[high];
            out["levels"]    = static_cast<long long>(high - low + 1);
            out["share_pct"] = round_to(static_cast<double>(covered) / static_cast<double>(total) * 100.0, 1);
            return out;
        }

        [[nodiscard]] json session_summary(std::vector<tick_sample> const& samples) {
            if (samples.empty()) {
                return nullptr;
            }
            bool const priced = std::any_of(samples.begin(), samples.end(), [](tick_sample const& sample) {
                return sample.price.has_value();
            });
            if (!priced) {
                return nullptr;
            }
            int const                      first_secs = samples.front().secs;
            std::optional<long long>       high;
            std::optional<long long>       low;
            std::optional<int>             high_secs;
            std::optional<int>             low_secs;
            std::optional<long long>       last;
            long long                      volume   = 0;
            long long                      turnover = 0;
            std::map<long long, long long> levels;

            struct print {
                long long value  = 0;
                int       secs   = 0;
                long long price  = 0;
                long long shares = 0;
            };

            std::vector<print>       largest;
            std::optional<long long> first_priced;
            for (auto const& sample : samples) {
                if (sample.price) {
                    if (!first_priced) {
                        first_priced = sample.price;
                    }
                    if (!high || *sample.price > *high) {
                        high      = sample.price;
                        high_secs = sample.secs;
                    }
                    if (!low || *sample.price < *low) {
                        low      = sample.price;
                        low_secs = sample.secs;
                    }
                    last                   = sample.price;
                    levels[*sample.price] += sample.volume;
                    largest.push_back({*sample.price * sample.volume, sample.secs, *sample.price, sample.volume});
                }
                volume   += sample.volume;
                turnover += (sample.price ? *sample.price : 0) * sample.volume;
            }
            std::optional<long long> const vwap =
              volume != 0
                ? std::optional<long long>(round_ll(static_cast<double>(turnover) / static_cast<double>(volume)))
                : std::nullopt;
            long long above = 0;
            if (vwap) {
                for (auto const& sample : samples) {
                    if (sample.price && *sample.price >= *vwap) {
                        above += sample.volume;
                    }
                }
            }
            json summary          = json::object();
            summary["first_rial"] = to_json(first_priced);
            summary["high_rial"]  = to_json(high);
            summary["high_time"]  = high_secs ? json(fmt_minute(minute_of(*high_secs))) : json(nullptr);
            summary["low_rial"]   = to_json(low);
            summary["low_time"]   = low_secs ? json(fmt_minute(minute_of(*low_secs))) : json(nullptr);
            summary["last_rial"]  = to_json(last);
            if (last && high && low && *high > *low) {
                summary["close_location_pct"] =
                  round_to(static_cast<double>(*last - *low) / static_cast<double>(*high - *low) * 100.0, 1);
            } else {
                summary["close_location_pct"] = nullptr;
            }
            summary["vwap_rial"] = to_json(vwap);
            if (vwap && volume != 0) {
                summary["above_vwap_pct"] =
                  round_to(static_cast<double>(above) / static_cast<double>(volume) * 100.0, 1);
            } else {
                summary["above_vwap_pct"] = nullptr;
            }
            summary["volume_shares"] = volume;
            summary["value_rial"]    = turnover;
            summary["trades"]        = static_cast<long long>(samples.size());

            json ranges = json::object();
            for (int const window : session_open_ranges) {
                int const                end = first_secs + window * 60;
                std::optional<long long> window_high;
                std::optional<long long> window_low;
                for (auto const& sample : samples) {
                    if (sample.secs > end || !sample.price) {
                        continue;
                    }
                    window_high = window_high ? std::max(*window_high, *sample.price) : *sample.price;
                    window_low  = window_low ? std::min(*window_low, *sample.price) : *sample.price;
                }
                if (window_high && window_low) {
                    json band                          = json::object();
                    band["high_rial"]                  = *window_high;
                    band["low_rial"]                   = *window_low;
                    ranges[std::format("{}m", window)] = std::move(band);
                }
            }
            summary["opening_range"] = std::move(ranges);
            summary["value_area"]    = value_area(levels);

            std::sort(largest.begin(), largest.end(), [](print const& a, print const& b) {
                if (a.value != b.value) {
                    return a.value > b.value;
                }
                if (a.secs != b.secs) {
                    return a.secs > b.secs;
                }
                if (a.price != b.price) {
                    return a.price > b.price;
                }
                return a.shares > b.shares;
            });
            if (largest.size() > 3) {
                largest.resize(3);
            }
            std::stable_sort(largest.begin(), largest.end(), [](print const& a, print const& b) {
                return a.secs < b.secs;
            });
            json prints = json::array();
            for (auto const& item : largest) {
                json entry             = json::object();
                entry["time"]          = fmt_minute(minute_of(item.secs));
                entry["price_rial"]    = item.price;
                entry["volume_shares"] = item.shares;
                entry["value_rial"]    = item.value;
                prints.push_back(std::move(entry));
            }
            summary["largest_prints"] = std::move(prints);
            return summary;
        }

        // ------------------------------------------------------------------ fetching

        [[nodiscard]] json official_from_row(json const& row) {
            if (!row.is_object()) {
                return nullptr;
            }
            auto const last_price  = as_int(price(pick(row, {"pDrCotVal"})));
            auto const yesterday   = as_int(price(pick(row, {"priceYesterday"})));
            json       official    = json::object();
            official["close"]      = price(pick(row, {"pClosing"}));
            official["last_price"] = to_json(last_price);
            official["first"]      = price(pick(row, {"priceFirst"}));
            official["low"]        = price(pick(row, {"priceMin"}));
            official["high"]       = price(pick(row, {"priceMax"}));
            official["yesterday"]  = to_json(yesterday);
            if (last_price && yesterday) {
                official["change"] = *last_price - *yesterday;
            } else {
                official["change"] = value_of(row, {"priceChange"});
            }
            official["close_time"] = trade_time(pick(row, {"hEven"}));
            official["trades"]     = value_of(row, {"zTotTran"});
            official["volume"]     = value_of(row, {"qTotTran5J"});
            official["value_rial"] = value_of(row, {"qTotCap"});
            bool const any         = std::any_of(official.begin(), official.end(), [](json const& value) {
                return !value.is_null();
            });
            return any ? official : json(nullptr);
        }

        struct [[nodiscard]] session_info {
            std::string              greg;
            std::string              jalali;
            std::optional<long long> trades;
            std::optional<long long> volume;
            json                     official;
        };

        [[nodiscard]] std::vector<session_info> fetch_sessions(client& http, std::string const& code) {
            auto const rows =
              http.api(std::format("ClosingPrice/GetClosingPriceDailyList/{}/0", code), "closingPriceDaily");
            std::map<std::string, session_info> sessions;
            if (rows.is_array()) {
                for (auto const& row : rows) {
                    if (!row.is_object()) {
                        continue;
                    }
                    auto const greg = normal(scalar_text(pick(row, {"dEven"})));
                    if (!digits_between(greg, 8, 8)) {
                        continue;
                    }
                    session_info info;
                    info.greg      = greg;
                    info.jalali    = trade_date(greg);
                    info.trades    = as_int(value_of(row, {"zTotTran"}));
                    info.volume    = as_int(value_of(row, {"qTotTran5J"}));
                    info.official  = official_from_row(row);
                    sessions[greg] = std::move(info);
                }
            }
            std::vector<session_info> out;
            out.reserve(sessions.size());
            for (auto& [greg, info] : sessions) {
                (void) greg;
                out.push_back(std::move(info));
            }
            return out;
        }

        [[nodiscard]] std::vector<json>
        fetch_ticks(client& http, std::string const& code, std::string const& greg, int retries = 6) {
            auto const url = std::format("{}/Trade/GetTradeHistory/{}/{}/false", tsetmc_base, code, greg);
            for (int attempt = 0; attempt < retries; ++attempt) {
                json payload = nullptr;
                try {
                    payload = http.json_get(url, max_bytes);
                } catch (error const&) {
                    payload = nullptr;
                }
                auto rows = payload_rows(payload, "tradeHistory");
                if (!rows.empty()) {
                    return rows;
                }
                if (attempt < retries - 1) {
                    sleep_seconds(1.5 * (attempt + 1));
                }
            }
            return {};
        }

        [[nodiscard]] std::vector<json> fetch_live_ticks(client& http, std::string const& code) {
            auto const payload = http.json_get(std::format("{}/Trade/GetTrade/{}", tsetmc_base, code), max_bytes);
            auto       rows    = payload_rows(payload, "trade");
            auto const clock   = tehran_parts(tehran_now());
            int const  now     = clock.hour * 10'000 + clock.minute * 100 + clock.second;
            if (now < market_pre_open) {
                return {};
            }
            long long last = 0;
            for (auto const& row : rows) {
                if (!row.is_object()) {
                    continue;
                }
                auto const raw = normal(scalar_text(pick(row, {"hEven"})));
                if (digits_between(raw, 1, 6)) {
                    last = std::max(last, std::stoll(raw));
                }
            }
            if (!rows.empty() && hhmmss_seconds(static_cast<int>(last)) > hhmmss_seconds(now) + live_skew_seconds) {
                return {};
            }
            return rows;
        }

        [[nodiscard]] std::vector<bar> fetch_live_bars(client& http, std::string const& code) {
            auto const       payload = http.json_get(std::format("{}/Trade/GetTradeIntraDay/{}", tsetmc_base, code));
            std::vector<bar> bars;
            for (auto const& row : payload_rows(payload, "tradeIntraDay")) {
                if (!row.is_object()) {
                    continue;
                }
                auto const raw = normal(scalar_text(pick(row, {"hEven"})));
                if (!digits_between(raw, 1, 6)) {
                    continue;
                }
                bar value;
                value.minute   = minute_of(std::stoi(raw));
                value.open     = as_int(price(pick(row, {"openPrice"})));
                value.high     = as_int(price(pick(row, {"maxPrice"})));
                value.low      = as_int(price(pick(row, {"minPrice"})));
                value.close    = as_int(price(pick(row, {"closePrice"})));
                value.volume   = as_int(value_of(row, {"volume"})).value_or(0);
                value.trades   = std::nullopt;
                value.turnover = std::nullopt;
                bars.push_back(value);
            }
            std::sort(bars.begin(), bars.end(), [](bar const& a, bar const& b) {
                return a.minute < b.minute;
            });
            return bars;
        }

        [[nodiscard]] json fetch_live_quote(client& http, std::string const& code) {
            auto const info = http.api(std::format("ClosingPrice/GetClosingPriceInfo/{}", code), "closingPriceInfo");
            if (!info.is_object()) {
                return nullptr;
            }
            auto const raw = normal(scalar_text(pick(info, {"lastHEven"})));
            if (!digits_between(raw, 1, 6)) {
                return nullptr;
            }
            int const minute = minute_of(std::stoi(raw));
            if (minute == 0) {
                return nullptr;
            }
            auto const last_price = as_int(price(pick(info, {"pDrCotVal"})));
            auto const yesterday  = as_int(price(pick(info, {"priceYesterday"})));
            json       quote      = json::object();
            quote["minute"]       = minute;
            quote["time"]         = fmt_minute(minute);
            quote["last_price"]   = to_json(last_price);
            quote["trades"]       = value_of(info, {"zTotTran"});
            quote["volume"]       = value_of(info, {"qTotTran5J"});
            quote["first"]        = price(pick(info, {"priceFirst"}));
            quote["low"]          = price(pick(info, {"priceMin"}));
            quote["high"]         = price(pick(info, {"priceMax"}));
            quote["vwap"]         = price(pick(info, {"pClosing"}));
            if (last_price && yesterday) {
                quote["change"] = *last_price - *yesterday;
            } else {
                quote["change"] = value_of(info, {"priceChange"});
            }
            quote["yesterday"]  = to_json(yesterday);
            quote["value_rial"] = value_of(info, {"qTotCap"});
            return quote;
        }

        [[nodiscard]] json identity(client& http, std::string const& code) {
            auto const row = http.api(std::format("Instrument/GetInstrumentIdentity/{}", code), "instrumentIdentity");
            if (!row.is_object()) {
                throw error("No instrument identity");
            }
            json out            = json::object();
            out["symbol"]       = normal(scalar_text(pick(row, {"lVal18AFC", "lVal18"})));
            out["name"]         = normal(scalar_text(pick(row, {"lVal30"})));
            out["isin"]         = pick(row, {"cIsin"});
            out["industry"]     = pick(pick(row, {"sector"}), {"lSecVal"});
            out["sub_industry"] = pick(pick(row, {"subSector"}), {"lSoSecVal"});
            out["board"]        = pick(row, {"cgrValCotTitle"});
            out["share_count"]  = value_of(row, {"shareCount", "zTitad"});
            return out;
        }

        [[nodiscard]] json price_limits(client& http, std::string const& code) {
            auto const row = http.api(std::format("Instrument/GetInstrumentInfo/{}", code), "instrumentInfo");
            if (!row.is_object()) {
                return nullptr;
            }
            auto const band = pick(row, {"staticThreshold"});
            if (!band.is_object()) {
                return nullptr;
            }
            auto const low  = as_int(price(pick(band, {"psGelStaMin"})));
            auto const high = as_int(price(pick(band, {"psGelStaMax"})));
            if (!low || !high) {
                return nullptr;
            }
            json out         = json::object();
            out["low_rial"]  = *low;
            out["high_rial"] = *high;
            return out;
        }

        [[nodiscard]] json live_client_type(client& http, std::string const& code) {
            auto const row = http.api(std::format("ClientType/GetClientType/{}/1/0", code), "clientType");
            if (!row.is_object()) {
                return nullptr;
            }
            auto const buy_i              = as_int(value_of(row, {"buy_I_Volume"}));
            auto const sell_i             = as_int(value_of(row, {"sell_I_Volume"}));
            auto const buy_n              = as_int(value_of(row, {"buy_N_Volume"}));
            auto const sell_n             = as_int(value_of(row, {"sell_N_Volume"}));
            json       out                = json::object();
            out["individual_buy_shares"]  = to_json(buy_i);
            out["individual_sell_shares"] = to_json(sell_i);
            out["legal_buy_shares"]       = to_json(buy_n);
            out["legal_sell_shares"]      = to_json(sell_n);
            out["legal_net_shares"] = buy_n.has_value() && sell_n.has_value() ? json(*buy_n - *sell_n) : json(nullptr);
            out["individual_buy_value_rial"]  = nullptr;
            out["individual_sell_value_rial"] = nullptr;
            out["legal_buy_value_rial"]       = nullptr;
            out["legal_sell_value_rial"]      = nullptr;
            out["legal_net_value_rial"]       = nullptr;
            out["individual_buyers"]          = value_of(row, {"buy_CountI"});
            out["individual_sellers"]         = value_of(row, {"sell_CountI"});
            out["legal_buyers"]               = value_of(row, {"buy_CountN"});
            out["legal_sellers"]              = value_of(row, {"sell_CountN"});
            return out;
        }

        [[nodiscard]] json client_type_history(client& http, std::string const& code) {
            auto const rows = http.api(std::format("ClientType/GetClientTypeHistory/{}", code), "clientType");
            if (!rows.is_array()) {
                throw error("Unexpected client-type history result");
            }
            json result = json::object();
            for (auto const& row : rows) {
                if (!row.is_object()) {
                    continue;
                }
                auto const date = trade_date(pick(row, {"recDate"}));
                if (date.empty()) {
                    continue;
                }
                auto const buy_i                 = as_int(value_of(row, {"buy_I_Volume"}));
                auto const sell_i                = as_int(value_of(row, {"sell_I_Volume"}));
                auto const buy_n                 = as_int(value_of(row, {"buy_N_Volume"}));
                auto const sell_n                = as_int(value_of(row, {"sell_N_Volume"}));
                auto const buy_i_value           = as_int(value_of(row, {"buy_I_Value"}));
                auto const sell_i_value          = as_int(value_of(row, {"sell_I_Value"}));
                auto const buy_n_value           = as_int(value_of(row, {"buy_N_Value"}));
                auto const sell_n_value          = as_int(value_of(row, {"sell_N_Value"}));
                json       record                = json::object();
                record["individual_buy_shares"]  = to_json(buy_i);
                record["individual_sell_shares"] = to_json(sell_i);
                record["legal_buy_shares"]       = to_json(buy_n);
                record["legal_sell_shares"]      = to_json(sell_n);
                record["legal_net_shares"] =
                  buy_n.has_value() && sell_n.has_value() ? json(*buy_n - *sell_n) : json(nullptr);
                record["individual_buy_value_rial"]  = to_json(buy_i_value);
                record["individual_sell_value_rial"] = to_json(sell_i_value);
                record["legal_buy_value_rial"]       = to_json(buy_n_value);
                record["legal_sell_value_rial"]      = to_json(sell_n_value);
                record["legal_net_value_rial"] =
                  buy_n_value.has_value() && sell_n_value.has_value()
                    ? json(*buy_n_value - *sell_n_value)
                    : json(nullptr);
                record["individual_buyers"]  = value_of(row, {"buy_I_Count"});
                record["individual_sellers"] = value_of(row, {"sell_I_Count"});
                record["legal_buyers"]       = value_of(row, {"buy_N_Count"});
                record["legal_sellers"]      = value_of(row, {"sell_N_Count"});
                result[date]                 = std::move(record);
            }
            return result;
        }

        [[nodiscard]] json instrument_statistics(client& http, std::string const& code, int attempts = 3) {
            auto const url  = std::format("{}/MarketData/GetInstrumentStatistic/{}", tsetmc_base, code);
            json       rows = nullptr;
            for (int attempt = 0; attempt < attempts; ++attempt) {
                rows = unwrap(http.json_get(url), "instrumentStatistic");
                if (!rows.is_array()) {
                    throw error("Unexpected instrument statistics result");
                }
                if (!rows.empty()) {
                    break;
                }
                if (attempt < attempts - 1) {
                    http.drop(url);
                    sleep_seconds(0.5 * (attempt + 1));
                }
            }
            static std::map<std::pair<long long, long long>, std::string_view> const fields = {
              {  {1, 5},                    "avg_value_3m_rial"},
              {  {2, 5},                   "avg_value_12m_rial"},
              {  {5, 7},                 "avg_volume_3m_shares"},
              {  {6, 7},                "avg_volume_12m_shares"},
              {  {9, 4},                        "avg_trades_3m"},
              { {10, 4},                       "avg_trades_12m"},
              { {13, 8},                       "last_vwap_rial"},
              { {14, 8},                "last_vwap_capped_rial"},
              { {15, 5},                      "last_value_rial"},
              { {16, 7},                   "last_volume_shares"},
              { {17, 4},                          "last_trades"},
              { {36, 6},                   "company_value_rial"},
              { {37, 6},                   "company_value_rank"},
              {{50, 11},  "avg_individual_buy_volume_3m_shares"},
              {{54, 11},       "avg_legal_buy_volume_3m_shares"},
              {{58, 11},             "avg_individual_buyers_3m"},
              {{62, 11},                  "avg_legal_buyers_3m"},
              {{70, 11}, "avg_individual_sell_volume_3m_shares"},
              {{74, 11},      "avg_legal_sell_volume_3m_shares"},
              {{78, 11},            "avg_individual_sellers_3m"},
              {{82, 11},                 "avg_legal_sellers_3m"}
            };
            json result = json::object();
            for (auto const& row : rows) {
                if (!row.is_object()) {
                    continue;
                }
                auto const data_type      = as_int(value_of(row, {"dataType"}));
                auto const partition_code = as_int(value_of(row, {"partitionCode"}));
                if (!data_type || !partition_code) {
                    continue;
                }
                auto const it = fields.find({*data_type, *partition_code});
                if (it == fields.end()) {
                    continue;
                }
                auto const parsed = number(pick(row, {"dataValue"}));
                if (!parsed.is_null()) {
                    result[std::string(it->second)] = parsed;
                }
            }
            return result.empty() ? json(nullptr) : result;
        }

        [[nodiscard]] json market_overview(client& http) {
            auto const row = http.api("MarketData/GetMarketOverview/1", "marketOverview");
            if (!row.is_object()) {
                throw error("Unexpected market overview result");
            }
            auto const index        = number(pick(row, {"indexLastValue"}));
            auto const change       = number(pick(row, {"indexChange"}));
            auto const equal        = number(pick(row, {"indexEqualWeightedLastValue"}));
            auto const equal_change = number(pick(row, {"indexEqualWeightedChange"}));
            auto const change_pct   = [](json const& current, json const& delta) -> json {
                auto const base_current = as_double(current);
                auto const base_delta   = as_double(delta);
                if (!base_current || !base_delta) {
                    return nullptr;
                }
                double const base = *base_current - *base_delta;
                if (base == 0.0) {
                    return nullptr;
                }
                return round_to(*base_delta / base * 100.0, 2);
            };
            json out                         = json::object();
            out["date"]                      = trade_date(pick(row, {"marketActivityDEven"}));
            out["time"]                      = trade_time(pick(row, {"marketActivityHEven"}));
            out["index_value"]               = index;
            out["index_change"]              = change;
            out["index_change_pct"]          = change_pct(index, change);
            out["equal_weighted_value"]      = equal;
            out["equal_weighted_change_pct"] = change_pct(equal, equal_change);
            out["trades"]                    = number(pick(row, {"marketActivityZTotTran"}));
            out["volume_shares"]             = number(pick(row, {"marketActivityQTotTran"}));
            out["value_rial"]                = number(pick(row, {"marketActivityQTotCap"}));
            out["state"]                     = pick(row, {"marketStateTitle"});
            return out;
        }

        [[nodiscard]] json market_breadth_day(client& http, std::string const& greg, std::string const& code) {
            auto const rows = http.api(std::format("ClosingPrice/GetInstrmentsHistoryInDay/{}", greg),
                                       "closingPriceDailyHistoryWithInstDetails");
            if (!rows.is_array() || rows.empty()) {
                return nullptr;
            }
            std::vector<double>   changes;
            long long             up      = 0;
            long long             down    = 0;
            long long             flat    = 0;
            long long             skipped = 0;
            std::optional<double> own;
            for (auto const& row : rows) {
                if (!row.is_object()) {
                    continue;
                }
                auto const yesterday = as_int(price(pick(row, {"priceYesterday"})));
                auto const change    = as_double(number(pick(row, {"priceChange"})));
                if (!yesterday || *yesterday < 10 || !change.has_value()) {
                    ++skipped;
                    continue;
                }
                double const pct = *change / static_cast<double>(*yesterday) * 100.0;
                if (std::abs(pct) > 25.0) {
                    ++skipped;
                    continue;
                }
                changes.push_back(pct);
                if (pct > 0.01) {
                    ++up;
                } else if (pct < -0.01) {
                    ++down;
                } else {
                    ++flat;
                }
                if (scalar_text(pick(row, {"insCode"})) == code) {
                    own = pct;
                }
            }
            if (changes.empty()) {
                return nullptr;
            }
            auto sorted = changes;
            std::sort(sorted.begin(), sorted.end());
            double     median = 0.0;
            auto const size   = sorted.size();
            if (size % 2 == 1) {
                median = sorted[size / 2];
            } else {
                median = (sorted[size / 2 - 1] + sorted[size / 2]) / 2.0;
            }
            double sum = 0.0;
            for (double const value : changes) {
                sum += value;
            }
            json out                 = json::object();
            out["date"]              = trade_date(greg);
            out["scope"]             = "whole market, every quoted instrument";
            out["instruments"]       = static_cast<long long>(changes.size());
            out["skipped"]           = skipped;
            out["advancers"]         = up;
            out["decliners"]         = down;
            out["unchanged"]         = flat;
            out["median_change_pct"] = round_to(median, 2);
            out["avg_change_pct"]    = round_to(sum / static_cast<double>(changes.size()), 2);
            if (own.has_value()) {
                out["instrument_change_pct"] = round_to(*own, 2);
                long long below              = 0;
                for (double const value : changes) {
                    if (value < *own) {
                        ++below;
                    }
                }
                out["instrument_percentile"] =
                  round_to(static_cast<double>(below) / static_cast<double>(changes.size()) * 100.0, 1);
            }
            return out;
        }

        // ------------------------------------------------------------------ order book

        [[nodiscard]] json book_update(json const& row) {
            auto const level = as_int(value_of(row, {"number"}));
            if (!level || *level < 1 || *level > depth_levels) {
                return nullptr;
            }
            auto const hhmmss = as_int(value_of(row, {"hEven"}));
            json       out    = json::object();
            out["level"]      = *level;
            out["hhmmss"]     = to_json(hhmmss);
            out["bid_rial"]   = price(pick(row, {"pMeDem"}));
            out["bid_shares"] = value_of(row, {"qTitMeDem"});
            out["bid_orders"] = value_of(row, {"zOrdMeDem"});
            out["ask_rial"]   = price(pick(row, {"pMeOf"}));
            out["ask_shares"] = value_of(row, {"qTitMeOf"});
            out["ask_orders"] = value_of(row, {"zOrdMeOf"});
            return out;
        }

        [[nodiscard]] std::vector<json>
        fetch_book_stream(client& http, std::string const& code, std::string const& greg, int retries = 4) {
            auto const url = std::format("{}/BestLimits/{}/{}", tsetmc_base, code, greg);
            for (int attempt = 0; attempt < retries; ++attempt) {
                json payload = nullptr;
                try {
                    payload = http.json_get(url, max_bytes);
                } catch (error const&) {
                    payload = nullptr;
                }
                std::vector<json> updates;
                for (auto const& row : payload_rows(payload, "bestLimitsHistory")) {
                    if (!row.is_object()) {
                        continue;
                    }
                    auto update = book_update(row);
                    if (update.is_object()) {
                        auto const hhmmss = as_int(get_or_null(update, "hhmmss"));
                        if (hhmmss && *hhmmss <= depth_cutoff) {
                            updates.push_back(std::move(update));
                        }
                    }
                }
                if (!updates.empty()) {
                    std::sort(updates.begin(), updates.end(), [](json const& a, json const& b) {
                        auto const ah = as_int(get_or_null(a, "hhmmss")).value_or(0);
                        auto const bh = as_int(get_or_null(b, "hhmmss")).value_or(0);
                        if (ah != bh) {
                            return ah < bh;
                        }
                        return as_int(get_or_null(a, "level")).value_or(0) <
                               as_int(get_or_null(b, "level")).value_or(0);
                    });
                    return updates;
                }
                if (attempt < retries - 1) {
                    sleep_seconds(1.5 * (attempt + 1));
                }
            }
            return {};
        }

        [[nodiscard]] std::vector<json> fetch_live_book(client& http, std::string const& code) {
            auto const        payload = http.json_get(std::format("{}/BestLimits/{}", tsetmc_base, code), max_bytes);
            std::vector<json> updates;
            for (auto const& row : payload_rows(payload, "bestLimits")) {
                if (!row.is_object()) {
                    continue;
                }
                auto update = book_update(row);
                if (update.is_object()) {
                    updates.push_back(std::move(update));
                }
            }
            std::sort(updates.begin(), updates.end(), [](json const& a, json const& b) {
                return as_int(get_or_null(a, "level")).value_or(0) < as_int(get_or_null(b, "level")).value_or(0);
            });
            return updates;
        }

        [[nodiscard]] std::size_t
        advance_book(std::vector<json> const& updates, std::size_t cursor, std::map<int, json>& state, int hhmmss) {
            while (cursor < updates.size() && as_int(get_or_null(updates[cursor], "hhmmss")).value_or(0) <= hhmmss) {
                auto const level = static_cast<int>(as_int(get_or_null(updates[cursor], "level")).value_or(0));
                state[level]     = updates[cursor];
                ++cursor;
            }
            return cursor;
        }

        [[nodiscard]] json depth_snapshot(std::map<int, json> const& state) {
            if (state.empty()) {
                return json::object();
            }
            auto const& best      = state.begin()->second;
            long long   bid_depth = 0;
            long long   ask_depth = 0;
            for (auto const& [level, item] : state) {
                (void) level;
                bid_depth += as_int(get_or_null(item, "bid_shares")).value_or(0);
                ask_depth += as_int(get_or_null(item, "ask_shares")).value_or(0);
            }
            long long const total    = bid_depth + ask_depth;
            auto const      bid_rial = as_int(get_or_null(best, "bid_rial"));
            auto const      ask_rial = as_int(get_or_null(best, "ask_rial"));
            json            out      = json::object();
            out["bid_rial"]          = get_or_null(best, "bid_rial");
            out["bid_shares"]        = get_or_null(best, "bid_shares");
            out["bid_orders"]        = get_or_null(best, "bid_orders");
            out["ask_rial"]          = get_or_null(best, "ask_rial");
            out["ask_shares"]        = get_or_null(best, "ask_shares");
            out["ask_orders"]        = get_or_null(best, "ask_orders");
            out["spread_rial"]       = bid_rial && ask_rial ? json(*ask_rial - *bid_rial) : json(nullptr);
            out["bid_depth_shares"]  = bid_depth;
            out["ask_depth_shares"]  = ask_depth;
            out["depth_imbalance_pct"] =
              total != 0
                ? json(round_to(static_cast<double>(bid_depth - ask_depth) / static_cast<double>(total) * 100.0, 1))
                : json(nullptr);
            return out;
        }

        [[nodiscard]] json depth_summary(std::vector<json> const& updates, std::optional<int> hhmmss) {
            std::map<int, json> state;
            if (!hhmmss) {
                for (auto const& update : updates) {
                    state[static_cast<int>(as_int(get_or_null(update, "level")).value_or(0))] = update;
                }
            } else {
                (void) advance_book(updates, 0, state, *hhmmss);
            }
            auto snapshot = depth_snapshot(state);
            if (snapshot.empty()) {
                return nullptr;
            }
            snapshot["time"] = hhmmss ? json(fmt_minute(minute_of(*hhmmss))) : json(nullptr);
            return snapshot;
        }

        [[nodiscard]] std::vector<int>
        row_boundaries(std::vector<json> const& rows, int interval, std::optional<int> last_minute) {
            int const        span = std::max(0, interval - 1);
            std::vector<int> out;
            out.reserve(rows.size());
            for (auto const& row : rows) {
                int minute = minute_of(text_hhmmss(scalar_text(get_or_null(row, "to")))) + span;
                if (last_minute) {
                    minute = std::min(minute, *last_minute);
                }
                out.push_back(minute);
            }
            return out;
        }

        void attach_depth(std::vector<json>&       rows,
                          std::vector<json> const& updates,
                          int                      interval,
                          std::optional<int>       last_minute) {
            if (updates.empty() || rows.empty()) {
                return;
            }
            std::map<int, json> state;
            std::size_t         cursor = 0;
            auto const          bounds = row_boundaries(rows, interval, last_minute);
            for (std::size_t i = 0; i < rows.size(); ++i) {
                cursor              = advance_book(updates, cursor, state, minute_hhmmss(bounds[i]));
                auto const snapshot = depth_snapshot(state);
                for (auto it = snapshot.begin(); it != snapshot.end(); ++it) {
                    rows[i][it.key()] = it.value();
                }
            }
        }

        // ------------------------------------------------------------------ day loading

        struct [[nodiscard]] day_data {
            std::string                date;
            std::string                greg;
            std::string                source;
            int                        native_interval = 1;
            std::vector<bar>           bars;
            json                       session;
            json                       official;
            json                       check;
            std::vector<json>          depth_updates;
            std::optional<std::string> depth_note;
            std::vector<json>          live_book;
            json                       live_quote;
        };

        [[nodiscard]] bool book_covers_session(day_data const& day, std::vector<json> const& updates) {
            if (day.bars.empty() || updates.empty()) {
                return true;
            }
            int min_minute = day.bars.front().minute;
            int max_minute = day.bars.front().minute;
            for (auto const& value : day.bars) {
                min_minute = std::min(min_minute, value.minute);
                max_minute = std::max(max_minute, value.minute);
            }
            auto const first    = as_int(get_or_null(updates.front(), "hhmmss")).value_or(0);
            auto const last     = as_int(get_or_null(updates.back(), "hhmmss")).value_or(0);
            bool const started  = minute_of(static_cast<int>(first)) <= min_minute;
            bool const finished = minute_of(static_cast<int>(last)) + depth_cover_slack_minutes >= max_minute;
            return started && finished;
        }

        void attach_day_depth(
          client&            http,
          std::string const& code,
          std::string const& greg,
          day_data&          day,
          int                attempts = 3) {
            std::vector<json> updates;
            for (int attempt = 0; attempt < attempts; ++attempt) {
                try {
                    updates = fetch_book_stream(http, code, greg);
                } catch (error const& exc) {
                    day.depth_note = std::format("order book unavailable: {}", exc.what());
                    return;
                }
                if (updates.empty()) {
                    day.depth_note = std::string(
                      "order book not published for this date (it appears after the close, and only for "
                      "roughly the last two months)");
                    return;
                }
                if (book_covers_session(day, updates)) {
                    day.depth_updates = std::move(updates);
                    return;
                }
                if (attempt < attempts - 1) {
                    http.drop(std::format("{}/BestLimits/{}/{}", tsetmc_base, code, greg));
                    sleep_seconds(0.5 * (attempt + 1));
                }
            }
            auto const first = as_int(get_or_null(updates.front(), "hhmmss")).value_or(0);
            auto const last  = as_int(get_or_null(updates.back(), "hhmmss")).value_or(0);
            day.depth_note   = std::format(
              "the level stream only spans {}–{}, not the whole session (the exchange is still publishing this "
              "date); rerun later",
              fmt_minute(minute_of(static_cast<int>(first))),
              fmt_minute(minute_of(static_cast<int>(last))));
        }

        void attach_live_depth(client& http, std::string const& code, day_data& day) {
            std::vector<json> updates;
            try {
                updates = fetch_live_book(http, code);
            } catch (error const& exc) {
                day.depth_note = std::format("order book unavailable: {}", exc.what());
                return;
            }
            if (updates.empty()) {
                day.depth_note = std::string("no order book snapshot available");
                return;
            }
            day.live_book = std::move(updates);
            day.depth_note =
              std::string("intraday order-book history is published after the close; showing the current snapshot");
        }

        [[nodiscard]] day_data day_from_rows(std::string jalali, std::string source, std::vector<json> const& rows) {
            auto const samples = tick_samples(rows, false);
            day_data   day;
            day.date            = std::move(jalali);
            day.source          = std::move(source);
            day.bars            = samples_to_bars(samples);
            day.native_interval = 1;
            day.session         = session_summary(samples);
            return day;
        }

        [[nodiscard]] json tape_check(std::optional<long long> expected_trades,
                                      std::optional<long long> expected_volume,
                                      std::vector<json> const& rows) {
            if (!expected_trades) {
                return nullptr;
            }
            auto const clean  = dedup_ticks(rows, false);
            auto const count  = static_cast<long long>(clean.size());
            long long  volume = 0;
            for (auto const& row : clean) {
                volume += as_int(value_of(row, {"qTitTran"})).value_or(0);
            }
            std::vector<std::string> problems;
            if (count != *expected_trades) {
                problems.push_back(std::format("{} trades vs {} reported", count, *expected_trades));
            }
            if (expected_volume && *expected_volume != 0 && volume != *expected_volume) {
                problems.push_back(std::format("volume {} vs {} reported", volume, *expected_volume));
            }
            return problems.empty() ? json(nullptr) : json(join(problems, "; "));
        }

        struct [[nodiscard]] entry {
            std::string              greg;
            std::string              jalali;
            bool                     live = false;
            std::optional<long long> expected_trades;
            std::optional<long long> expected_volume;
            json                     official;
        };

        [[nodiscard]] std::optional<day_data>
        load_day(client& http, std::string const& code, entry const& selected, bool depth) {
            if (!selected.live) {
                auto const rows = fetch_ticks(http, code, selected.greg);
                if (rows.empty()) {
                    return std::nullopt;
                }
                auto day     = day_from_rows(selected.jalali, "ticks", rows);
                day.greg     = selected.greg;
                day.check    = tape_check(selected.expected_trades, selected.expected_volume, rows);
                day.official = selected.official;
                if (depth) {
                    attach_day_depth(http, code, selected.greg, day);
                }
                return day;
            }
            std::vector<json> rows;
            try {
                rows = fetch_live_ticks(http, code);
            } catch (error const&) {
                rows.clear();
            }
            if (!rows.empty()) {
                auto day = day_from_rows(selected.jalali, "live", rows);
                day.greg = selected.greg;
                if (depth) {
                    attach_live_depth(http, code, day);
                }
                return day;
            }
            auto bars = fetch_live_bars(http, code);
            if (bars.empty()) {
                return std::nullopt;
            }
            day_data day;
            day.date            = selected.jalali;
            day.greg            = selected.greg;
            day.source          = "live";
            day.native_interval = 2;
            day.bars            = std::move(bars);
            if (depth) {
                attach_live_depth(http, code, day);
            }
            return day;
        }

        // ------------------------------------------------------------------ session selection

        struct [[nodiscard]] today_info {
            std::string greg;
            std::string jalali;
            bool        trading = false;
        };

        struct [[nodiscard]] selected_sessions {
            std::vector<entry> entries;
            std::optional<int> wanted;
        };

        [[nodiscard]] std::pair<std::string, std::string> parse_jalali(
          std::string const& text,
          std::string_view   label) {
            static std::regex const pattern(R"(^(\d{4})[/\-.](\d{1,2})[/\-.](\d{1,2})$)");
            auto const              normalized = normal(text);
            std::smatch             match;
            if (!std::regex_match(normalized, match, pattern)) {
                throw error(std::format("{}: invalid Jalali date '{}' (expected YYYY/MM/DD)", label, text));
            }
            int const year  = std::stoi(match[1].str());
            int const month = std::stoi(match[2].str());
            int const day   = std::stoi(match[3].str());
            if (month < 1 || month > 12 || day < 1 || day > 31) {
                throw error(std::format("{}: invalid Jalali date '{}'", label, text));
            }
            auto const gregorian = jalali_to_gregorian(year, month, day);
            return {std::format("{:04d}/{:02d}/{:02d}", year, month, day),
                    std::format("{:04d}{:02d}{:02d}", gregorian.year, gregorian.month, gregorian.day)};
        }

        [[nodiscard]] selected_sessions select_entries(
          std::vector<session_info> const& sessions,
          intraday_options const&          options,
          today_info const&                today) {
            std::map<std::string, session_info const*> by_greg;
            for (auto const& session : sessions) {
                by_greg[session.greg] = &session;
            }
            bool const live_ok = today.trading && !options.no_live;
            auto const is_live = [&](std::string const& greg) {
                return live_ok && greg == today.greg && !by_greg.contains(greg);
            };
            auto const make = [&](std::string greg, std::string jalali, bool live) {
                entry selected;
                selected.greg   = std::move(greg);
                selected.jalali = std::move(jalali);
                selected.live   = live;
                auto const it   = by_greg.find(selected.greg);
                if (it != by_greg.end()) {
                    selected.expected_trades = it->second->trades;
                    selected.expected_volume = it->second->volume;
                    selected.official        = it->second->official;
                }
                return selected;
            };
            auto const entry_of = [&](std::string const& greg, std::string const& jalali) {
                return make(greg, jalali, is_live(greg));
            };

            if (options.date) {
                auto const [jalali, greg] = parse_jalali(*options.date, "--date");
                if (by_greg.contains(greg)) {
                    return {{entry_of(greg, jalali)}, std::nullopt};
                }
                if (greg == today.greg) {
                    if (live_ok) {
                        return {{make(greg, jalali, true)}, std::nullopt};
                    }
                    throw error(
                      std::format("--date {}: today's session is only available live (remove --no-live)", jalali));
                }
                throw error(std::format("--date {}: not a trading session", jalali));
            }

            if (options.from || options.to) {
                std::optional<std::string> low_jalali;
                std::optional<std::string> high_jalali;
                if (options.from) {
                    low_jalali = parse_jalali(*options.from, "--from").first;
                }
                if (options.to) {
                    high_jalali = parse_jalali(*options.to, "--to").first;
                }
                if (low_jalali && high_jalali && *low_jalali > *high_jalali) {
                    throw error("--from must not be later than --to");
                }
                std::vector<entry> entries;
                for (auto const& session : sessions) {
                    if ((!low_jalali || session.jalali >= *low_jalali) &&
                        (!high_jalali || session.jalali <= *high_jalali))
                    {
                        entries.push_back(entry_of(session.greg, session.jalali));
                    }
                }
                if (live_ok && !by_greg.contains(today.greg) && (!low_jalali || today.jalali >= *low_jalali) &&
                    (!high_jalali || today.jalali <= *high_jalali))
                {
                    entries.push_back(make(today.greg, today.jalali, true));
                }
                if (entries.empty()) {
                    throw error("no trading sessions in the requested range");
                }
                return {std::move(entries), std::nullopt};
            }

            std::vector<entry> entries;
            entries.reserve(sessions.size() + 1);
            for (auto const& session : sessions) {
                entries.push_back(entry_of(session.greg, session.jalali));
            }
            if (live_ok && !by_greg.contains(today.greg)) {
                entries.push_back(make(today.greg, today.jalali, true));
            }
            return {std::move(entries), options.days > 0 ? std::optional<int>(options.days) : std::optional<int>(1)};
        }

        // ------------------------------------------------------------------ rendering

        [[nodiscard]] std::vector<std::string> bar_columns(bool collapsed, bool depth) {
            std::vector<std::string> columns;
            columns.reserve(bar_column_names.size() + depth_columns.size() + 1);
            for (auto const name : bar_column_names) {
                columns.emplace_back(name);
            }
            if (collapsed) {
                columns.emplace_back("bars");
            }
            if (depth) {
                auto const at = collapsed ? std::find(columns.begin(), columns.end(), "bars") : columns.end();
                columns.insert(at, depth_columns.begin(), depth_columns.end());
            }
            return columns;
        }

        [[nodiscard]] std::vector<json> bars_to_rows(std::vector<bar> const& bars) {
            std::vector<json> rows;
            rows.reserve(bars.size());
            for (auto const& value : bars) {
                json row             = json::object();
                row["from"]          = fmt_minute(value.minute);
                row["to"]            = fmt_minute(value.minute);
                row["open_rial"]     = to_json(value.open);
                row["high_rial"]     = to_json(value.high);
                row["low_rial"]      = to_json(value.low);
                row["close_rial"]    = to_json(value.close);
                row["vwap_rial"]     = to_json(bar_vwap(value));
                row["volume_shares"] = value.volume;
                row["trades"]        = to_json(value.trades);
                rows.push_back(std::move(row));
            }
            return rows;
        }

        [[nodiscard]] std::vector<json> collapsed_to_rows(std::vector<collapsed_bar> const& collapsed) {
            std::vector<json> rows;
            rows.reserve(collapsed.size());
            for (auto const& value : collapsed) {
                json row          = json::object();
                row["from"]       = fmt_minute(value.from);
                row["to"]         = fmt_minute(value.to);
                row["open_rial"]  = to_json(value.open);
                row["high_rial"]  = to_json(value.high);
                row["low_rial"]   = to_json(value.low);
                row["close_rial"] = to_json(value.close);
                row["vwap_rial"]  = to_json(
                  value.turnover && value.volume != 0
                    ? std::optional<long long>(
                        round_ll(static_cast<double>(*value.turnover) / static_cast<double>(value.volume)))
                    : std::nullopt);
                row["volume_shares"] = value.volume;
                row["trades"]        = to_json(value.trades);
                row["bars"]          = value.count;
                rows.push_back(std::move(row));
            }
            return rows;
        }

        [[nodiscard]] std::vector<json>
        book_entries(day_data const& day, std::vector<json> const& updates, std::optional<int> last_minute) {
            std::vector<json> entries;
            if (!updates.empty()) {
                if (!day.bars.empty()) {
                    auto auction = depth_summary(updates, minute_hhmmss(day.bars.front().minute));
                    if (!auction.is_null()) {
                        entries.push_back(json{
                          {   "kind",          "auction"},
                          {"summary", std::move(auction)}
                        });
                    }
                }
                auto const close_time = last_minute ? std::optional<int>(minute_hhmmss(*last_minute)) : std::nullopt;
                auto       close      = depth_summary(updates, close_time);
                if (!close.is_null() && (entries.empty() || close != get_or_null(entries.back(), "summary"))) {
                    entries.push_back(json{
                      {   "kind",          "close"},
                      {"summary", std::move(close)}
                    });
                }
                return entries;
            }
            if (!day.live_book.empty()) {
                auto live = depth_summary(day.live_book, std::nullopt);
                if (!live.is_null()) {
                    live["time"] = get_or_null(day.live_quote, "time");
                    entries.push_back(json{
                      {   "kind",          "live"},
                      {"summary", std::move(live)}
                    });
                }
            }
            return entries;
        }

        [[nodiscard]] std::string official_line(json const& official, json const& tape_vwap) {
            std::vector<std::string> pieces;
            auto const               close = as_int(get_or_null(official, "close"));
            if (close) {
                auto const close_time = get_or_null(official, "close_time");
                auto const stamp = truthy(close_time) ? std::format(" @ {}", scalar_text(close_time)) : std::string{};
                pieces.push_back(std::format("قیمت پایانی {}{}", fmt_int(*close), stamp));
            }
            auto const last = as_int(get_or_null(official, "last_price"));
            if (last) {
                std::string detail;
                auto const  change    = as_int(get_or_null(official, "change"));
                auto const  yesterday = as_int(get_or_null(official, "yesterday"));
                if (change.has_value()) {
                    detail = std::format(" ({:+}", *change);
                    if (yesterday && *yesterday != 0) {
                        detail += std::format(
                          ", {}% vs {}",
                          fmt_signed_fixed(static_cast<double>(*change) / static_cast<double>(*yesterday) * 100.0, 1),
                          fmt_int(*yesterday));
                    }
                    detail += ')';
                }
                pieces.push_back(std::format("last trade {}{}", fmt_int(*last), detail));
            }
            auto const vwap = as_int(tape_vwap);
            if (vwap) {
                pieces.push_back(std::format("tape VWAP {}", fmt_int(*vwap)));
            }
            auto const first = as_int(get_or_null(official, "first"));
            if (first) {
                pieces.push_back(std::format("first {}", fmt_int(*first)));
            }
            auto const low  = as_int(get_or_null(official, "low"));
            auto const high = as_int(get_or_null(official, "high"));
            if (low && high) {
                pieces.push_back(std::format("range {}–{}", fmt_int(*low), fmt_int(*high)));
            }
            auto const trades     = as_int(get_or_null(official, "trades"));
            auto const volume     = as_int(get_or_null(official, "volume"));
            auto const value_rial = as_int(get_or_null(official, "value_rial"));
            if (trades.has_value()) {
                pieces.push_back(std::format("{} trades", fmt_int(*trades)));
            }
            if (volume.has_value()) {
                pieces.push_back(std::format("{} shares", fmt_int(*volume)));
            }
            if (value_rial.has_value()) {
                pieces.push_back(std::format("{} rial", fmt_int(*value_rial)));
            }
            return pieces.empty() ? std::string{} : "Official: " + join(pieces, " · ");
        }

        [[nodiscard]] std::string depth_line(json const& summary, std::string_view kind, bool orders) {
            if (summary.is_null() || !summary.is_object() || summary.empty()) {
                return {};
            }
            std::vector<std::string> pieces;
            auto const               bid_rial = as_int(get_or_null(summary, "bid_rial"));
            if (bid_rial) {
                auto text = std::format("bid {} × {}",
                                        fmt_int(*bid_rial),
                                        fmt_int(as_int(get_or_null(summary, "bid_shares")).value_or(0)));
                if (orders) {
                    auto const count = as_int(get_or_null(summary, "bid_orders"));
                    if (count.has_value()) {
                        text += std::format(" ({} ord)", *count >= 9999 ? std::string(">=9,999") : fmt_int(*count));
                    }
                }
                pieces.push_back(std::move(text));
            }
            auto const ask_rial = as_int(get_or_null(summary, "ask_rial"));
            if (ask_rial) {
                auto text = std::format("ask {} × {}",
                                        fmt_int(*ask_rial),
                                        fmt_int(as_int(get_or_null(summary, "ask_shares")).value_or(0)));
                if (orders) {
                    auto const count = as_int(get_or_null(summary, "ask_orders"));
                    if (count.has_value()) {
                        text += std::format(" ({} ord)", *count >= 9999 ? std::string(">=9,999") : fmt_int(*count));
                    }
                }
                pieces.push_back(std::move(text));
            }
            auto const spread = as_int(get_or_null(summary, "spread_rial"));
            if (spread.has_value()) {
                pieces.push_back(std::format("spread {}", fmt_int(*spread)));
            }
            auto const bid_depth = as_int(get_or_null(summary, "bid_depth_shares"));
            auto const ask_depth = as_int(get_or_null(summary, "ask_depth_shares"));
            if (bid_depth.has_value() && ask_depth.has_value()) {
                pieces.push_back(std::format("5-level {} bid / {} ask", fmt_int(*bid_depth), fmt_int(*ask_depth)));
            }
            auto const imbalance = as_double(get_or_null(summary, "depth_imbalance_pct"));
            if (imbalance.has_value()) {
                pieces.push_back(std::format("imbalance {}%", fmt_signed_fixed(*imbalance, 1)));
            }
            if (pieces.empty()) {
                return {};
            }
            auto const time  = get_or_null(summary, "time");
            auto const stamp = truthy(time) ? std::format(" {}", scalar_text(time)) : std::string{};
            return std::format("Depth ({}{}): ", kind, stamp) + join(pieces, " · ");
        }

        [[nodiscard]] std::string session_line(json const& summary, json const& official, json const& live_quote) {
            if (summary.is_null() || !summary.is_object() || summary.empty()) {
                return {};
            }
            std::vector<std::string> pieces;
            auto const               first = as_int(get_or_null(summary, "first_rial"));
            if (first) {
                pieces.push_back(std::format("first {}", fmt_int(*first)));
            }
            auto const high = as_int(get_or_null(summary, "high_rial"));
            if (high) {
                auto const high_time = get_or_null(summary, "high_time");
                auto const stamp     = truthy(high_time) ? std::format(" @ {}", scalar_text(high_time)) : std::string{};
                pieces.push_back(std::format("high {}{}", fmt_int(*high), stamp));
            }
            auto const low = as_int(get_or_null(summary, "low_rial"));
            if (low) {
                auto const low_time = get_or_null(summary, "low_time");
                auto const stamp    = truthy(low_time) ? std::format(" @ {}", scalar_text(low_time)) : std::string{};
                pieces.push_back(std::format("low {}{}", fmt_int(*low), stamp));
            }
            auto const last = as_int(get_or_null(summary, "last_rial"));
            if (last) {
                auto       text     = std::format("last {}", fmt_int(*last));
                auto const location = as_double(get_or_null(summary, "close_location_pct"));
                if (location.has_value()) {
                    text += std::format(" (CLV {}%)", fmt_whole(*location));
                }
                pieces.push_back(std::move(text));
            }
            auto const vwap = as_int(get_or_null(summary, "vwap_rial"));
            if (vwap) {
                auto       text  = std::format("VWAP {}", fmt_int(*vwap));
                auto const share = as_double(get_or_null(summary, "above_vwap_pct"));
                if (share.has_value()) {
                    text += std::format(" ({}% of volume at/above)", fmt_whole(*share));
                }
                pieces.push_back(std::move(text));
            }
            json yesterday = get_or_null(official, "yesterday");
            if (!truthy(yesterday)) {
                yesterday = get_or_null(live_quote, "yesterday");
            }
            auto const yesterday_value = as_int(yesterday);
            if (first.has_value() && yesterday_value && *yesterday_value != 0) {
                pieces.push_back(std::format(
                  "gap {}% vs {}",
                  fmt_signed_fixed((static_cast<double>(*first) / static_cast<double>(*yesterday_value) - 1.0) * 100.0,
                                   1),
                  fmt_int(*yesterday_value)));
            }
            return pieces.empty() ? std::string{} : "Session: " + join(pieces, " · ");
        }

        [[nodiscard]] std::string structure_line(json const& summary) {
            if (summary.is_null() || !summary.is_object() || summary.empty()) {
                return {};
            }
            std::vector<std::string> pieces;
            auto const               ranges = get_or_null(summary, "opening_range");
            for (std::string_view const window : {"5m", "15m", "30m"}) {
                auto const band = get_or_null(ranges, window);
                if (band.is_object() && !band.empty()) {
                    auto const low  = as_int(get_or_null(band, "low_rial"));
                    auto const high = as_int(get_or_null(band, "high_rial"));
                    if (low && high) {
                        pieces.push_back(std::format("{} {}–{}", window, fmt_int(*low), fmt_int(*high)));
                    }
                }
            }
            auto const area = get_or_null(summary, "value_area");
            if (area.is_object() && !area.empty()) {
                auto const poc    = as_int(get_or_null(area, "poc_rial"));
                auto const val    = as_int(get_or_null(area, "val_rial"));
                auto const vah    = as_int(get_or_null(area, "vah_rial"));
                auto const levels = as_int(get_or_null(area, "levels"));
                auto const share  = as_double(get_or_null(area, "share_pct"));
                if (poc && val && vah) {
                    pieces.push_back(std::format(
                      "POC {} · 70% band {}–{} ({} levels, {}% of volume)",
                      fmt_int(*poc),
                      fmt_int(*val),
                      fmt_int(*vah),
                      fmt_int(levels.value_or(0)),
                      fmt_whole(share.value_or(0.0))));
                }
            }
            return pieces.empty() ? std::string{} : "Structure: " + join(pieces, " · ");
        }

        [[nodiscard]] std::string prints_line(json const& summary) {
            auto const prints = get_or_null(summary, "largest_prints");
            if (!prints.is_array() || prints.empty()) {
                return {};
            }
            std::vector<std::string> pieces;
            for (auto const& item : prints) {
                auto const price  = as_int(get_or_null(item, "price_rial"));
                auto const volume = as_int(get_or_null(item, "volume_shares"));
                if (!price.has_value() || !volume.has_value()) {
                    continue;
                }
                pieces.push_back(std::format(
                  "{} {} × {} ({} rial)",
                  scalar_text(get_or_null(item, "time")),
                  fmt_int(*price),
                  fmt_int(*volume),
                  format_rial(as_double(get_or_null(item, "value_rial")))));
            }
            return pieces.empty() ? std::string{} : "Largest prints: " + join(pieces, " · ");
        }

        [[nodiscard]] std::string limit_distance_line(json const& quote, json const& limits) {
            if (quote.is_null() || limits.is_null() || !quote.is_object() || !limits.is_object()) {
                return {};
            }
            auto const last = as_int(get_or_null(quote, "last_price"));
            if (!last) {
                return {};
            }
            auto const               low  = as_int(get_or_null(limits, "low_rial"));
            auto const               high = as_int(get_or_null(limits, "high_rial"));
            std::vector<std::string> pieces;
            if (high && *high != 0) {
                if (*last >= *high) {
                    pieces.emplace_back("at the high limit");
                } else {
                    pieces.push_back(std::format(
                      "{}% below the high limit",
                      fmt_fixed(
                        (static_cast<double>(*high) - static_cast<double>(*last)) / static_cast<double>(*high) * 100.0,
                        1)));
                }
            }
            if (low && *low != 0) {
                if (*last <= *low) {
                    pieces.emplace_back("at the low limit");
                } else {
                    pieces.push_back(std::format(
                      "{}% above the low limit",
                      fmt_fixed(
                        (static_cast<double>(*last) - static_cast<double>(*low)) / static_cast<double>(*low) * 100.0,
                        1)));
                }
            }
            return pieces.empty() ? std::string{}
                                  : std::format("Limits: last {} is ", fmt_int(*last)) + join(pieces, " and ");
        }

        [[nodiscard]] std::string stats_line(json const& stats, json const& session) {
            if (stats.is_null() || !stats.is_object() || stats.empty()) {
                return {};
            }
            std::vector<std::string> pieces;
            auto const               average = as_double(get_or_null(stats, "avg_volume_3m_shares"));
            if (average.has_value()) {
                auto       text  = std::format("3-mo avg {} shares/day", format_shares(average));
                auto const older = as_double(get_or_null(stats, "avg_volume_12m_shares"));
                if (older.has_value()) {
                    text += std::format(" (12-mo {})", format_shares(older));
                }
                pieces.push_back(std::move(text));
            }
            auto const trades = as_double(get_or_null(stats, "avg_trades_3m"));
            if (trades.has_value()) {
                pieces.push_back(std::format("{} trades/day", fmt_whole(*trades)));
            }
            auto const buyers = as_double(get_or_null(stats, "avg_individual_buyers_3m"));
            if (buyers.has_value()) {
                pieces.push_back(std::format("{} real buyers/day", fmt_whole(*buyers)));
            }
            auto const volume = as_double(get_or_null(session, "volume"));
            if (session.is_object() && !session.empty() && volume.has_value()) {
                auto       text           = std::format("session {}", format_shares(volume));
                auto const session_trades = as_double(get_or_null(session, "trades"));
                if (average && *average != 0.0) {
                    text += std::format(" ({}x)", fmt_fixed(*volume / *average, 2));
                }
                if (session_trades.has_value()) {
                    text += std::format(", {} trades", fmt_int(round_ll(*session_trades)));
                }
                auto const session_buyers = as_double(get_or_null(session, "buyers"));
                if (session_buyers.has_value()) {
                    text += std::format(", {} real buyers", fmt_int(round_ll(*session_buyers)));
                }
                pieces.push_back(std::move(text));
            }
            auto const value = as_double(get_or_null(stats, "company_value_rial"));
            if (value.has_value()) {
                auto       text = std::format("company value {} rial", format_rial(value));
                auto const rank = as_double(get_or_null(stats, "company_value_rank"));
                if (rank.has_value()) {
                    text += std::format(" (#{} by value)", fmt_whole(*rank));
                }
                pieces.push_back(std::move(text));
            }
            return pieces.empty() ? std::string{} : "Context: " + join(pieces, " · ");
        }

        [[nodiscard]] std::string index_line(json const& overview) {
            if (overview.is_null() || !overview.is_object() || overview.empty()) {
                return {};
            }
            std::vector<std::string> pieces;
            auto const               value = as_double(get_or_null(overview, "index_value"));
            if (value.has_value()) {
                auto       text   = std::format("TEDPIX {}", fmt_fixed(*value, 2));
                auto const change = as_double(get_or_null(overview, "index_change_pct"));
                if (change.has_value()) {
                    text += std::format(" {}%", fmt_signed_fixed(*change, 2));
                }
                pieces.push_back(std::move(text));
            }
            auto const equal = as_double(get_or_null(overview, "equal_weighted_change_pct"));
            if (equal.has_value()) {
                pieces.push_back(std::format("equal-weighted {}%", fmt_signed_fixed(*equal, 2)));
            }
            std::vector<std::string> bits;
            auto const               trades = as_double(get_or_null(overview, "trades"));
            if (trades.has_value()) {
                bits.push_back(std::format("{} trades", fmt_whole(*trades)));
            }
            auto const volume = as_double(get_or_null(overview, "volume_shares"));
            if (volume.has_value()) {
                bits.push_back(std::format("{} shares", format_shares(volume)));
            }
            auto const value_rial = as_double(get_or_null(overview, "value_rial"));
            if (value_rial.has_value()) {
                bits.push_back(std::format("{} rial", format_rial(value_rial)));
            }
            if (!bits.empty()) {
                pieces.push_back("market " + join(bits, ", "));
            }
            auto const state = get_or_null(overview, "state");
            if (truthy(state)) {
                pieces.push_back(scalar_text(state));
            }
            if (pieces.empty()) {
                return {};
            }
            auto const time  = get_or_null(overview, "time");
            auto const stamp = truthy(time) ? std::format(" {}", scalar_text(time)) : std::string{};
            return std::format("Market{}: ", stamp) + join(pieces, " · ");
        }

        [[nodiscard]] std::string breadth_line(json const& breadth) {
            if (breadth.is_null() || !breadth.is_object() || breadth.empty()) {
                return {};
            }
            std::vector<std::string> pieces;
            pieces.push_back(std::format(
              "{} up / {} down / {} flat",
              fmt_int(as_int(get_or_null(breadth, "advancers")).value_or(0)),
              fmt_int(as_int(get_or_null(breadth, "decliners")).value_or(0)),
              fmt_int(as_int(get_or_null(breadth, "unchanged")).value_or(0))));
            auto const median = as_double(get_or_null(breadth, "median_change_pct"));
            if (median.has_value()) {
                pieces.push_back(std::format("median {}%", fmt_signed_fixed(*median, 2)));
            }
            auto const own = as_double(get_or_null(breadth, "instrument_change_pct"));
            if (own.has_value()) {
                auto       text       = std::format("this stock {}%", fmt_signed_fixed(*own, 2));
                auto const percentile = as_double(get_or_null(breadth, "instrument_percentile"));
                if (percentile.has_value()) {
                    text += std::format(" (above {}% of {} quoted)",
                                        fmt_whole(*percentile),
                                        fmt_int(as_int(get_or_null(breadth, "instruments")).value_or(0)));
                }
                pieces.push_back(std::move(text));
            }
            auto const tracked = as_int(get_or_null(breadth, "tracked"));
            if (tracked) {
                pieces.push_back(
                  std::format("tracked index components only ({} symbols, not the whole market)", fmt_int(*tracked)));
            }
            auto label = scalar_text(get_or_null(breadth, "date"));
            if (label.empty()) {
                label = "large-cap proxy";
            }
            return std::format("Breadth ({}): ", label) + join(pieces, " · ");
        }

        [[nodiscard]] std::string csv_field(std::string text) {
            if (text.find_first_of(",\"\n\r") == std::string::npos) {
                return text;
            }
            std::string out;
            out.reserve(text.size() + 2);
            out.push_back('"');
            for (char const c : text) {
                if (c == '"') {
                    out.push_back('"');
                }
                out.push_back(c);
            }
            out.push_back('"');
            return out;
        }

        [[nodiscard]] std::string rstrip(std::string text) {
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' ||
                                     text.back() == '\r' || text.back() == '\f' || text.back() == '\v'))
            {
                text.pop_back();
            }
            return text;
        }

        [[nodiscard]] std::string csv_text(json const& columns, json const& rows) {
            std::string out;
            bool        first = true;
            for (auto const& column : columns) {
                if (!first) {
                    out.push_back(',');
                }
                out   += csv_field(scalar_text(column));
                first  = false;
            }
            out.push_back('\n');
            for (auto const& row : rows) {
                first = true;
                for (auto const& column : columns) {
                    if (!first) {
                        out.push_back(',');
                    }
                    auto const key  = scalar_text(column);
                    out            += csv_field(scalar_text(get_or_null(row, key)));
                    first           = false;
                }
                out.push_back('\n');
            }
            return rstrip(std::move(out));
        }

        [[nodiscard]] std::string render_markdown(json const& result) {
            std::vector<std::string> lines;
            lines.push_back(std::format("Fetched at: {}", scalar_text(get_or_null(result, "fetched_at"))));
            lines.push_back(std::format("Market session open by schedule: {}",
                                        scalar_text(get_or_null(result, "market_session_open"))));
            lines.push_back("Prices and traded value are in rial, volume in shares.");
            lines.push_back(
              "Not published by TSETMC: per-minute حقیقی/حقوقی (only the day-level breakdown shown below), "
              "closing-auction detail (قیمت پایانی is a volume-weighted average, not an auction print), and "
              "per-instrument NAV history (only the current NAV is exposed).");
            if (truthy(get_or_null(result, "depth"))) {
                lines.push_back(
                  "Order-book depth: 5 levels only, from TSETMC's level-update stream; published per day after the "
                  "close (the live session shows the current snapshot) and available for roughly the last two months.");
            }
            if (truthy(get_or_null(result, "flow"))) {
                lines.push_back(
                  "Buy/sell-initiated volume is an estimate: TSETMC publishes no aggressor flag, so each trade is "
                  "classified against the best bid/ask at that moment and the leftovers are reported, not guessed.");
            }
            auto const mode = scalar_text(get_or_null(result, "mode"));
            if (mode == "ticks") {
                lines.push_back("Rows: every trade (raw tape)");
            } else if (mode == "profile") {
                lines.push_back("Rows: price-level volume profile");
            } else if (mode == "ladder") {
                lines.push_back(std::format(
                  "Rows: 5-level order book at {}-minute boundaries (the cumulative columns "
                  "carry the running depth)",
                  scalar_text(get_or_null(result, "interval_minutes"))));
            } else {
                std::vector<std::string> parts;
                parts.push_back(
                  std::format("{}-minute OHLCV bars", scalar_text(get_or_null(result, "interval_minutes"))));
                parts.emplace_back(truthy(get_or_null(result, "interval_auto")) ? "auto interval" : "fixed interval");
                parts.emplace_back(truthy(get_or_null(result, "collapsed")) ? "collapsed" : "not collapsed");
                lines.push_back("Rows: " + join(parts, ", "));
                if (truthy(get_or_null(result, "collapsed"))) {
                    lines.push_back(
                      "Collapsed: consecutive identical-OHLC bars merged; the `bars` column is how many "
                      "bars each row merged (1 = untouched).");
                }
            }
            auto const interval_note = get_or_null(result, "interval_note");
            if (truthy(interval_note)) {
                lines.push_back(scalar_text(interval_note));
            }
            auto const live_note = get_or_null(result, "live_note");
            if (truthy(live_note)) {
                lines.push_back(scalar_text(live_note));
            }
            auto const limits = get_or_null(result, "limits");
            if (truthy(limits)) {
                lines.push_back(std::format("Price limits (today): {}–{} (static daily band; not historical)",
                                            fmt_int(as_int(get_or_null(limits, "low_rial")).value_or(0)),
                                            fmt_int(as_int(get_or_null(limits, "high_rial")).value_or(0))));
            }
            auto line = stats_line(get_or_null(result, "statistics"), get_or_null(result, "latest_session"));
            if (!line.empty()) {
                lines.push_back(std::move(line));
            }
            line = index_line(get_or_null(result, "market"));
            if (!line.empty()) {
                lines.push_back(std::move(line));
            }
            auto const               skipped = get_or_null(get_or_null(result, "extras"), "skipped");
            std::vector<std::string> informative;
            if (skipped.is_object()) {
                for (auto it = skipped.begin(); it != skipped.end(); ++it) {
                    auto const reason = scalar_text(it.value());
                    if (reason != "off") {
                        informative.push_back(std::format("{} ({})", it.key(), reason));
                    }
                }
            }
            if (!informative.empty()) {
                lines.push_back("Skipped extras: " + join(informative, "; "));
            }
            auto const instrument = get_or_null(result, "instrument");
            auto       label      = scalar_text(get_or_null(instrument, "symbol"));
            if (label.empty()) {
                label = scalar_text(get_or_null(instrument, "name"));
            }
            if (label.empty()) {
                label = scalar_text(get_or_null(instrument, "ins_code"));
            }
            for (auto const& day : get_or_null(result, "days")) {
                auto const source = scalar_text(get_or_null(day, "source"));
                lines.emplace_back();
                lines.push_back(
                  std::format("## {} — {}  (source: {})", label, scalar_text(get_or_null(day, "date")), source));
                if (mode == "bars" && as_int(get_or_null(day, "native_interval")).value_or(0) == 2) {
                    lines.push_back(
                      "Bar resolution: 2-minute (TSETMC's native intraday fallback; per-bar trade count "
                      "is not published)");
                }
                auto const official = get_or_null(day, "official");
                if (truthy(official)) {
                    auto official_text = official_line(official, get_or_null(day, "tape_vwap"));
                    if (!official_text.empty()) {
                        lines.push_back(std::move(official_text));
                    }
                }
                for (auto text : {session_line(get_or_null(day, "session"), official, get_or_null(day, "live_quote")),
                                  structure_line(get_or_null(day, "session")),
                                  prints_line(get_or_null(day, "session"))})
                {
                    if (!text.empty()) {
                        lines.push_back(std::move(text));
                    }
                }
                for (auto const& book_entry : get_or_null(day, "book")) {
                    auto text = depth_line(get_or_null(book_entry, "summary"),
                                           scalar_text(get_or_null(book_entry, "kind")),
                                           truthy(get_or_null(result, "orders")));
                    if (!text.empty()) {
                        lines.push_back(std::move(text));
                    }
                }
                auto text = breadth_line(get_or_null(day, "breadth"));
                if (!text.empty()) {
                    lines.push_back(std::move(text));
                }
                auto const client_type = get_or_null(day, "client_type");
                if (truthy(client_type)) {
                    lines.push_back("حقیقی/حقوقی (day): " + dump_compact(client_type));
                }
                auto const quote = get_or_null(day, "live_quote");
                if (truthy(quote)) {
                    std::vector<std::string> detail;
                    detail.push_back(std::format("Live quote: last {} @ {}",
                                                 py_str(get_or_null(quote, "last_price")),
                                                 py_str(get_or_null(quote, "time"))));
                    if (!get_or_null(quote, "trades").is_null()) {
                        detail.push_back(std::format("{} trades", scalar_text(get_or_null(quote, "trades"))));
                    }
                    if (!get_or_null(quote, "volume").is_null()) {
                        detail.push_back(std::format("{} shares", scalar_text(get_or_null(quote, "volume"))));
                    }
                    if (!get_or_null(quote, "low").is_null() && !get_or_null(quote, "high").is_null()) {
                        detail.push_back(std::format("day {}–{}",
                                                     scalar_text(get_or_null(quote, "low")),
                                                     scalar_text(get_or_null(quote, "high"))));
                    }
                    if (!get_or_null(quote, "vwap").is_null()) {
                        detail.push_back(std::format("قیمت پایانی {}", scalar_text(get_or_null(quote, "vwap"))));
                    }
                    auto const change    = as_int(get_or_null(quote, "change"));
                    auto const yesterday = as_int(get_or_null(quote, "yesterday"));
                    if (change.has_value() && yesterday && *yesterday != 0) {
                        detail.push_back(std::format("{} vs {}", fmt_signed(*change), fmt_int(*yesterday)));
                    }
                    auto       quote_text = join(detail, " · ");
                    auto const lag        = as_int(get_or_null(quote, "lag_minutes"));
                    if (lag) {
                        quote_text += std::format(" — tape through {} ({}m behind)",
                                                  scalar_text(get_or_null(quote, "tape_time")),
                                                  *lag);
                    }
                    lines.push_back(std::move(quote_text));
                    auto limit_text = limit_distance_line(quote, limits);
                    if (!limit_text.empty()) {
                        lines.push_back(std::move(limit_text));
                    }
                }
                lines.push_back("```csv");
                lines.push_back(csv_text(get_or_null(day, "columns"), get_or_null(day, "rows")));
                lines.push_back("```");
            }
            auto const errors = get_or_null(result, "errors");
            if (errors.is_object() && !errors.empty()) {
                lines.emplace_back();
                lines.push_back("## Missing or failed sources / داده‌های ناموجود");
                lines.push_back("```json");
                lines.push_back(dump_compact(errors));
                lines.push_back("```");
            }
            return join(lines, "\n");
        }

    } // namespace

    std::string intraday_markdown(std::string const& symbol, intraday_options const& options) {
        int const days = options.days == 0 ? 1 : options.days;
        if (days < 1 || days > max_days) {
            throw error(std::format("--days must be between 1 and {}", max_days));
        }
        if (options.from.has_value() != options.to.has_value()) {
            throw error("--from and --to must be used together");
        }
        if (options.date && (options.from || options.to)) {
            throw error("--date cannot be combined with --from/--to");
        }
        if (options.date && options.days > 0) {
            throw error("--date cannot be combined with --days");
        }
        if (options.interval && *options.interval < 1) {
            throw error("--interval must be 'auto' or a positive integer");
        }

        client     http;
        auto const now = tehran_now();

        json        instrument = resolve_one(http, symbol);
        std::string code       = scalar_text(get_or_null(instrument, "ins_code"));
        if (code.empty()) {
            throw error("instrument has no ins_code");
        }
        if (scalar_text(get_or_null(instrument, "symbol")).empty()) {
            try {
                auto const ident = identity(http, code);
                for (auto it = ident.begin(); it != ident.end(); ++it) {
                    if (truthy(it.value())) {
                        instrument[it.key()] = it.value();
                    }
                }
            } catch (error const&) { // NOLINT(bugprone-empty-catch): optional identity enrichment.
            }
        }

        auto const sessions = fetch_sessions(http, code);
        if (sessions.empty()) {
            throw error("no trading sessions found for this instrument");
        }

        auto const        clock      = tehran_parts(now);
        std::string const today_greg = std::format("{:04d}{:02d}{:02d}", clock.year, clock.month, clock.day);
        today_info const  today{
          today_greg,
          trade_date(json(today_greg)),
          clock.weekday == 6 || clock.weekday == 0 || clock.weekday == 1 || clock.weekday == 2 || clock.weekday == 3};

        auto const [entries, wanted] = select_entries(sessions, options, today);
        if (entries.empty()) {
            throw error("no sessions selected");
        }

        json errors    = json::object();
        auto set_error = [&errors](std::string const& key, std::string const& value) {
            errors[key] = value;
        };

        json limits = nullptr;
        try {
            limits = price_limits(http, code);
        } catch (error const& exc) {
            set_error("price_limits", exc.what());
        }
        json client_types = json::object();
        try {
            client_types = client_type_history(http, code);
        } catch (error const& exc) {
            set_error("client_type_history", exc.what());
        }

        // The book extra is on by default, so depth is sampled on every bar.
        bool const extras_book    = true;
        bool const extras_orders  = true;
        bool const extras_opening = true;
        bool const extras_stats   = true;
        bool const extras_flow    = false;
        bool const depth          = extras_book || extras_flow || options.depth;

        std::map<int, std::optional<day_data>> loaded;
        auto const                             load_one = [&](int index) {
            auto const& selected = entries[static_cast<std::size_t>(index)];
            try {
                loaded[index] = load_day(http, code, selected, depth);
            } catch (error const& exc) {
                loaded[index] = std::nullopt;
                set_error(selected.jalali, exc.what());
            }
        };

        std::vector<day_data> day_list;
        if (wanted) {
            int const start = std::max(0, static_cast<int>(entries.size()) - *wanted);
            for (int index = start; std::cmp_less(index, entries.size()); ++index) {
                load_one(index);
            }
            std::vector<int> have;
            for (auto const& [index, day] : loaded) {
                if (day) {
                    have.push_back(index);
                }
            }
            std::sort(have.begin(), have.end());
            int cursor = start - 1;
            while (std::cmp_less(have.size(), *wanted) && cursor >= 0) {
                load_one(cursor);
                if (loaded[cursor]) {
                    have.push_back(cursor);
                }
                --cursor;
            }
            std::sort(have.begin(), have.end());
            int const take = std::min<int>(*wanted, static_cast<int>(have.size()));
            for (int i = static_cast<int>(have.size()) - take; std::cmp_less(i, have.size()); ++i) {
                day_list.push_back(*loaded[have[static_cast<std::size_t>(i)]]);
            }
        } else {
            for (int index = 0; std::cmp_less(index, entries.size()); ++index) {
                load_one(index);
            }
            for (auto const& [index, day] : loaded) {
                if (day) {
                    day_list.push_back(*day);
                } else if (!errors.contains(entries[static_cast<std::size_t>(index)].jalali)) {
                    set_error(entries[static_cast<std::size_t>(index)].jalali, "no data");
                }
            }
        }

        if (day_list.empty()) {
            throw error("no intraday data available for the requested session(s)");
        }

        for (auto const& day : day_list) {
            if (truthy(day.check)) {
                set_error(day.date, std::format("tape vs daily aggregate: {}", scalar_text(day.check)));
            }
            if (day.depth_note) {
                set_error(std::format("{} (depth)", day.date), *day.depth_note);
            }
        }

        std::optional<std::string> live_note;
        auto const&                newest     = entries.back();
        bool const                 has_newest = std::any_of(day_list.begin(), day_list.end(), [&](day_data const& day) {
            return day.date == newest.jalali;
        });
        if (newest.live && !has_newest) {
            live_note = std::format("Live session ({}) has no data yet; showing the most recent completed sessions.",
                                    newest.jalali);
        }

        std::optional<std::size_t> live_index;
        for (std::size_t i = 0; i < day_list.size(); ++i) {
            if (day_list[i].source == "live") {
                live_index = i;
                break;
            }
        }
        if (live_index) {
            json quote = nullptr;
            try {
                quote = fetch_live_quote(http, code);
            } catch (error const& exc) {
                if (!errors.contains("live_quote")) {
                    set_error("live_quote", exc.what());
                }
            }
            if (!quote.is_null()) {
                std::optional<int> tape_minute;
                for (auto const& value : day_list[*live_index].bars) {
                    tape_minute = tape_minute ? std::max(*tape_minute, value.minute) : value.minute;
                }
                long long const lag = tape_minute ? as_int(get_or_null(quote, "minute")).value_or(0) - *tape_minute : 0;
                if (lag >= lag_warn_minutes) {
                    quote["tape_time"]   = fmt_minute(*tape_minute);
                    quote["lag_minutes"] = lag;
                }
                day_list[*live_index].live_quote = std::move(quote);
            }
        }

        std::map<std::string, std::string> notes;
        bool const extras_index = std::any_of(day_list.begin(), day_list.end(), [](day_data const& day) {
            return day.source == "live";
        });
        if (!extras_index) {
            notes["index"] = "the market index is published live only";
        }
        std::size_t published = 0;
        for (auto const& day : day_list) {
            if (day.source != "live") {
                ++published;
            }
        }
        bool extras_breadth = false;
        if (published == 0) {
            notes["breadth"] = "no published session selected";
        } else if (published > static_cast<std::size_t>(breadth_auto_max_days)) {
            notes["breadth"] = std::format("{} sessions x ~175 KB; use --with breadth", published);
        } else {
            extras_breadth = true;
        }

        json statistics = nullptr;
        if (extras_stats) {
            try {
                statistics = instrument_statistics(http, code);
            } catch (error const& exc) {
                set_error("statistics", exc.what());
            }
            if (statistics.is_null()) {
                notes["stats"] = "the exchange returned none for this instrument";
            }
        }

        json overview = nullptr;
        if (extras_index) {
            try {
                overview = market_overview(http);
            } catch (error const& exc) {
                set_error("market_overview", exc.what());
            }
            if (overview.is_null()) {
                notes["index"] = "the market overview returned nothing";
            }
        }

        std::map<std::string, json> breadth;
        if (extras_breadth) {
            for (auto const& day : day_list) {
                if (day.source == "live") {
                    continue;
                }
                try {
                    auto value = market_breadth_day(http, day.greg, code);
                    if (!value.is_null()) {
                        breadth[day.date] = std::move(value);
                    }
                } catch (error const& exc) {
                    set_error(std::format("{} (breadth)", day.date), exc.what());
                }
            }
            std::vector<std::string> missing;
            for (auto const& day : day_list) {
                if (day.source != "live" && !breadth.contains(day.date) &&
                    !errors.contains(std::format("{} (breadth)", day.date)))
                {
                    missing.push_back(day.date);
                }
            }
            if (!missing.empty()) {
                notes["breadth"] = "the exchange has not published whole-market rows for " + join(missing, ", ");
            }
        }

        bool                       interval_auto = false;
        std::optional<std::string> interval_note;
        int                        interval = 0;
        if (options.interval) {
            interval = *options.interval;
        } else {
            interval_auto = true;
        }

        long long total_minutes = 0;
        for (auto const& day : day_list) {
            total_minutes += static_cast<long long>(day.bars.size());
        }
        bool const collapse = total_minutes > default_collapse_threshold;

        if (interval_auto) {
            std::vector<std::vector<bar>> bars_per_day;
            bars_per_day.reserve(day_list.size());
            for (auto const& day : day_list) {
                bars_per_day.push_back(day.bars);
            }
            int const forced         = choose_interval(bars_per_day, default_max_rows, collapse);
            long long raw_one_minute = 0;
            for (auto const& day : day_list) {
                raw_one_minute += collapse ? static_cast<long long>(collapse_bars(day.bars).size())
                                           : static_cast<long long>(day.bars.size());
            }
            interval = forced;
            if (forced > 1) {
                auto const note = std::format(
                  "Auto interval: {}-minute to stay within {} rows (1-minute would have produced {} rows).",
                  forced,
                  default_max_rows,
                  raw_one_minute);
                interval_note = interval_note ? *interval_note + " " + note : note;
            }
        }

        std::vector<json> rendered;
        rendered.reserve(day_list.size());
        for (auto const& day : day_list) {
            auto const&        depth_updates = day.depth_updates;
            std::vector<json>  book;
            auto const         columns = bar_columns(collapse, options.depth);
            auto const         merged  = merge_bars(day.bars, interval);
            auto               rows    = collapse ? collapsed_to_rows(collapse_bars(merged)) : bars_to_rows(merged);
            std::optional<int> last_minute;
            for (auto const& value : day.bars) {
                last_minute = last_minute ? std::max(*last_minute, value.minute) : value.minute;
            }
            if (!depth_updates.empty() && !rows.empty()) {
                attach_depth(rows, depth_updates, interval, last_minute);
            }
            if (extras_book) {
                book = book_entries(day, depth_updates, last_minute);
                if (!extras_opening) {
                    std::erase_if(book, [](json const& item) {
                        return scalar_text(get_or_null(item, "kind")) == "auction";
                    });
                }
            }
            json client_type = get_or_null(client_types, day.date);
            if (day.source == "live" && client_type.is_null()) {
                try {
                    client_type = live_client_type(http, code);
                } catch (error const&) {
                    client_type = nullptr;
                }
            }
            json item                = json::object();
            item["date"]             = day.date;
            item["source"]           = day.source;
            item["interval_minutes"] = std::max(interval, day.native_interval > 0 ? day.native_interval : 1);
            item["native_interval"]  = day.native_interval;
            item["official"]         = day.official;
            item["tape_vwap"]        = to_json(session_vwap(day.bars));
            item["session"]          = day.session;
            item["book"]             = book;
            item["client_type"]      = client_type;
            item["breadth"]          = breadth.contains(day.date) ? breadth.at(day.date) : json(nullptr);
            item["flow"]             = nullptr;
            item["live_quote"]       = day.live_quote;
            item["columns"]          = columns;
            item["rows"]             = rows;
            rendered.push_back(std::move(item));
        }

        json latest_session = nullptr;
        if (live_index) {
            auto const quote         = day_list[*live_index].live_quote;
            json       live_rendered = nullptr;
            for (auto const& item : rendered) {
                if (scalar_text(get_or_null(item, "source")) == "live") {
                    live_rendered = item;
                    break;
                }
            }
            auto const buyers = get_or_null(get_or_null(live_rendered, "client_type"), "individual_buyers");
            latest_session    = json{
              {"volume", get_or_null(quote, "volume")},
              {"trades", get_or_null(quote, "trades")},
              {"buyers", buyers}
            };
        } else if (!statistics.is_null()) {
            for (auto const& item : rendered) {
                auto const official = get_or_null(item, "official");
                auto const volume   = get_or_null(official, "volume");
                auto const expected = get_or_null(statistics, "last_volume_shares");
                if (volume.is_number() && expected.is_number() && volume == expected) {
                    auto const buyers = get_or_null(get_or_null(item, "client_type"), "individual_buyers");
                    latest_session    = json{
                      {"volume", get_or_null(official, "volume")},
                      {"trades", get_or_null(official, "trades")},
                      {"buyers", buyers}
                    };
                    break;
                }
            }
        }

        std::vector<std::string> used;
        bool const               any_book = std::any_of(rendered.begin(), rendered.end(), [](json const& item) {
            return truthy(get_or_null(item, "book"));
        });
        if (any_book) {
            used.emplace_back("book");
            bool const any_auction = std::any_of(rendered.begin(), rendered.end(), [](json const& item) {
                for (auto const& entry : get_or_null(item, "book")) {
                    if (scalar_text(get_or_null(entry, "kind")) == "auction") {
                        return true;
                    }
                }
                return false;
            });
            if (any_auction) {
                used.emplace_back("opening");
            }
            if (extras_orders) {
                used.emplace_back("orders");
            }
        }
        if (!statistics.is_null()) {
            used.emplace_back("stats");
        }
        if (!overview.is_null()) {
            used.emplace_back("index");
        }
        if (std::any_of(rendered.begin(),
                        rendered.end(),
                        [](json const& item) {
                            return truthy(get_or_null(item, "breadth"));
                        }))
        {
            used.emplace_back("breadth");
        }
        if (std::any_of(rendered.begin(),
                        rendered.end(),
                        [](json const& item) {
                            return truthy(get_or_null(item, "flow"));
                        }))
        {
            used.emplace_back("flow");
        }
        json skipped = json::object();
        for (auto const name : extra_names) {
            if (std::find(used.begin(), used.end(), name) == used.end()) {
                auto const it              = notes.find(std::string(name));
                skipped[std::string(name)] = it != notes.end() ? json(it->second) : json("off");
            }
        }

        json result                   = json::object();
        result["fetched_at"]          = jalali_stamp(now);
        result["market_session_open"] = market_session_open(now);
        result["instrument"]          = instrument;
        result["mode"]                = "bars";
        result["depth"]               = depth;
        result["orders"]              = extras_orders;
        result["flow"]                = std::find(used.begin(), used.end(), "flow") != used.end();
        result["interval_minutes"]    = interval;
        result["interval_auto"]       = interval_auto;
        result["interval_note"]       = interval_note ? json(*interval_note) : json(nullptr);
        result["live_note"]           = live_note ? json(*live_note) : json(nullptr);
        result["collapsed"]           = collapse;
        result["collapse_mode"]       = "auto";
        result["collapse_threshold"]  = default_collapse_threshold;
        result["statistics"]          = statistics;
        result["market"]              = overview;
        result["latest_session"]      = latest_session;
        result["limits"]              = limits;
        result["extras"]              = json{
          {   "used",    used},
          {"skipped", skipped}
        };
        result["days"]   = rendered;
        result["errors"] = errors;
        return render_markdown(result);
    }

} // namespace dana::tools::tse
