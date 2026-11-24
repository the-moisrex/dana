#include "tse_market.hpp"

#include "ai/http.hpp"
#include "tse_resolver.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <format>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dana::tools::tse {

    namespace {

        [[nodiscard]] json field(json const& object, std::string_view key) {
            if (!object.is_object()) {
                return nullptr;
            }
            auto const it = object.find(std::string(key));
            return it == object.end() ? json(nullptr) : *it;
        }

        [[nodiscard]] std::string text_of(json const& value) {
            return scalar_text(value);
        }

        [[nodiscard]] std::string field_text(json const& object, std::string_view key) {
            return text_of(field(object, key));
        }

        [[nodiscard]] double num_or(json const& value, double fallback = 0.0) {
            return as_double(value).value_or(fallback);
        }

        [[nodiscard]] json round2(double value) {
            return round_to(value, 2);
        }

        [[nodiscard]] json round1(double value) {
            return round_to(value, 1);
        }

        [[nodiscard]] double median(std::vector<double> values) {
            if (values.empty()) {
                return 0.0;
            }
            std::sort(values.begin(), values.end());
            auto const n = values.size();
            if (n % 2 == 1) {
                return values[n / 2];
            }
            return (values[n / 2 - 1] + values[n / 2]) / 2.0;
        }

        [[nodiscard]] double sample_stdev(std::vector<double> const& values) {
            if (values.size() < 2) {
                return 0.0;
            }
            double const mean = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
            double       sum  = 0.0;
            for (double const value : values) {
                sum += (value - mean) * (value - mean);
            }
            return std::sqrt(sum / static_cast<double>(values.size() - 1));
        }

        // ------------------------------------------------------------ identity

        [[nodiscard]] json identity_impl(client& http, std::string const& code) {
            auto const row = http.api(std::format("Instrument/GetInstrumentIdentity/{}", code), "instrumentIdentity");
            if (!row.is_object()) {
                throw error("No instrument identity");
            }
            json out;
            out["symbol"]       = normal(text_of(pick(row, {"lVal18AFC", "lVal18"})));
            out["name"]         = normal(text_of(pick(row, {"lVal30"})));
            out["isin"]         = pick(row, {"cIsin"});
            out["industry"]     = pick(pick(row, {"sector"}), {"lSecVal"});
            out["sub_industry"] = pick(pick(row, {"subSector"}), {"lSoSecVal"});
            out["board"]        = pick(row, {"cgrValCotTitle"});
            out["share_count"]  = value_of(row, {"shareCount", "zTitad"});
            return out;
        }

        [[nodiscard]] json quote_impl(client& http, std::string const& code) {
            auto const row = http.api(std::format("ClosingPrice/GetClosingPriceInfo/{}", code), "closingPriceInfo");
            if (!row.is_object()) {
                throw error("No quote for this instrument");
            }
            auto const state     = pick(row, {"instrumentState"});
            auto const last      = price(pick(row, {"pDrCotVal", "pl"}));
            auto const yesterday = price(pick(row, {"priceYesterday", "py"}));
            json       out;
            out["ins_code"]   = code;
            out["trade_date"] = trade_date(pick(row, {"dEven"}));
            out["trade_time"] = trade_time(pick(row, {"hEven"}));
            out["state"] =
              pick(state, {"cEtavalTitle"}).is_null() ? pick(row, {"cEtavalTitle"}) : pick(state, {"cEtavalTitle"});
            out["last_rial"]    = last;
            out["closing_rial"] = price(pick(row, {"pClosing", "pc"}));
            out["last_change_pct"] =
              (!last.is_null() && !yesterday.is_null() && num_or(yesterday) != 0.0)
                ? round2((num_or(last) / num_or(yesterday) - 1.0) * 100.0)
                : json(nullptr);
            out["yesterday_rial"]     = yesterday;
            out["first_rial"]         = price(pick(row, {"priceFirst", "pf"}));
            out["low_rial"]           = price(pick(row, {"priceMin", "pmn"}));
            out["high_rial"]          = price(pick(row, {"priceMax", "pmx"}));
            out["volume_shares"]      = value_of(row, {"qTotTran5J", "tvol"});
            out["value_rial"]         = value_of(row, {"qTotCap", "tval"});
            out["trade_count"]        = value_of(row, {"zTotTran", "tno"});
            out["base_volume_shares"] = value_of(row, {"baseVol"});
            out["eps_rial"]           = value_of(row, {"estimatedEPS", "eps"});
            out["pe"]                 = value_of(row, {"pe"});
            out["sector_pe"]          = value_of(row, {"sectorPE"});
            return out;
        }

        inline constexpr int book_levels = 5;

        [[nodiscard]] json book_impl(client& http, std::string const& code) {
            auto const rows = http.api(std::format("BestLimits/{}", code), "bestLimits");
            if (!rows.is_array()) {
                throw error("Unexpected order book result");
            }
            std::vector<json> levels;
            for (auto const& row : rows) {
                if (row.is_object()) {
                    levels.push_back(row);
                }
            }
            std::stable_sort(levels.begin(), levels.end(), [](json const& a, json const& b) {
                return num_or(value_of(a, {"number", "n"})) < num_or(value_of(b, {"number", "n"}));
            });
            json out = json::array();
            for (std::size_t i = 0; i < levels.size() && i < static_cast<std::size_t>(book_levels); ++i) {
                auto const& row = levels[i];
                out.push_back(json{
                  {            "level",   value_of(row, {"number", "n"})},
                  {   "bid_price_rial", price(pick(row,     {"pMeDem"}))},
                  {"bid_volume_shares",   value_of(row,   {"qTitMeDem"})},
                  {       "bid_orders",   value_of(row,   {"zOrdMeDem"})},
                  {   "ask_price_rial", price(pick(row,      {"pMeOf"}))},
                  {"ask_volume_shares",   value_of(row,    {"qTitMeOf"})},
                  {       "ask_orders",   value_of(row,    {"zOrdMeOf"})}
                });
            }
            return out;
        }

        // ------------------------------------------------------------ order-book stream

        inline constexpr int book_auction_seconds     = 9 * 3600;            // 09:00:00
        inline constexpr int book_close_seconds       = 12 * 3600 + 30 * 60; // 12:30:00
        inline constexpr int book_cover_slack_seconds = 15 * 60;             // 15 minutes

        [[nodiscard]] int clock_seconds(double stamp) noexcept {
            auto const hhmmss = static_cast<long long>(stamp);
            return static_cast<int>((hhmmss / 10'000) * 3600 + (hhmmss / 100 % 100) * 60 + hhmmss % 100);
        }

        [[nodiscard]] std::string clock_text(double stamp) {
            auto const hhmmss = static_cast<long long>(stamp);
            return std::format("{:02d}:{:02d}:{:02d}", hhmmss / 10'000, hhmmss / 100 % 100, hhmmss % 100);
        }

        [[nodiscard]] std::string clock_label(int seconds) {
            return std::format("{:02d}:{:02d}:{:02d}", seconds / 3600, seconds / 60 % 60, seconds % 60);
        }

        [[nodiscard]] json book_level(json const& row) {
            auto const level = value_of(row, {"number", "n"});
            if (level.is_null() || !(num_or(level) >= 1.0 && num_or(level) <= static_cast<double>(book_levels))) {
                return nullptr;
            }
            auto const stamp = value_of(row, {"hEven"});
            if (stamp.is_null()) {
                return nullptr;
            }
            return json{
              {"level", static_cast<long long>(num_or(level))},
              {"hhmmss", static_cast<long long>(num_or(stamp))},
              {"bid_rial", price(pick(row, {"pMeDem"}))},
              {"bid_shares", value_of(row, {"qTitMeDem"})},
              {"bid_orders", value_of(row, {"zOrdMeDem"})},
              {"ask_rial", price(pick(row, {"pMeOf"}))},
              {"ask_shares", value_of(row, {"qTitMeOf"})},
              {"ask_orders", value_of(row, {"zOrdMeOf"})}
            };
        }

        [[nodiscard]] std::vector<json>
        book_stream(client& http, std::string const& code, std::string const& greg, int attempts = 4) {
            auto const        url = std::format("{}/BestLimits/{}/{}", tsetmc_base, code, greg);
            std::vector<json> best;
            for (int attempt = 0; attempt < attempts; ++attempt) {
                json payload = nullptr;
                try {
                    payload = http.json_get(url);
                } catch (error const&) {
                    payload = nullptr;
                }
                std::vector<json> updates;
                if (payload.is_object()) {
                    auto const rows = field(payload, "bestLimitsHistory");
                    if (rows.is_array()) {
                        for (auto const& row : rows) {
                            if (!row.is_object()) {
                                continue;
                            }
                            auto update = book_level(row);
                            if (
                              !update.is_null() && clock_seconds(num_or(field(update, "hhmmss"))) <= book_close_seconds)
                            {
                                updates.push_back(std::move(update));
                            }
                        }
                    }
                }
                if (!updates.empty()) {
                    std::stable_sort(updates.begin(), updates.end(), [](json const& a, json const& b) {
                        auto const ta = num_or(field(a, "hhmmss"));
                        auto const tb = num_or(field(b, "hhmmss"));
                        if (ta != tb) {
                            return ta < tb;
                        }
                        return num_or(field(a, "level")) < num_or(field(b, "level"));
                    });
                    if (updates.size() > best.size()) {
                        best = updates;
                    }
                    if (clock_seconds(num_or(field(updates.back(), "hhmmss"))) + book_cover_slack_seconds >=
                        book_close_seconds)
                    {
                        return updates;
                    }
                } else if (attempt >= 1) {
                    break;
                }
                if (attempt < attempts - 1) {
                    http.drop(url);
                    std::this_thread::sleep_for(std::chrono::milliseconds(500 * (attempt + 1)));
                }
            }
            if (!best.empty()) {
                std::stable_sort(best.begin(), best.end(), [](json const& a, json const& b) {
                    auto const ta = num_or(field(a, "hhmmss"));
                    auto const tb = num_or(field(b, "hhmmss"));
                    if (ta != tb) {
                        return ta < tb;
                    }
                    return num_or(field(a, "level")) < num_or(field(b, "level"));
                });
            }
            return best;
        }

        [[nodiscard]] json book_snapshot(std::vector<json> const& updates, int seconds) {
            std::map<long long, json> state;
            for (auto const& update : updates) {
                if (clock_seconds(num_or(field(update, "hhmmss"))) <= seconds) {
                    state[static_cast<long long>(num_or(field(update, "level")))] = update;
                }
            }
            if (state.empty()) {
                return nullptr;
            }
            std::vector<json> ordered;
            for (auto const& [level, update] : state) {
                (void) level;
                ordered.push_back(update);
            }
            auto const& best      = ordered.front();
            long long   bid_depth = 0;
            long long   ask_depth = 0;
            for (auto const& item : ordered) {
                bid_depth += static_cast<long long>(num_or(field(item, "bid_shares")));
                ask_depth += static_cast<long long>(num_or(field(item, "ask_shares")));
            }
            long long const total    = bid_depth + ask_depth;
            auto const      bid_rial = field(best, "bid_rial");
            auto const      ask_rial = field(best, "ask_rial");
            json            out;
            out["time"]             = clock_label(seconds);
            out["bid_rial"]         = bid_rial;
            out["bid_shares"]       = field(best, "bid_shares");
            out["bid_orders"]       = field(best, "bid_orders");
            out["ask_rial"]         = ask_rial;
            out["ask_shares"]       = field(best, "ask_shares");
            out["ask_orders"]       = field(best, "ask_orders");
            out["spread_rial"]      = (!ask_rial.is_null() && !bid_rial.is_null())
                                        ? json(static_cast<long long>(num_or(ask_rial) - num_or(bid_rial)))
                                        : json(nullptr);
            out["bid_depth_shares"] = bid_depth;
            out["ask_depth_shares"] = ask_depth;
            out["depth_imbalance_pct"] =
              total != 0 ? round1(static_cast<double>(bid_depth - ask_depth) / static_cast<double>(total) * 100.0)
                         : json(nullptr);
            return out;
        }

        [[nodiscard]] json
        last_session_book(client& http, std::string const& code, std::string const& greg, std::string const& jalali) {
            auto const updates = book_stream(http, code, greg);
            if (updates.empty()) {
                return nullptr;
            }
            int const start = clock_seconds(num_or(field(updates.front(), "hhmmss")));
            int const end   = clock_seconds(num_or(field(updates.back(), "hhmmss")));
            json      result;
            result["date"]   = jalali;
            result["stream"] = std::format("{}\u2013{}",
                                           clock_text(num_or(field(updates.front(), "hhmmss"))),
                                           clock_text(num_or(field(updates.back(), "hhmmss"))));
            if (start <= book_auction_seconds) {
                auto const auction = book_snapshot(updates, book_auction_seconds);
                if (!auction.is_null()) {
                    result["auction"] = auction;
                }
            }
            if (end + book_cover_slack_seconds >= book_close_seconds) {
                auto const close = book_snapshot(updates, book_close_seconds);
                if (!close.is_null()) {
                    result["close"] = close;
                }
            }
            if (!result.contains("auction") && !result.contains("close")) {
                return nullptr;
            }
            if (end + book_cover_slack_seconds < book_close_seconds) {
                result["note"] = std::format(
                  "the level stream stops at {}; the exchange is still publishing this date, so there is no "
                  "close snapshot yet",
                  clock_text(num_or(field(updates.back(), "hhmmss"))));
            }
            return result;
        }

        [[nodiscard]] json flow_impl(client& http, std::string const& code) {
            auto row = http.api(std::format("ClientType/GetClientType/{}/1/0", code), "clientType");
            if (row.is_array()) {
                row = row.empty() ? json(nullptr) : row[0];
            }
            if (!row.is_object()) {
                throw error("No client-type data for this instrument");
            }
            auto const buy_n  = value_of(row, {"buy_N_Volume"});
            auto const sell_n = value_of(row, {"sell_N_Volume"});
            json       out;
            out["individual_buy_shares"]  = value_of(row, {"buy_I_Volume"});
            out["individual_sell_shares"] = value_of(row, {"sell_I_Volume"});
            out["legal_buy_shares"]       = buy_n;
            out["legal_sell_shares"]      = sell_n;
            out["legal_net_shares"] =
              (!buy_n.is_null() && !sell_n.is_null())
                ? json(static_cast<long long>(num_or(buy_n) - num_or(sell_n)))
                : json(nullptr);
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

        inline constexpr std::array<std::string_view, 5> client_type_volume_fields{
          "individual_buy_shares",
          "individual_sell_shares",
          "legal_buy_shares",
          "legal_sell_shares",
          "legal_net_shares"};
        inline constexpr std::array<std::string_view, 5> client_type_value_fields{
          "individual_buy_value_rial",
          "individual_sell_value_rial",
          "legal_buy_value_rial",
          "legal_sell_value_rial",
          "legal_net_value_rial"};
        inline constexpr std::array<std::string_view, 14> client_type_history_fields{
          "individual_buy_shares",
          "individual_sell_shares",
          "legal_buy_shares",
          "legal_sell_shares",
          "legal_net_shares",
          "individual_buy_value_rial",
          "individual_sell_value_rial",
          "legal_buy_value_rial",
          "legal_sell_value_rial",
          "legal_net_value_rial",
          "individual_buyers",
          "individual_sellers",
          "legal_buyers",
          "legal_sellers"};

        [[nodiscard]] json attach_session_values(json flow_row, json const& hist_rows) {
            if (flow_row.is_null() || !flow_row.is_object() || !hist_rows.is_array() || hist_rows.empty()) {
                return flow_row;
            }
            auto const& last = hist_rows.back();
            bool        same = true;
            for (auto const name : client_type_volume_fields) {
                auto const value = field(flow_row, name);
                if (value.is_null() || value != field(last, name)) {
                    same = false;
                    break;
                }
            }
            if (same) {
                for (auto const name : client_type_value_fields) {
                    if (field(flow_row, name).is_null()) {
                        flow_row[std::string(name)] = field(last, name);
                    }
                }
            }
            return flow_row;
        }

        [[nodiscard]] json client_type_history_impl(client& http, std::string const& code) {
            auto const rows = http.api(std::format("ClientType/GetClientTypeHistory/{}", code), "clientType");
            if (!rows.is_array()) {
                throw error("Unexpected client-type history result");
            }
            json out = json::object();
            for (auto const& row : rows) {
                if (!row.is_object()) {
                    continue;
                }
                auto const date = trade_date(pick(row, {"recDate"}));
                if (date.empty()) {
                    continue;
                }
                auto const buy_i        = value_of(row, {"buy_I_Volume"});
                auto const sell_i       = value_of(row, {"sell_I_Volume"});
                auto const buy_n        = value_of(row, {"buy_N_Volume"});
                auto const sell_n       = value_of(row, {"sell_N_Volume"});
                auto const buy_i_value  = value_of(row, {"buy_I_Value"});
                auto const sell_i_value = value_of(row, {"sell_I_Value"});
                auto const buy_n_value  = value_of(row, {"buy_N_Value"});
                auto const sell_n_value = value_of(row, {"sell_N_Value"});
                out[date]               = json{
                  {"individual_buy_shares", buy_i},
                  {"individual_sell_shares", sell_i},
                  {"legal_buy_shares", buy_n},
                  {"legal_sell_shares", sell_n},
                  {"legal_net_shares",
                   (!buy_n.is_null() && !sell_n.is_null())
                     ? json(static_cast<long long>(num_or(buy_n) - num_or(sell_n)))
                     : json(nullptr)},
                  {"individual_buy_value_rial", buy_i_value},
                  {"individual_sell_value_rial", sell_i_value},
                  {"legal_buy_value_rial", buy_n_value},
                  {"legal_sell_value_rial", sell_n_value},
                  {"legal_net_value_rial",
                   (!buy_n_value.is_null() && !sell_n_value.is_null())
                     ? json(static_cast<long long>(num_or(buy_n_value) - num_or(sell_n_value)))
                     : json(nullptr)},
                  {"individual_buyers", value_of(row, {"buy_I_Count"})},
                  {"individual_sellers", value_of(row, {"sell_I_Count"})},
                  {"legal_buyers", value_of(row, {"buy_N_Count"})},
                  {"legal_sellers", value_of(row, {"sell_N_Count"})}
                };
            }
            return out;
        }

        [[nodiscard]] json price_adjustments_impl(client& http, std::string const& code) {
            auto const rows = http.api(std::format("ClosingPrice/GetPriceAdjustList/{}", code), "priceAdjust");
            if (!rows.is_array()) {
                throw error("Unexpected price adjustments result");
            }
            std::vector<json> result;
            for (auto const& row : rows) {
                if (!row.is_object()) {
                    continue;
                }
                auto const adj  = price(pick(row, {"pClosing"}));
                auto const raw  = price(pick(row, {"pClosingNotAdjusted"}));
                auto const date = trade_date(pick(row, {"dEven"}));
                if (!adj.is_null() && !raw.is_null() && num_or(raw) != 0.0 && !date.empty()) {
                    result.push_back(json{
                      {        "date",                      date},
                      {"pClosing_adj",                       adj},
                      {"pClosing_raw",                       raw},
                      {   "adj_ratio", num_or(adj) / num_or(raw)}
                    });
                }
            }
            std::stable_sort(result.begin(), result.end(), [](json const& a, json const& b) {
                return field_text(a, "date") < field_text(b, "date");
            });
            return result;
        }

        [[nodiscard]] json share_changes_impl(client& http, std::string const& code) {
            auto const rows =
              http.api(std::format("Instrument/GetInstrumentShareChange/{}", code), "instrumentShareChange");
            if (!rows.is_array()) {
                throw error("Unexpected share changes result");
            }
            std::set<std::string> seen;
            std::vector<json>     result;
            for (auto const& row : rows) {
                if (!row.is_object()) {
                    continue;
                }
                auto const date = trade_date(pick(row, {"dEven"}));
                auto const old  = number(pick(row, {"numberOfShareOld"}));
                auto const new_ = number(pick(row, {"numberOfShareNew"}));
                if (!date.empty() && !old.is_null() && !new_.is_null() && num_or(old) > 0.0 && num_or(new_) > 0.0) {
                    auto const key = std::format("{}|{}|{}", date, text_of(old), text_of(new_));
                    if (!seen.insert(key).second) {
                        continue;
                    }
                    result.push_back(json{
                      {      "date",                       date},
                      {"old_shares",                        old},
                      {"new_shares",                       new_},
                      {     "ratio", num_or(old) / num_or(new_)}
                    });
                }
            }
            std::stable_sort(result.begin(), result.end(), [](json const& a, json const& b) {
                return field_text(a, "date") < field_text(b, "date");
            });
            return result;
        }

        [[nodiscard]] json
        adjust_history_impl(json const& unadjusted, json const& price_adjustments, json const& share_changes) {
            if (!unadjusted.is_array() || unadjusted.empty()) {
                return json::array();
            }
            std::unordered_map<std::string, double> price_adj_by_date;
            std::vector<std::string>                factor_dates;
            for (auto const& item : price_adjustments) {
                auto const date         = field_text(item, "date");
                price_adj_by_date[date] = num_or(field(item, "adj_ratio"));
                factor_dates.push_back(date);
            }
            std::sort(factor_dates.begin(), factor_dates.end(), std::greater<>());
            std::unordered_map<std::string, double> sc_by_date;
            for (auto const& item : share_changes) {
                sc_by_date[field_text(item, "date")] = num_or(field(item, "ratio"));
            }
            json adjusted = json::array();
            for (auto const& row : unadjusted) {
                auto const date   = field_text(row, "date");
                double     factor = 1.0;
                for (auto const& ev_date : factor_dates) {
                    if (date < ev_date) {
                        factor *= price_adj_by_date[ev_date];
                    }
                }
                json adj_row = row;
                for (auto const name : {"first_rial", "high_rial", "low_rial", "closing_rial", "last_rial"}) {
                    auto const value = field(row, name);
                    adj_row[std::string("adj_") + name] =
                      value.is_null() ? json(nullptr) : round2(num_or(value) * factor);
                }
                adj_row["adj_factor"] = round_to(factor, 6);
                json action           = nullptr;
                if (price_adj_by_date.contains(date)) {
                    action = sc_by_date.contains(date) ? "dividend+split" : "dividend";
                } else if (sc_by_date.contains(date)) {
                    action = "split";
                }
                adj_row["corporate_action"] = action;
                adjusted.push_back(adj_row);
            }
            return adjusted;
        }

        [[nodiscard]] json history_impl(client& http, std::string const& code, int days, bool adjusted, json& errors) {
            auto const rows =
              http.api(std::format("ClosingPrice/GetClosingPriceDailyList/{}/{}", code, days), "closingPriceDaily");
            if (!rows.is_array()) {
                throw error("Unexpected daily history result");
            }
            json flow_by_date = json::object();
            try {
                flow_by_date = client_type_history_impl(http, code);
            } catch (error const& exc) {
                errors["client_type_history"] = exc.what();
            }
            json unadjusted = json::array();
            for (auto const& row : rows) {
                if (!row.is_object()) {
                    continue;
                }
                auto const date = trade_date(pick(row, {"dEven"}));
                if (date.empty()) {
                    continue;
                }
                unadjusted.push_back(json{
                  {"date", date},
                  {"first_rial", price(pick(row, {"priceFirst", "pf"}))},
                  {"high_rial", price(pick(row, {"priceMax", "pmx"}))},
                  {"low_rial", price(pick(row, {"priceMin", "pmn"}))},
                  {"closing_rial", price(pick(row, {"pClosing", "pc"}))},
                  {"last_rial", price(pick(row, {"pDrCotVal", "pl"}))},
                  {"volume_shares", value_of(row, {"qTotTran5J", "tvol"})},
                  {"value_rial", value_of(row, {"qTotCap", "tval"})},
                  {"trade_count", value_of(row, {"zTotTran", "tno"})}
                });
            }
            std::stable_sort(unadjusted.begin(), unadjusted.end(), [](json const& a, json const& b) {
                return field_text(a, "date") < field_text(b, "date");
            });
            if (days > 0 && unadjusted.size() > static_cast<std::size_t>(days)) {
                json trimmed = json::array();
                for (std::size_t i = unadjusted.size() - static_cast<std::size_t>(days); i < unadjusted.size(); ++i) {
                    trimmed.push_back(unadjusted[i]);
                }
                unadjusted = std::move(trimmed);
            }
            for (auto& row : unadjusted) {
                auto const date = field_text(row, "date");
                auto const flow = field(flow_by_date, date);
                if (flow.is_object()) {
                    for (auto const name : client_type_history_fields) {
                        row[std::string(name)] = field(flow, name);
                    }
                } else {
                    for (auto const name : client_type_history_fields) {
                        row[std::string(name)] = nullptr;
                    }
                }
            }
            if (!adjusted) {
                return json{
                  {"unadjusted", unadjusted}
                };
            }
            auto const pa = price_adjustments_impl(http, code);
            auto const sc = share_changes_impl(http, code);
            return json{
              {"unadjusted", unadjusted},
              {"adjusted", adjust_history_impl(unadjusted, pa, sc)},
              {"adjustment_events", json{{"price_adjustments", pa}, {"share_changes", sc}}}
            };
        }

        // ------------------------------------------------------------ technicals

        [[nodiscard]] json technicals_impl(json const& history_rows) {
            if (!history_rows.is_array() || history_rows.empty()) {
                return nullptr;
            }
            auto adjusted_or = [](json const& row, std::string_view adjusted, std::string_view raw) {
                auto const amount = field(row, adjusted);
                return amount.is_null() ? field(row, raw) : amount;
            };
            auto closing = [&](json const& row) {
                return adjusted_or(row, "adj_closing_rial", "closing_rial");
            };
            auto opening = [&](json const& row) {
                return adjusted_or(row, "adj_first_rial", "first_rial");
            };
            auto high = [&](json const& row) {
                return adjusted_or(row, "adj_high_rial", "high_rial");
            };
            auto low = [&](json const& row) {
                return adjusted_or(row, "adj_low_rial", "low_rial");
            };

            std::vector<json> rows;
            for (auto const& row : history_rows) {
                if (!closing(row).is_null()) {
                    rows.push_back(row);
                }
            }
            if (rows.size() < 2) {
                return nullptr;
            }
            std::vector<double> closes;
            closes.reserve(rows.size());
            for (auto const& row : rows) {
                closes.push_back(num_or(closing(row)));
            }
            double const last = closes.back();
            json         result;
            result["as_of"]        = field(rows.back(), "date");
            result["sessions"]     = rows.size();
            result["closing_rial"] = last;

            static constexpr std::array<std::pair<std::string_view, int>, 5> periods{
              {{"1w", 5}, {"1m", 21}, {"3m", 63}, {"6m", 126}, {"1y", 250}}
            };
            for (auto const& [label, window] : periods) {
                result[std::format("change_{}_pct", label)] =
                  closes.size() > static_cast<std::size_t>(window)
                    ? round2((last / closes[closes.size() - 1 - static_cast<std::size_t>(window)] - 1.0) * 100.0)
                    : json(nullptr);
            }
            auto volatility = [&](std::size_t window) -> json {
                if (closes.size() <= window) {
                    return nullptr;
                }
                std::vector<double> returns;
                for (std::size_t i = closes.size() - window; i < closes.size(); ++i) {
                    returns.push_back(std::log(closes[i] / closes[i - 1]));
                }
                if (returns.size() < 2) {
                    return nullptr;
                }
                return round1(sample_stdev(returns) * std::sqrt(252.0) * 100.0);
            };
            result["volatility_20d_pct"] = volatility(20);
            result["volatility_60d_pct"] = volatility(60);

            std::vector<double> true_ranges;
            for (std::size_t i = std::max<std::size_t>(1, closes.size() - 14); i < closes.size(); ++i) {
                auto const day_high = high(rows[i]);
                auto const day_low  = low(rows[i]);
                if (day_high.is_null() || day_low.is_null()) {
                    continue;
                }
                double const previous = closes[i - 1];
                true_ranges.push_back(std::max({num_or(day_high) - num_or(day_low),
                                                std::abs(num_or(day_high) - previous),
                                                std::abs(num_or(day_low) - previous)}));
            }
            std::optional<double> atr;
            if (true_ranges.size() == 14) {
                atr = std::accumulate(true_ranges.begin(), true_ranges.end(), 0.0) /
                      static_cast<double>(true_ranges.size());
            }
            result["atr_14_rial"] = atr ? round2(*atr) : json(nullptr);
            result["atr_14_pct"]  = (atr && last != 0.0) ? round2(*atr / last * 100.0) : json(nullptr);

            for (std::size_t window : std::array<std::size_t, 3>{20, 50, 200}) {
                std::optional<double> average;
                if (closes.size() >= window) {
                    average = std::accumulate(closes.end() - static_cast<std::ptrdiff_t>(window), closes.end(), 0.0) /
                              static_cast<double>(window);
                }
                result[std::format("ma_{}_rial", window)] = average ? round2(*average) : json(nullptr);
                result[std::format("price_vs_ma_{}_pct", window)] =
                  (average && *average != 0.0) ? round2((last / *average - 1.0) * 100.0) : json(nullptr);
            }

            std::optional<double> rsi;
            if (closes.size() > 14) {
                double gains  = 0.0;
                double losses = 0.0;
                for (std::size_t i = closes.size() - 14; i < closes.size(); ++i) {
                    double const delta  = closes[i] - closes[i - 1];
                    gains              += delta > 0 ? delta : 0.0;
                    losses             += delta < 0 ? -delta : 0.0;
                }
                if (gains != 0.0 || losses != 0.0) {
                    rsi = losses == 0.0 ? 100.0 : 100.0 - 100.0 / (1.0 + gains / losses);
                }
            }
            result["rsi_14"] = rsi ? round1(*rsi) : json(nullptr);

            std::size_t const     year_start = rows.size() > 250 ? rows.size() - 250 : 0;
            std::optional<double> year_high;
            std::optional<double> year_low;
            for (std::size_t i = year_start; i < rows.size(); ++i) {
                auto const h = high(rows[i]);
                auto const l = low(rows[i]);
                if (!h.is_null()) {
                    year_high = year_high ? std::max(*year_high, num_or(h)) : num_or(h);
                }
                if (!l.is_null()) {
                    year_low = year_low ? std::min(*year_low, num_or(l)) : num_or(l);
                }
            }
            bool const complete_year = rows.size() >= 250;
            result["high_52w_rial"]  = complete_year && year_high ? json(*year_high) : json(nullptr);
            result["low_52w_rial"]   = complete_year && year_low ? json(*year_low) : json(nullptr);
            result["range_52w_position_pct"] =
              (complete_year && year_high && year_low && *year_high > *year_low)
                ? round1((last - *year_low) / (*year_high - *year_low) * 100.0)
                : json(nullptr);

            auto drawdown = [&](std::size_t window) -> json {
                std::vector<double> values(closes.end() - static_cast<std::ptrdiff_t>(window), closes.end());
                double              peak  = values.front();
                double              worst = 0.0;
                for (double const value : values) {
                    peak  = std::max(peak, value);
                    worst = std::min(worst, value / peak - 1.0);
                }
                return round2(worst * 100.0);
            };
            result["max_drawdown_1y_pct"] = closes.size() >= 250 ? drawdown(250) : json(nullptr);
            result["max_drawdown_3y_pct"] = closes.size() >= 750 ? drawdown(750) : json(nullptr);

            std::vector<double> gaps;
            for (std::size_t i = std::max<std::size_t>(1, rows.size() - 60); i < rows.size(); ++i) {
                auto const   first    = opening(rows[i]);
                double const previous = closes[i - 1];
                if (!first.is_null() && previous != 0.0) {
                    gaps.push_back((num_or(first) / previous - 1.0) * 100.0);
                }
            }
            result["median_gap_60d_pct"] = gaps.size() == 60 ? round2(median(gaps)) : json(nullptr);
            return result;
        }

        // ------------------------------------------------------------ corrections

        inline constexpr double           correction_min_decline     = 8.0;
        inline constexpr int              correction_max_sessions    = 120;
        inline constexpr double           consolidation_max_width    = 12.0;
        inline constexpr int              consolidation_min_sessions = 15;
        inline constexpr int              outcome_window_sessions    = 250;
        inline constexpr std::string_view flow_history_first_date    = "1397/09/05";

        [[nodiscard]] double corr_closing(json const& row) {
            auto const amount = field(row, "adj_closing_rial");
            return amount.is_null() ? num_or(field(row, "closing_rial")) : num_or(amount);
        }

        [[nodiscard]] bool corr_uptrend(
          std::vector<double> const&                closes,
          std::vector<std::optional<double>> const& averages,
          std::size_t                               index) {
            auto const avg_now  = averages[index];
            auto const avg_prev = averages[index - 20];
            if (index < 120 || !avg_now || !avg_prev) {
                return false;
            }
            auto const recent_begin  = closes.begin() + static_cast<std::ptrdiff_t>(index - 60);
            auto const recent_end    = closes.begin() + static_cast<std::ptrdiff_t>(index);
            auto const earlier_begin = closes.begin() + static_cast<std::ptrdiff_t>(index - 120);
            auto const earlier_end   = closes.begin() + static_cast<std::ptrdiff_t>(index - 60);
            return closes[index] > *avg_now && *avg_now > *avg_prev &&
                   *std::max_element(recent_begin, recent_end) > *std::max_element(earlier_begin, earlier_end) &&
                   *std::min_element(recent_begin, recent_end) > *std::min_element(earlier_begin, earlier_end);
        }

        [[nodiscard]] std::vector<std::optional<double>> corr_ma(std::vector<double> const& closes,
                                                                 std::size_t                window = 50) {
            if (window == 0) {
                return {};
            }
            std::vector<std::optional<double>> averages;
            averages.reserve(closes.size());
            for (std::size_t i = 0; i < closes.size(); ++i) {
                if (i + 1 < window) {
                    averages.push_back(std::nullopt);
                    continue;
                }
                double sum = 0.0;
                for (std::size_t j = i + 1 - window; j <= i; ++j) {
                    sum += closes[j];
                }
                averages.push_back(sum / static_cast<double>(window));
            }
            return averages;
        }

        [[nodiscard]] std::vector<std::size_t> corr_swing_highs(std::vector<double> const& closes,
                                                                std::size_t                span = 5) {
            std::vector<std::size_t> highs;
            if (closes.size() <= 2 * span) {
                return highs;
            }
            for (std::size_t i = span; i + span < closes.size(); ++i) {
                double const value  = closes[i];
                bool         is_max = true;
                int          count  = 0;
                for (std::size_t j = i - span; j <= i + span; ++j) {
                    if (closes[j] > value) {
                        is_max = false;
                        break;
                    }
                    if (closes[j] == value) {
                        ++count;
                    }
                }
                if (is_max && count == 1) {
                    highs.push_back(i);
                }
            }
            return highs;
        }

        [[nodiscard]] json corr_flow(json const& rows, std::size_t start, std::size_t end) {
            std::size_t sessions = end - start + 1;
            long long   buy      = 0;
            long long   sell     = 0;
            long long   volume   = 0;
            std::size_t covered  = 0;
            for (std::size_t i = start; i <= end; ++i) {
                auto const& row        = rows[i];
                auto const  legal_buy  = field(row, "legal_buy_shares");
                auto const  legal_sell = field(row, "legal_sell_shares");
                if (legal_buy.is_null() || legal_sell.is_null()) {
                    continue;
                }
                ++covered;
                buy    += static_cast<long long>(num_or(legal_buy));
                sell   += static_cast<long long>(num_or(legal_sell));
                volume += static_cast<long long>(num_or(field(row, "volume_shares")));
            }
            if (covered == 0) {
                return json{
                  {      "legal_buy_shares", nullptr},
                  {     "legal_sell_shares", nullptr},
                  {      "net_legal_shares", nullptr},
                  {"net_flow_pct_of_volume", nullptr},
                  {         "volume_shares", nullptr},
                  {     "flow_coverage_pct",     0.0}
                };
            }
            long long const net = buy - sell;
            return json{
              {      "legal_buy_shares",                                  buy                                        },
              {     "legal_sell_shares",                                                                         sell},
              {      "net_legal_shares",                                                                          net},
              {"net_flow_pct_of_volume",
               volume != 0 ? round2(static_cast<double>(net) / static_cast<double>(volume) * 100.0) : json(nullptr)  },
              {         "volume_shares",                                                                       volume},
              {     "flow_coverage_pct", round1(static_cast<double>(covered) / static_cast<double>(sessions) * 100.0)}
            };
        }

        [[nodiscard]] json corr_percent(double numerator, double denominator) {
            return denominator != 0.0 ? round2((numerator / denominator - 1.0) * 100.0) : json(nullptr);
        }

        [[nodiscard]] json corr_level(json const& value) {
            return value.is_null() ? json(nullptr) : round2(num_or(value));
        }

        [[nodiscard]] json
        corr_episode(json const& rows, std::vector<double> const& closes, std::size_t high, std::size_t trough) {
            json result;
            result["start_date"]          = field(rows[high], "date");
            result["end_date"]            = field(rows[trough], "date");
            result["high_close_adj_rial"] = round2(closes[high]);
            result["low_close_adj_rial"]  = round2(closes[trough]);
            result["decline_pct"]         = corr_percent(closes[trough], closes[high]);
            result["sessions"]            = trough - high;
            auto const flow               = corr_flow(rows, high, trough);
            for (auto it = flow.begin(); it != flow.end(); ++it) {
                result[it.key()] = it.value();
            }
            std::size_t const window_end = std::min(rows.size(), trough + outcome_window_sessions + 1);
            std::size_t       peak       = trough;
            for (std::size_t i = trough; i < window_end; ++i) {
                if (closes[i] > closes[peak]) {
                    peak = i;
                }
            }
            std::optional<std::size_t> breakout;
            for (std::size_t i = trough + 1; i < window_end; ++i) {
                if (closes[i] > closes[high]) {
                    breakout = i;
                    break;
                }
            }
            result["rally_from_low_pct"] = corr_percent(closes[peak], closes[trough]);
            result["sessions_to_peak"]   = peak - trough;
            result["broke_prior_high"]   = breakout.has_value();
            result["sessions_to_break"]  = breakout ? json(static_cast<long long>(*breakout - trough)) : json(nullptr);
            result["break_date"]         = breakout ? field(rows[*breakout], "date") : json(nullptr);
            result["return_after_break_20_pct"] =
              (breakout && *breakout + 20 < rows.size())
                ? corr_percent(closes[*breakout + 20], closes[*breakout])
                : json(nullptr);
            result["return_after_break_60_pct"] =
              (breakout && *breakout + 60 < rows.size())
                ? corr_percent(closes[*breakout + 60], closes[*breakout])
                : json(nullptr);
            result["outcome_window_complete"] = trough + outcome_window_sessions < rows.size();
            result["low_close_raw_rial"]      = corr_level(field(rows[trough], "closing_rial"));
            std::optional<double> lowest;
            for (std::size_t i = high; i <= trough; ++i) {
                auto const low = field(rows[i], "low_rial");
                if (!low.is_null()) {
                    lowest = lowest ? std::min(*lowest, num_or(low)) : num_or(low);
                }
            }
            result["low_low_raw_rial"]    = lowest ? round2(*lowest) : json(nullptr);
            result["prior_high_raw_rial"] = corr_level(field(rows[high], "closing_rial"));
            return result;
        }

        [[nodiscard]] json
        corr_consolidation(json const& rows, std::vector<double> const& closes, std::size_t start, std::size_t end) {
            double const high = *std::max_element(closes.begin() + static_cast<std::ptrdiff_t>(start),
                                                  closes.begin() + static_cast<std::ptrdiff_t>(end + 1));
            double const low  = *std::min_element(closes.begin() + static_cast<std::ptrdiff_t>(start),
                                                  closes.begin() + static_cast<std::ptrdiff_t>(end + 1));
            json         result;
            result["start_date"]          = field(rows[start], "date");
            result["end_date"]            = field(rows[end], "date");
            result["width_pct"]           = round2((high / low - 1.0) * 100.0);
            result["sessions"]            = end - start + 1;
            result["high_close_adj_rial"] = round2(high);
            result["low_close_adj_rial"]  = round2(low);
            auto const flow               = corr_flow(rows, start, end);
            for (auto it = flow.begin(); it != flow.end(); ++it) {
                result[it.key()] = it.value();
            }
            std::vector<double> before;
            std::size_t const   before_start = start > 60 ? start - 60 : 0;
            for (std::size_t i = before_start; i < start; ++i) {
                auto const v = field(rows[i], "volume_shares");
                if (!v.is_null()) {
                    before.push_back(num_or(v));
                }
            }
            std::vector<double> inside;
            for (std::size_t i = start; i <= end; ++i) {
                auto const v = field(rows[i], "volume_shares");
                if (!v.is_null()) {
                    inside.push_back(num_or(v));
                }
            }
            double const before_sum = std::accumulate(before.begin(), before.end(), 0.0);
            result["volume_vs_before_pct"] =
              (!before.empty() && !inside.empty() && before_sum > 0.0)
                ? corr_percent(std::accumulate(inside.begin(), inside.end(), 0.0) / static_cast<double>(inside.size()),
                               before_sum / static_cast<double>(before.size()))
                : json(nullptr);
            std::size_t const horizon = std::min(rows.size() - 1, end + 60);
            double            peak    = high;
            for (std::size_t i = end + 1; i <= horizon; ++i) {
                peak = std::max(peak, closes[i]);
            }
            result["breakout_pct"] = corr_percent(peak, high);
            std::optional<std::size_t> breakout;
            for (std::size_t i = end + 1; i < std::min(rows.size(), end + 121); ++i) {
                if (closes[i] > high) {
                    breakout = i;
                    break;
                }
            }
            result["breakout_date"] = breakout ? field(rows[*breakout], "date") : json(nullptr);
            result["breakout_held"] = horizon > end ? json(closes[horizon] > high) : json(nullptr);
            return result;
        }

        [[nodiscard]] json corr_current(
          json const&                               rows,
          std::vector<double> const&                closes,
          std::vector<std::optional<double>> const& averages,
          std::vector<std::optional<double>> const& long_average) {
            if (rows.empty()) {
                return nullptr;
            }
            std::size_t const last   = closes.size() - 1;
            std::size_t const window = last > 120 ? last - 120 : 0;
            std::size_t       high   = window;
            for (std::size_t i = window; i <= last; ++i) {
                if (closes[i] > closes[high]) {
                    high = i;
                }
            }
            double const      current_close = closes[last];
            auto const        decline       = corr_percent(current_close, closes[high]);
            std::size_t const sessions      = last - high;
            auto const        width =
              corr_percent(*std::max_element(closes.begin() + static_cast<std::ptrdiff_t>(high), closes.end()),
                           *std::min_element(closes.begin() + static_cast<std::ptrdiff_t>(high), closes.end()));
            std::string status = "no qualifying episode";
            if (!decline.is_null() && num_or(decline) <= -correction_min_decline) {
                status = "correction in progress";
            } else if (
              !width.is_null() && num_or(width) <= consolidation_max_width &&
              sessions >= static_cast<std::size_t>(consolidation_min_sessions) && corr_uptrend(closes, averages, high))
            {
                status = "consolidation in progress";
            }
            json result;
            result["status"]                 = status;
            result["start_date"]             = field(rows[high], "date");
            result["last_date"]              = field(rows[last], "date");
            result["high_close_adj_rial"]    = round2(closes[high]);
            result["current_close_adj_rial"] = round2(current_close);
            result["decline_pct"]            = decline;
            result["sessions"]               = sessions;
            result["range_width_pct"]        = width;
            auto const avg_last              = averages[last];
            auto const long_avg_last         = long_average[last];
            result["price_vs_ma_50_pct"]     = avg_last ? corr_percent(current_close, *avg_last) : json(nullptr);
            result["price_vs_ma_200_pct"] = long_avg_last ? corr_percent(current_close, *long_avg_last) : json(nullptr);
            result["prior_high_raw_rial"] = corr_level(field(rows[high], "closing_rial"));
            result["last_close_raw_rial"] = corr_level(field(rows[last], "closing_rial"));
            result["last_low_raw_rial"]   = corr_level(field(rows[last], "low_rial"));
            auto const flow               = corr_flow(rows, high, last);
            for (auto it = flow.begin(); it != flow.end(); ++it) {
                result[it.key()] = it.value();
            }
            return result;
        }

        [[nodiscard]] json corr_baseline(std::vector<double> const&                closes,
                                         std::vector<std::optional<double>> const& averages) {
            std::vector<double> forward_60;
            std::vector<double> forward_250;
            std::size_t         new_high         = 0;
            std::size_t         uptrend_sessions = 0;
            for (std::size_t index = 0; index < closes.size(); ++index) {
                if (!corr_uptrend(closes, averages, index)) {
                    continue;
                }
                ++uptrend_sessions;
                if (index + 60 < closes.size()) {
                    forward_60.push_back(closes[index + 60] / closes[index] - 1.0);
                }
                if (index + 250 < closes.size()) {
                    forward_250.push_back(closes[index + 250] / closes[index] - 1.0);
                }
                std::size_t const begin = index > 60 ? index - 60 : 0;
                if (begin == index) {
                    continue;
                }
                double const threshold = *std::max_element(closes.begin() + static_cast<std::ptrdiff_t>(begin),
                                                           closes.begin() + static_cast<std::ptrdiff_t>(index));
                for (std::size_t i = index + 1; i < std::min(closes.size(), index + 251); ++i) {
                    if (closes[i] > threshold) {
                        ++new_high;
                        break;
                    }
                }
            }
            auto share_positive = [](std::vector<double> const& values) -> json {
                if (values.empty()) {
                    return nullptr;
                }
                std::size_t positive = 0;
                for (double const value : values) {
                    if (value > 0.0) {
                        ++positive;
                    }
                }
                return round1(100.0 * static_cast<double>(positive) / static_cast<double>(values.size()));
            };
            return json{
              {              "uptrend_sessions",          uptrend_sessions                                                },
              {         "median_forward_60_pct",   forward_60.empty() ? json(nullptr) : round2(median(forward_60) * 100.0)},
              { "share_positive_forward_60_pct",                                                share_positive(forward_60)},
              {        "median_forward_250_pct", forward_250.empty() ? json(nullptr) : round2(median(forward_250) * 100.0)},
              {"share_positive_forward_250_pct",                                               share_positive(forward_250)},
              { "share_new_high_within_250_pct",
               uptrend_sessions != 0
               ? round1(100.0 * static_cast<double>(new_high) / static_cast<double>(uptrend_sessions))
               : json(nullptr)                                                                                            }
            };
        }

        [[nodiscard]] json corrections_impl(json const& history_rows) {
            if (!history_rows.is_array()) {
                return nullptr;
            }
            std::vector<json> rows;
            for (auto const& row : history_rows) {
                if (!field(row, "adj_closing_rial").is_null() || !field(row, "closing_rial").is_null()) {
                    rows.push_back(row);
                }
            }
            if (rows.size() < 130) {
                return nullptr;
            }
            std::vector<double> closes;
            closes.reserve(rows.size());
            for (auto const& row : rows) {
                closes.push_back(corr_closing(row));
            }
            auto                       averages     = corr_ma(closes);
            auto                       long_average = corr_ma(closes, 200);
            json                       episodes     = json::array();
            std::optional<std::size_t> last_trough;
            for (std::size_t high : corr_swing_highs(closes)) {
                if (last_trough && high <= *last_trough + 5) {
                    continue;
                }
                if (!corr_uptrend(closes, averages, high)) {
                    continue;
                }
                std::optional<std::size_t> trough;
                for (std::size_t i = high + 1; i <= std::min(rows.size() - 1, high + correction_max_sessions); ++i) {
                    if (closes[i] > closes[high]) {
                        break;
                    }
                    if (!trough || closes[i] < closes[*trough]) {
                        trough = i;
                    }
                }
                if (!trough || *trough - high < 3) {
                    continue;
                }
                auto const decline = corr_percent(closes[*trough], closes[high]);
                if (decline.is_null() || num_or(decline) > -correction_min_decline) {
                    continue;
                }
                episodes.push_back(corr_episode(rows, closes, high, *trough));
                last_trough = trough;
            }
            json                       consolidations = json::array();
            std::size_t                index          = 0;
            std::optional<std::size_t> last_end;
            while (index + consolidation_min_sessions <= rows.size()) {
                if (last_end && index <= *last_end + 10) {
                    index = *last_end + 11;
                    continue;
                }
                std::size_t end  = index + consolidation_min_sessions - 1;
                double      high = *std::max_element(closes.begin() + static_cast<std::ptrdiff_t>(index),
                                                     closes.begin() + static_cast<std::ptrdiff_t>(end + 1));
                double      low  = *std::min_element(closes.begin() + static_cast<std::ptrdiff_t>(index),
                                                     closes.begin() + static_cast<std::ptrdiff_t>(end + 1));
                while (end + 1 < rows.size()) {
                    double const candidate_high = std::max(high, closes[end + 1]);
                    double const candidate_low  = std::min(low, closes[end + 1]);
                    auto const   width          = corr_percent(candidate_high, candidate_low);
                    if (width.is_null() || num_or(width) > consolidation_max_width) {
                        break;
                    }
                    high = candidate_high;
                    low  = candidate_low;
                    ++end;
                }
                if (end - index + 1 >= static_cast<std::size_t>(consolidation_min_sessions) &&
                    corr_uptrend(closes, averages, index))
                {
                    std::size_t const sessions = end - index + 1;
                    std::size_t       downs    = 0;
                    std::size_t       ups      = 0;
                    for (std::size_t i = index + 1; i <= end; ++i) {
                        if (closes[i] < closes[i - 1]) {
                            ++downs;
                        }
                        if (closes[i] > closes[i - 1]) {
                            ++ups;
                        }
                    }
                    bool const oscillates =
                      downs >=
                        std::max<std::size_t>(3, static_cast<std::size_t>(static_cast<double>(sessions) * 0.2)) &&
                      ups >= std::max<std::size_t>(3, static_cast<std::size_t>(static_cast<double>(sessions) * 0.2));
                    auto const start_date = field_text(rows[index], "date");
                    auto const end_date   = field_text(rows[end], "date");
                    bool       overlaps   = false;
                    for (auto const& episode : episodes) {
                        auto const ep_start = field_text(episode, "start_date");
                        auto const ep_end   = field_text(episode, "end_date");
                        if ((ep_start <= start_date && start_date <= ep_end) ||
                            (start_date <= ep_start && ep_start <= end_date))
                        {
                            overlaps = true;
                            break;
                        }
                    }
                    if (oscillates && !overlaps) {
                        consolidations.push_back(corr_consolidation(rows, closes, index, end));
                        last_end = end;
                    }
                    index = end + 1;
                } else {
                    ++index;
                }
            }
            return json{
              {"window",
               json{{"first", field(rows.front(), "date")},
                    {"last", field(rows.back(), "date")},
                    {"sessions", rows.size()}}},
              {"flow_from", flow_history_first_date},
              {"corrections", episodes},
              {"consolidations", consolidations},
              {"current", corr_current(rows, closes, averages, long_average)},
              {"baseline", corr_baseline(closes, averages)}
            };
        }

        // ------------------------------------------------------------ fundamentals

        [[nodiscard]] json
        detect_fund(json const& isin, json const& industry, json const& sub_industry, json const& units) {
            auto const label       = normal(text_of(industry)) + " " + normal(text_of(sub_industry));
            bool const isin_string = isin.is_string();
            bool const has_units   = !units.is_null() && !(units.is_object() && units.empty());
            if (!isin_string && !has_units && label.find("صندوق") == std::string::npos) {
                return nullptr;
            }
            bool const fund = (isin_string && isin.get<std::string>().starts_with("IRT")) || has_units ||
                              label.find("صندوق") != std::string::npos;
            return fund;
        }

        [[nodiscard]] json
        info_impl(client& http, std::string const& code, json const& ident, json const& current, json& errors) {
            json result;
            result["industry"]     = field(ident, "industry");
            result["sub_industry"] = field(ident, "sub_industry");
            result["board"]        = field(ident, "board");
            result["eps_rial"]     = field(current, "eps_rial");
            result["eps_source"]   = field(current, "eps_rial").is_null() ? json(nullptr) : json("TSETMC quote");
            result["pe"]           = field(current, "pe");
            result["implied_pe_from_closing"] = nullptr;
            result["sector_pe"]               = field(current, "sector_pe");
            result["share_count"]             = field(ident, "share_count");
            result["market_cap_rial"]         = nullptr;
            result["market_cap_source"]       = nullptr;
            result["free_float_pct"]          = nullptr;
            result["week_range_rial"]         = nullptr;
            result["year_range_rial"]         = nullptr;
            result["avg_volume_5d_shares"]    = nullptr;
            result["psr_raw"]                 = nullptr;
            result["daily_price_limits_rial"] = nullptr;
            result["fund_units"]              = nullptr;
            result["is_fund"]                 = nullptr;
            result["nav_issuance_rial"]       = nullptr;
            result["nav_redemption_rial"]     = nullptr;
            result["nav_as_of"]               = nullptr;
            result["nav_as_of_time"]          = nullptr;
            result["nav_premium_pct"]         = nullptr;

            try {
                auto const row = http.api(std::format("Instrument/GetInstrumentInfo/{}", code), "instrumentInfo");
                if (!row.is_object()) {
                    throw error("No instrument fundamentals");
                }
                static constexpr std::array<std::pair<std::string_view, std::array<std::string_view, 2>>, 4>
                  alias_table{
                    {{"pe", {"pe", ""}},
                     {"sector_pe", {"sectorPE", ""}},
                     {"share_count", {"shareCount", "zTitad"}},
                     {"free_float_pct", {"freeFloatPercent", "floatPercent"}}}
                };
                for (auto const& [key, aliases] : alias_table) {
                    json amount = nullptr;
                    for (auto const alias : aliases) {
                        if (alias.empty()) {
                            continue;
                        }
                        amount = value_of(row, {alias});
                        if (!amount.is_null()) {
                            break;
                        }
                    }
                    if (!amount.is_null()) {
                        result[key] = amount;
                    }
                }
                auto const earnings = pick(row, {"eps"});
                if (earnings.is_object()) {
                    auto const estimated = value_of(earnings, {"estimatedEPS"});
                    auto const actual    = value_of(earnings, {"epsValue"});
                    if (!estimated.is_null()) {
                        result["eps_rial"]   = estimated;
                        result["eps_source"] = "TSETMC estimatedEPS";
                    } else if (!actual.is_null()) {
                        result["eps_rial"]   = actual;
                        result["eps_source"] = "TSETMC epsValue";
                    }
                    auto const sector_pe = value_of(earnings, {"sectorPE"});
                    if (!sector_pe.is_null()) {
                        result["sector_pe"] = sector_pe;
                    }
                    auto const psr = value_of(earnings, {"psr"});
                    if (!psr.is_null()) {
                        result["psr_raw"] = psr;
                    }
                } else {
                    auto const eps = value_of(row, {"estimatedEPS", "eps"});
                    if (!eps.is_null()) {
                        result["eps_rial"]   = eps;
                        result["eps_source"] = "TSETMC instrumentInfo";
                    }
                }
                if (result["psr_raw"].is_null()) {
                    result["psr_raw"] = value_of(row, {"psr"});
                }
                result["market_cap_rial"] = value_of(row, {"marketValue", "marketCap"});
                if (!result["market_cap_rial"].is_null()) {
                    result["market_cap_source"] = "TSETMC instrumentInfo";
                }
                auto const sector = pick(pick(row, {"sector"}), {"lSecVal"});
                if (!sector.is_null()) {
                    result["industry"] = sector;
                }
                auto const board = pick(row, {"cgrValCotTitle"});
                if (!board.is_null()) {
                    result["board"] = board;
                }
                auto const low_week  = value_of(row, {"minWeek"});
                auto const high_week = value_of(row, {"maxWeek"});
                if (!low_week.is_null() || !high_week.is_null()) {
                    result["week_range_rial"] = json{
                      { "low",  low_week},
                      {"high", high_week}
                    };
                }
                auto const low_year  = value_of(row, {"minYear"});
                auto const high_year = value_of(row, {"maxYear"});
                if (!low_year.is_null() || !high_year.is_null()) {
                    result["year_range_rial"] = json{
                      { "low",  low_year},
                      {"high", high_year}
                    };
                }
                result["avg_volume_5d_shares"] = value_of(row, {"qTotTran5JAvg"});
                auto const threshold           = pick(row, {"staticThreshold"});
                if (threshold.is_object()) {
                    auto const low_limit  = value_of(threshold, {"psGelStaMin"});
                    auto const high_limit = value_of(threshold, {"psGelStaMax"});
                    if (!low_limit.is_null() || !high_limit.is_null()) {
                        result["daily_price_limits_rial"] = json{
                          { "low",  low_limit},
                          {"high", high_limit}
                        };
                    }
                }
                auto const units = value_of(row, {"etfIssuedUnit"});
                if (!units.is_null() && num_or(units) != 0.0) {
                    result["fund_units"] = json{
                      {"units", units},
                      {"date", trade_date(pick(row, {"etfUnitDeven"}))}
                    };
                }
            } catch (error const& exc) {
                errors["instrument_info"] = exc.what();
            }
            auto const closing = field(current, "closing_rial");
            result["is_fund"] =
              detect_fund(field(ident, "isin"), result["industry"], result["sub_industry"], result["fund_units"]);
            if (result["is_fund"].is_boolean() && result["is_fund"].get<bool>()) {
                try {
                    auto const fund = http.api(std::format("Fund/GetETFByInsCode/{}", code), "etf");
                    if (!fund.is_object()) {
                        throw error("No fund NAV data");
                    }
                    result["nav_issuance_rial"]   = value_of(fund, {"pSubTran"});
                    result["nav_redemption_rial"] = value_of(fund, {"pRedTran"});
                    result["nav_as_of"]           = trade_date(pick(fund, {"deven"}));
                    result["nav_as_of_time"]      = trade_time(pick(fund, {"hEven"}));
                    auto const redemption         = result["nav_redemption_rial"];
                    if (!closing.is_null() && !redemption.is_null() && num_or(redemption) != 0.0) {
                        result["nav_premium_pct"] = round2((num_or(closing) / num_or(redemption) - 1.0) * 100.0);
                    }
                } catch (error const& exc) {
                    errors["fund_nav"] = exc.what();
                }
            }
            auto const isin = field(ident, "isin");
            if (!isin.is_null() && !text_of(isin).empty() &&
                (result["share_count"].is_null() || (result["market_cap_rial"].is_null() && closing.is_null())))
            {
                try {
                    auto const url = std::format("{}?InstrumentId={}", webgw_base, url_encode(text_of(isin)));
                    auto       row = http.json_get(url);
                    row            = pick(row, {"data"}).is_null() ? row : pick(row, {"data"});
                    if (!row.is_object()) {
                        throw error("Unexpected official gateway response");
                    }
                    if (result["share_count"].is_null()) {
                        result["share_count"] = value_of(row, {"sharecount"});
                    }
                    if (result["market_cap_rial"].is_null()) {
                        result["market_cap_rial"] = value_of(row, {"marketvalue"});
                        if (!result["market_cap_rial"].is_null()) {
                            result["market_cap_source"] = "TSE official gateway";
                        }
                    }
                } catch (error const& exc) {
                    errors["market_cap"] = exc.what();
                }
            }
            if (result["market_cap_rial"].is_null() && !result["share_count"].is_null() && !closing.is_null()) {
                result["market_cap_rial"]   = number(num_or(result["share_count"]) * num_or(closing));
                result["market_cap_source"] = "derived: share_count × current closing_rial";
            }
            if (result["pe"].is_null() && !result["eps_rial"].is_null() && !closing.is_null() &&
                num_or(result["eps_rial"]) != 0.0)
            {
                result["implied_pe_from_closing"] = round2(num_or(closing) / num_or(result["eps_rial"]));
            }
            return result;
        }

        // ------------------------------------------------------------ market context

        [[nodiscard]] json market_currencies(client& http) {
            (void) http;
            ai::http_request request;
            request.url             = "https://www.tasnimnews.ir/common/CurrencyTable";
            request.method          = "POST";
            request.body            = "{}";
            request.content_type    = "application/json; charset=utf-8";
            request.timeout_seconds = 20;
            request.retries         = 1;
            std::string body;
            try {
                body = ai::perform(request).body;
            } catch (ai::http_error const& exc) {
                throw error(std::format("Cannot reach tasnimnews.ir: {}", exc.what()));
            }
            auto const response = std::string_view(body);
            auto       outer    = json::parse(response, nullptr, false);
            if (outer.is_discarded()) {
                throw error("Non-JSON output from tasnim currency feed");
            }
            json inner = outer;
            if (outer.is_string()) {
                inner = json::parse(outer.get<std::string>(), nullptr, false);
            }
            auto const items = field(inner, "currency");
            if (!items.is_array()) {
                throw error("Unexpected tasnim currency JSON output");
            }
            static std::set<std::string> const currency_titles{
              "price_dollar_rl", "price_eur", "price_gbp", "price_aed", "price_try", "price_cny", "price_jpy",
              "price_cad",       "price_aud", "price_chf", "price_sar", "price_kwd", "price_qar", "price_omr",
              "price_iqd",       "price_afn", "price_sek", "price_dkk", "price_nok", "price_hkd", "price_krw",
              "price_myr",       "price_thb", "price_rub", "price_azn", "price_bhd"};
            static std::set<std::string> const gold_titles{"geram18", "geram24", "silver_999"};
            static std::set<std::string> const coin_titles{
              "sekee",
              "sekeb",
              "nim",
              "rob",
              "gerami",
              "retail_sekee",
              "retail_sekeb",
              "retail_nim",
              "retail_rob",
              "retail_gerami"};
            json currencies = json::array();
            json gold       = json::array();
            json coins      = json::array();
            for (auto const& item : items) {
                auto const title = field_text(item, "title");
                if (currency_titles.contains(title)) {
                    currencies.push_back(item);
                } else if (gold_titles.contains(title)) {
                    gold.push_back(item);
                } else if (coin_titles.contains(title)) {
                    coins.push_back(item);
                }
            }
            return json{
              {    "source",                                        "tasnimnews.ir/fa/currency"},
              {"categories", json{{"currencies", currencies}, {"gold", gold}, {"coins", coins}}}
            };
        }

        [[nodiscard]] json world_bank_series(client& http, std::string const& indicator) {
            auto const payload = http.json_get(std::format("{}{}?format=json&per_page=4", world_bank_base, indicator));
            if (!payload.is_array() || payload.empty()) {
                throw error(std::format("Unexpected World Bank response for {}", indicator));
            }
            if (payload[0].is_object() && !field(payload[0], "message").is_null()) {
                throw error(std::format("World Bank: {}", field_text(payload[0], "message")));
            }
            if (payload.size() < 2) {
                throw error(std::format("Unexpected World Bank response for {}", indicator));
            }
            json series = json::array();
            if (payload[1].is_array()) {
                for (auto const& row : payload[1]) {
                    if (!row.is_object() || field(row, "date").is_null() || field(row, "value").is_null()) {
                        continue;
                    }
                    series.push_back(json{
                      {"year",        number(field(row,   "date"))},
                      { "pct", round2(num_or(field(row, "value")))}
                    });
                }
            }
            if (series.empty()) {
                throw error(std::format("No World Bank data for {}", indicator));
            }
            return series;
        }

        [[nodiscard]] json economy_impl(client& http) {
            return json{
              {"source", "World Bank (annual data)"},
              {"inflation_cpi_annual_pct", world_bank_series(http, "FP.CPI.TOTL.ZG")},
              {"gdp_growth_annual_pct", world_bank_series(http, "NY.GDP.MKTP.KD.ZG")}
            };
        }

        [[nodiscard]] json instrument_statistics_impl(client& http, std::string const& code) {
            static std::map<std::pair<long long, long long>, std::string> const fields{
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
            json rows = json::array();
            for (int attempt = 0; attempt < 3; ++attempt) {
                rows = unwrap(http.json_get(std::format("{}/MarketData/GetInstrumentStatistic/{}", tsetmc_base, code)),
                              "instrumentStatistic");
                if (!rows.is_array()) {
                    throw error("Unexpected instrument statistics result");
                }
                if (!rows.empty()) {
                    break;
                }
            }
            json result = json::object();
            for (auto const& row : rows) {
                if (!row.is_object()) {
                    continue;
                }
                auto const data_type = value_of(row, {"dataType"});
                auto const partition = value_of(row, {"partitionCode"});
                auto const key       = std::make_pair(as_int(data_type).value_or(-1), as_int(partition).value_or(-1));
                auto const it        = fields.find(key);
                if (it == fields.end()) {
                    continue;
                }
                auto const parsed = number(pick(row, {"dataValue"}));
                if (!parsed.is_null()) {
                    result[it->second] = parsed;
                }
            }
            return result.empty() ? json(nullptr) : result;
        }

        [[nodiscard]] json
        market_breadth_day(client& http, std::string const& greg, std::optional<std::string> const& code) {
            auto const rows = http.api(std::format("ClosingPrice/GetInstrmentsHistoryInDay/{}", greg),
                                       "closingPriceDailyHistoryWithInstDetails");
            if (!rows.is_array() || rows.empty()) {
                return nullptr;
            }
            std::vector<double>   changes;
            long long             up   = 0;
            long long             down = 0;
            long long             flat = 0;
            std::optional<double> own;
            long long             skipped = 0;
            for (auto const& row : rows) {
                if (!row.is_object()) {
                    continue;
                }
                auto const yesterday = price(pick(row, {"priceYesterday"}));
                auto const change    = number(pick(row, {"priceChange"}));
                if (yesterday.is_null() || num_or(yesterday) < 10.0 || change.is_null()) {
                    ++skipped;
                    continue;
                }
                double const pct = num_or(change) / num_or(yesterday) * 100.0;
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
                if (code && text_of(pick(row, {"insCode"})) == *code) {
                    own = pct;
                }
            }
            if (changes.empty()) {
                return nullptr;
            }
            json result;
            result["date"]              = trade_date(json(greg));
            result["scope"]             = "whole market, every quoted instrument";
            result["instruments"]       = changes.size();
            result["skipped"]           = skipped;
            result["advancers"]         = up;
            result["decliners"]         = down;
            result["unchanged"]         = flat;
            result["median_change_pct"] = round2(median(changes));
            result["avg_change_pct"] =
              round2(std::accumulate(changes.begin(), changes.end(), 0.0) / static_cast<double>(changes.size()));
            if (own) {
                result["instrument_change_pct"] = round2(*own);
                std::size_t below               = 0;
                for (double const value : changes) {
                    if (value < *own) {
                        ++below;
                    }
                }
                result["instrument_percentile"] =
                  round1(static_cast<double>(below) / static_cast<double>(changes.size()) * 100.0);
            }
            return result;
        }

        [[nodiscard]] json market_breadth_proxy(client& http) {
            static std::vector<std::pair<std::string, std::string>> const components{
              {"32357363984168442", "فولاد"},
              {"46348559193224090", "خودرو"},
              {"27922860956133067",  "مبین"}
            };
            json results  = json::array();
            json failures = json::array();
            for (auto const& [code, symbol] : components) {
                try {
                    results.push_back(json{
                      {"symbol", symbol},
                      {"quote", quote_impl(http, code)}
                    });
                } catch (error const& exc) {
                    failures.push_back(json{
                      {"symbol",     symbol},
                      { "error", exc.what()}
                    });
                }
            }
            json rows = json::array();
            for (auto const& item : results) {
                auto const quote_data = field(item, "quote");
                auto const last       = field(quote_data, "last_rial").is_null() ? field(quote_data, "closing_rial")
                                                                                 : field(quote_data, "last_rial");
                auto const yesterday  = field(quote_data, "yesterday_rial");
                auto const volume     = field(quote_data, "volume_shares");
                json       row;
                row["symbol"]    = field(item, "symbol");
                row["last_rial"] = last;
                row["change_pct"] =
                  (!volume.is_null() && !last.is_null() && !yesterday.is_null() && num_or(volume) != 0.0)
                    ? round2((num_or(last) / num_or(yesterday) - 1.0) * 100.0)
                    : json(nullptr);
                row["volume_shares"] = volume;
                row["value_rial"]    = field(quote_data, "value_rial");
                row["state"]         = field(quote_data, "state");
                rows.push_back(row);
            }
            json traded = json::array();
            for (auto const& row : rows) {
                if (!field(row, "volume_shares").is_null() && num_or(field(row, "volume_shares")) != 0.0) {
                    traded.push_back(row);
                }
            }
            std::vector<double> changes;
            for (auto const& row : traded) {
                if (!field(row, "change_pct").is_null()) {
                    changes.push_back(num_or(field(row, "change_pct")));
                }
            }
            std::vector<json> ranked;
            for (auto const& row : traded) {
                if (!field(row, "change_pct").is_null()) {
                    ranked.push_back(row);
                }
            }
            std::sort(ranked.begin(), ranked.end(), [](json const& a, json const& b) {
                return num_or(field(a, "change_pct")) > num_or(field(b, "change_pct"));
            });
            auto top = [](std::vector<json> const& list, bool gainers) {
                json out = json::array();
                for (auto const& row : list) {
                    double const pct = num_or(field(row, "change_pct"));
                    if (gainers ? pct > 0.01 : pct < -0.01) {
                        out.push_back(json{
                          {    "symbol", field(row,     "symbol")},
                          {"change_pct", field(row, "change_pct")}
                        });
                        if (out.size() >= 3) {
                            break;
                        }
                    }
                }
                return out;
            };
            std::vector<json> reversed(ranked.rbegin(), ranked.rend());
            long long         total_volume = 0;
            long long         total_value  = 0;
            for (auto const& row : traded) {
                total_volume += static_cast<long long>(num_or(field(row, "volume_shares")));
                total_value  += static_cast<long long>(num_or(field(row, "value_rial")));
            }
            json non_trading = json::array();
            for (auto const& row : rows) {
                if (field(row, "volume_shares").is_null() || num_or(field(row, "volume_shares")) == 0.0) {
                    non_trading.push_back(json{
                      {"symbol", field(row, "symbol")},
                      { "state", field(row,  "state")}
                    });
                }
            }
            json result;
            result["scope"]     = "tracked large-cap index components only, not the whole market";
            result["tracked"]   = components.size();
            result["traded"]    = traded.size();
            result["advancers"] = std::count_if(changes.begin(), changes.end(), [](double v) {
                return v > 0.01;
            });
            result["decliners"] = std::count_if(changes.begin(), changes.end(), [](double v) {
                return v < -0.01;
            });
            result["unchanged"] = std::count_if(changes.begin(), changes.end(), [](double v) {
                return v >= -0.01 && v <= 0.01;
            });
            result["avg_change_pct"] =
              changes.empty()
                ? json(nullptr)
                : round2(std::accumulate(changes.begin(), changes.end(), 0.0) / static_cast<double>(changes.size()));
            result["total_volume_shares"] = total_volume != 0 ? json(total_volume) : json(nullptr);
            result["total_value_rial"]    = total_value != 0 ? json(total_value) : json(nullptr);
            result["top_gainers"]         = top(ranked, true);
            result["top_losers"]          = top(reversed, false);
            result["non_trading"]         = non_trading;
            result["components"]          = rows;
            result["failures"]            = failures;
            return result;
        }

        [[nodiscard]] std::string gregorian_day_offset(int days) {
            auto const now = std::chrono::system_clock::now() + std::chrono::hours(3) + std::chrono::minutes(30) -
                             std::chrono::hours(24 * days);
            auto const tt  = std::chrono::system_clock::to_time_t(now);
            std::tm    tm{};
            ::gmtime_r(&tt, &tm);
            return std::format("{:04d}{:02d}{:02d}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        }

        [[nodiscard]] json market_breadth_impl(client& http, std::optional<std::string> const& code) {
            for (int offset = 0; offset < 8; ++offset) {
                try {
                    auto const exact = market_breadth_day(http, gregorian_day_offset(offset), code);
                    if (!exact.is_null()) {
                        return exact;
                    }
                } catch (error const&) {
                    // Best-effort probe: a missing or unpublished session is skipped.
                    continue;
                }
            }
            return market_breadth_proxy(http);
        }

        [[nodiscard]] json
        market_breadth_history_impl(client& http, std::string const& code, json const& newest, int sessions) {
            if (sessions < 1) {
                return json::array();
            }
            std::set<std::string> seen;
            if (newest.is_object() && !field(newest, "date").is_null()) {
                seen.insert(field_text(newest, "date"));
            }
            std::vector<json> found;
            for (int offset = 1; offset < sessions + 8; ++offset) {
                try {
                    auto const day = market_breadth_day(http, gregorian_day_offset(offset), code);
                    if (!day.is_null() && seen.insert(field_text(day, "date")).second) {
                        found.push_back(day);
                    }
                } catch (error const&) {
                    // Best-effort probe: a missing or unpublished session is skipped.
                    continue;
                }
            }
            std::sort(found.begin(), found.end(), [](json const& a, json const& b) {
                return field_text(a, "date") > field_text(b, "date");
            });
            json       out   = json::array();
            auto const limit = static_cast<std::size_t>(std::max(0, sessions));
            for (std::size_t i = 0; i < found.size() && i < limit; ++i) {
                out.push_back(found[i]);
            }
            return out;
        }

        [[nodiscard]] json indexes_impl(client& http) {
            auto const rows = http.api("Index/GetIndexB1LastAll/SelectedIndexes/1", "indexB1");
            if (!rows.is_array()) {
                throw error("Unexpected market index result");
            }
            json out = json::array();
            for (auto const& row : rows) {
                if (!row.is_object()) {
                    continue;
                }
                out.push_back(json{
                  {   "name_fa",            pick(row,        {"lVal30"})},
                  {     "value",        value_of(row, {"xDrNivJIdx004"})},
                  {"change_pct",        value_of(row,   {"xVarIdxJRfV"})},
                  {    "change",        value_of(row,   {"indexChange"})},
                  {      "time", trade_time(pick(row,        {"hEven"}))}
                });
            }
            return out;
        }

        [[nodiscard]] std::optional<std::pair<std::string, std::string>> last_completed_session(
          json const&  history_data,
          std::int64_t now) {
            if (!history_data.is_object()) {
                return std::nullopt;
            }
            auto const rows = field(history_data, "unadjusted");
            if (!rows.is_array() || rows.empty()) {
                return std::nullopt;
            }
            auto newest = field_text(rows.back(), "date");
            if (newest.empty()) {
                return std::nullopt;
            }
            if (market_session_open(now)) {
                auto const today = jalali_day(now);
                if (newest == std::format("{}/{}/{}", today.substr(0, 4), today.substr(4, 2), today.substr(6, 2))) {
                    if (rows.size() < 2) {
                        return std::nullopt;
                    }
                    newest = field_text(rows[rows.size() - 2], "date");
                }
            }
            std::smatch             match;
            static std::regex const date_re(R"(^(\d{4})/(\d{2})/(\d{2})$)");
            if (!std::regex_match(newest, match, date_re)) {
                return std::nullopt;
            }
            auto const g =
              jalali_to_gregorian(std::stoi(match[1].str()), std::stoi(match[2].str()), std::stoi(match[3].str()));
            return std::make_pair(std::format("{:04d}{:02d}{:02d}", g.year, g.month, g.day), newest);
        }

        // ------------------------------------------------------------ snapshot / markdown helpers

        [[nodiscard]] std::string csv_escape(std::string const& value) {
            if (value.find_first_of(",\"\n\r") == std::string::npos) {
                return value;
            }
            std::string out = "\"";
            for (char const c : value) {
                if (c == '"') {
                    out += "\"\"";
                } else {
                    out.push_back(c);
                }
            }
            out += '"';
            return out;
        }

        [[nodiscard]] std::string csv_value(json const& value) {
            if (value.is_null()) {
                return {};
            }
            if (value.is_boolean()) {
                return value.get<bool>() ? "True" : "False";
            }
            if (value.is_string()) {
                return value.get<std::string>();
            }
            return value.dump();
        }

        [[nodiscard]] std::vector<std::string> csv_block(std::vector<json> const&        rows,
                                                         std::vector<std::string> const& columns) {
            std::vector<std::string> lines;
            std::string              header;
            for (std::size_t i = 0; i < columns.size(); ++i) {
                if (i != 0) {
                    header.push_back(',');
                }
                header += csv_escape(columns[i]);
            }
            lines.push_back(header);
            for (auto const& row : rows) {
                std::string line;
                for (std::size_t i = 0; i < columns.size(); ++i) {
                    if (i != 0) {
                        line.push_back(',');
                    }
                    line += csv_escape(csv_value(field(row, columns[i])));
                }
                lines.push_back(line);
            }
            return lines;
        }

        inline constexpr std::array<std::string_view, 23> correction_columns{
          "start_date",
          "end_date",
          "sessions",
          "high_close_adj_rial",
          "low_close_adj_rial",
          "decline_pct",
          "legal_buy_shares",
          "legal_sell_shares",
          "net_legal_shares",
          "net_flow_pct_of_volume",
          "volume_shares",
          "flow_coverage_pct",
          "rally_from_low_pct",
          "sessions_to_peak",
          "broke_prior_high",
          "sessions_to_break",
          "break_date",
          "return_after_break_20_pct",
          "return_after_break_60_pct",
          "outcome_window_complete",
          "prior_high_raw_rial",
          "low_close_raw_rial",
          "low_low_raw_rial"};
        inline constexpr std::array<std::string_view, 13> consolidation_columns{
          "start_date",
          "end_date",
          "sessions",
          "width_pct",
          "high_close_adj_rial",
          "low_close_adj_rial",
          "net_legal_shares",
          "net_flow_pct_of_volume",
          "flow_coverage_pct",
          "volume_vs_before_pct",
          "breakout_pct",
          "breakout_date",
          "breakout_held"};
        inline constexpr std::array<std::string_view, 16> current_columns{
          "status",
          "start_date",
          "last_date",
          "sessions",
          "high_close_adj_rial",
          "current_close_adj_rial",
          "decline_pct",
          "range_width_pct",
          "net_legal_shares",
          "net_flow_pct_of_volume",
          "flow_coverage_pct",
          "price_vs_ma_50_pct",
          "price_vs_ma_200_pct",
          "prior_high_raw_rial",
          "last_close_raw_rial",
          "last_low_raw_rial"};

        [[nodiscard]] std::vector<std::string> corrections_markdown(json const& value) {
            std::vector<std::string> lines{"", "## Historical corrections / اصلاحات تاریخی"};
            if (value.is_object() && !field(value, "note").is_null()) {
                lines.push_back(field_text(value, "note"));
                return lines;
            }
            auto const window = field(value, "window");
            lines.push_back(std::format(
              "Window: {}–{} ({} sessions). Episodes are selected by price structure only — never by what "
              "followed them — so the outcome columns are measurements, not a filter. Percentages use "
              "split/dividend-adjusted closes; the `*_raw_rial` columns are the levels that actually traded. "
              "Daily حقیقی/حقوقی flow is available from {} only: earlier episodes report empty flow, which is "
              "unknown, not zero.",
              field_text(window, "first"),
              field_text(window, "last"),
              text_of(field(window, "sessions")),
              field_text(value, "flow_from")));
            auto const episodes = field(value, "corrections");
            lines.push_back("");
            lines.push_back(std::format("### Corrections ({})", episodes.is_array() ? episodes.size() : 0));
            if (episodes.is_array() && !episodes.empty()) {
                std::vector<json> rows(episodes.begin(), episodes.end());
                auto const        block = csv_block(rows, {correction_columns.begin(), correction_columns.end()});
                lines.push_back("```csv");
                lines.insert(lines.end(), block.begin(), block.end());
                lines.push_back("```");
            } else {
                lines.push_back("None met the structural definition in this window.");
            }
            auto const consolidations = field(value, "consolidations");
            lines.push_back("");
            lines.push_back(
              std::format("### Consolidations ({})", consolidations.is_array() ? consolidations.size() : 0));
            if (consolidations.is_array() && !consolidations.empty()) {
                std::vector<json> rows(consolidations.begin(), consolidations.end());
                auto const        block = csv_block(rows, {consolidation_columns.begin(), consolidation_columns.end()});
                lines.push_back("```csv");
                lines.insert(lines.end(), block.begin(), block.end());
                lines.push_back("```");
            } else {
                lines.push_back("None met the structural definition in this window.");
            }
            lines.push_back("");
            lines.push_back("### Current episode");
            auto const current = field(value, "current");
            if (!current.is_null()) {
                auto const block = csv_block({current}, {current_columns.begin(), current_columns.end()});
                lines.push_back("```csv");
                lines.insert(lines.end(), block.begin(), block.end());
                lines.push_back("```");
            } else {
                lines.push_back("Not available: the history is too short.");
            }
            auto const baseline = field(value, "baseline");
            if (num_or(field(baseline, "uptrend_sessions")) != 0.0) {
                lines.push_back("");
                lines.push_back(std::format(
                  "Baseline over {} uptrend sessions: median forward 60-session return {}% ({}% positive), "
                  "median forward 250-session return {}% ({}% positive), {}% reached a new 60-session high "
                  "within 250 sessions.",
                  text_of(field(baseline, "uptrend_sessions")),
                  text_of(field(baseline, "median_forward_60_pct")),
                  text_of(field(baseline, "share_positive_forward_60_pct")),
                  text_of(field(baseline, "median_forward_250_pct")),
                  text_of(field(baseline, "share_positive_forward_250_pct")),
                  text_of(field(baseline, "share_new_high_within_250_pct"))));
            }
            return lines;
        }

        [[nodiscard]] std::vector<std::string> codal_cache_lines(json const& cache) {
            if (!cache.is_object()) {
                return {};
            }
            static std::map<std::string, std::string> const labels{
              {    "announcements",        "announcements"},
              {       "financials", "financial statements"},
              {"portfolio_reports",    "portfolio reports"}
            };
            std::vector<std::string> items;
            for (auto it = cache.begin(); it != cache.end(); ++it) {
                if (!it.value().is_object()) {
                    continue;
                }
                auto const label = labels.contains(it.key()) ? labels.at(it.key()) : it.key();
                auto       line  = std::format(
                  "- {}: cached, fetched at {}",
                  label,
                  field(it.value(), "fetched_at").is_null() ? std::string("an unknown time")
                                                            : field_text(it.value(), "fetched_at"));
                if (!field(it.value(), "age").is_null()) {
                    line += std::format(" ({} ago)", field_text(it.value(), "age"));
                }
                if (field(it.value(), "stale").is_boolean() && field(it.value(), "stale").get<bool>()) {
                    line += ", stale";
                }
                if (!field(it.value(), "live_error").is_null()) {
                    line += std::format("; live fetch failed: {}", field_text(it.value(), "live_error"));
                }
                items.push_back(line);
            }
            if (items.empty()) {
                return {};
            }
            std::vector<std::string> out{"", "Cached Codal data (`tse.codal snapshot --refresh` refetches):"};
            out.insert(out.end(), items.begin(), items.end());
            return out;
        }

        [[nodiscard]] std::string json_compact(json const& value) {
            return value.dump(-1, ' ', false, json::error_handler_t::replace);
        }

    } // namespace

    // ---------------------------------------------------------------- public API

    json resolve_symbol(client& http, std::string const& symbol) {
        auto const normalized = normalize_input(symbol);
        auto const code       = normal(normalized);
        bool const digits     = !code.empty() && std::all_of(code.begin(), code.end(), [](char c) {
            return c >= '0' && c <= '9';
        });
        if (digits) {
            return json{
              {"ins_code",    code},
              {  "symbol",      ""},
              {    "name",      ""},
              {    "isin", nullptr},
              {    "flow", nullptr},
              {  "market", nullptr}
            };
        }
        resolve_options options;
        try {
            auto const instrument = resolve_one(http, normalized, options);
            return json{
              {"ins_code", field(instrument, "ins_code").is_null() ? json(code) : field(instrument, "ins_code")},
              {"symbol", field(instrument, "symbol").is_null() ? json("") : field(instrument, "symbol")},
              {"name", field(instrument, "name").is_null() ? json("") : field(instrument, "name")},
              {"isin", field(instrument, "isin")},
              {"flow", field(instrument, "flow")},
              {"market", field(instrument, "market")}
            };
        } catch (ambiguous_error const& exc) {
            throw error(std::format("Expected one exact symbol for '{}'; candidates: {}. Use an insCode if ambiguous",
                                    normalized,
                                    format_candidates(exc.candidates)));
        } catch (not_found const&) { // NOLINT(bugprone-empty-catch): fall through to the raw code path.
        } catch (error const&) {     // NOLINT(bugprone-empty-catch): resolution failed; use the raw code.
        }
        if (is_isin(code)) {
            auto const found = resolve_isin(http, code);
            if (found.is_null()) {
                throw error(std::format("ISIN {} not found on TSETMC", code));
            }
            return found;
        }
        auto const        matches = search(http, normalized);
        std::vector<json> exact;
        for (auto const& row : matches) {
            if (normal(field_text(row, "symbol")) == code) {
                exact.push_back(row);
            }
        }
        if (exact.size() != 1) {
            auto const& candidates = exact.empty() ? matches : exact;
            std::string details;
            for (std::size_t i = 0; i < candidates.size() && i < 10; ++i) {
                if (i != 0) {
                    details += ", ";
                }
                details +=
                  std::format("{} ({})", field_text(candidates[i], "symbol"), field_text(candidates[i], "ins_code"));
            }
            throw error(std::format("Expected one exact symbol for '{}'; candidates: {}. Use an insCode if ambiguous",
                                    normalized,
                                    details.empty() ? "none" : details));
        }
        return exact.front();
    }

    json identity(client& http, std::string const& code) {
        return identity_impl(http, code);
    }

    json quote(client& http, std::string const& code) {
        return quote_impl(http, code);
    }

    json book(client& http, std::string const& code) {
        return book_impl(http, code);
    }

    json flow(client& http, std::string const& code) {
        return flow_impl(http, code);
    }

    json client_type_history(client& http, std::string const& code) {
        return client_type_history_impl(http, code);
    }

    json price_adjustments(client& http, std::string const& code) {
        return price_adjustments_impl(http, code);
    }

    json share_changes(client& http, std::string const& code) {
        return share_changes_impl(http, code);
    }

    json history(client& http, std::string const& code, int days, bool adjusted, json& errors) {
        return history_impl(http, code, days, adjusted, errors);
    }

    json technicals(json const& rows) {
        return technicals_impl(rows);
    }

    json corrections(json const& rows) {
        return corrections_impl(rows);
    }

    json info(client& http, std::string const& code, json const& ident, json const& current, json& errors) {
        return info_impl(http, code, ident, current, errors);
    }

    json economy(client& http) {
        return economy_impl(http);
    }

    json instrument_statistics(client& http, std::string const& code) {
        return instrument_statistics_impl(http, code);
    }

    json market_overview(client& http) {
        auto const row = http.api("MarketData/GetMarketOverview/1", "marketOverview");
        if (!row.is_object()) {
            throw error("Unexpected market overview result");
        }
        auto const index        = number(pick(row, {"indexLastValue"}));
        auto const change       = number(pick(row, {"indexChange"}));
        auto const equal        = number(pick(row, {"indexEqualWeightedLastValue"}));
        auto const equal_change = number(pick(row, {"indexEqualWeightedChange"}));
        auto       change_pct   = [](json const& current, json const& delta) -> json {
            if (current.is_null() || delta.is_null()) {
                return nullptr;
            }
            double const base = num_or(current) - num_or(delta);
            if (base == 0.0) {
                return nullptr;
            }
            return round2(num_or(delta) / base * 100.0);
        };
        return json{
          {"date", trade_date(pick(row, {"marketActivityDEven"}))},
          {"time", trade_time(pick(row, {"marketActivityHEven"}))},
          {"index_value", index},
          {"index_change", change},
          {"index_change_pct", change_pct(index, change)},
          {"equal_weighted_value", equal},
          {"equal_weighted_change_pct", change_pct(equal, equal_change)},
          {"trades", number(pick(row, {"marketActivityZTotTran"}))},
          {"volume_shares", number(pick(row, {"marketActivityQTotTran"}))},
          {"value_rial", number(pick(row, {"marketActivityQTotCap"}))},
          {"state", pick(row, {"marketStateTitle"})}
        };
    }

    json market_breadth(client& http, std::optional<std::string> const& code) {
        return market_breadth_impl(http, code);
    }

    json indexes(client& http) {
        return indexes_impl(http);
    }

    json collect(client& http, std::string const& symbol, market_options const& options) {
        auto const instrument = resolve_symbol(http, symbol);
        auto const code       = field_text(instrument, "ins_code");
        auto const now        = tehran_now();
        json       output;
        output["fetched_at"]          = jalali_stamp(now);
        output["market_session_open"] = market_session_open(now);
        output["instrument"]          = instrument;
        output["quote"]               = nullptr;
        output["order_book"]          = nullptr;
        output["client_type"]         = nullptr;
        output["indexes"]             = nullptr;
        output["statistics"]          = nullptr;
        output["technicals"]          = nullptr;
        output["corrections"]         = nullptr;
        output["last_book"]           = nullptr;
        output["market_context"]      = nullptr;
        output["history"]             = nullptr;
        output["fundamentals"]        = nullptr;
        output["errors"]              = nullptr;

        json ident{
          {"isin", field(instrument, "isin")},
          {"industry", nullptr},
          {"sub_industry", nullptr},
          {"board", nullptr},
          {"share_count", nullptr}
        };

        json       errors = json::object();
        std::mutex err_mutex;
        auto       note_error = [&](std::string const& key, std::string const& message) {
            std::lock_guard lock(err_mutex);
            errors[key] = message;
        };
        std::map<std::string, std::future<json>> futures;
        auto                                     submit = [&](std::string key, auto fn) {
            futures.emplace(std::move(key), std::async(std::launch::async, [fn = std::move(fn)]() -> json {
                                return fn();
                            }));
        };
        auto take = [&](std::string const& key) -> json {
            auto const it = futures.find(key);
            if (it == futures.end()) {
                return nullptr;
            }
            try {
                auto value = it->second.get();
                futures.erase(it);
                return value;
            } catch (error const& exc) {
                note_error(key, exc.what());
                futures.erase(it);
                return nullptr;
            } catch (std::exception const& exc) {
                note_error(key, exc.what());
                futures.erase(it);
                return nullptr;
            } catch (...) {
                note_error(key, "unknown exception");
                futures.erase(it);
                return nullptr;
            }
        };

        submit("identity", [&] {
            return identity_impl(http, code);
        });
        submit("quote", [&] {
            return quote_impl(http, code);
        });
        submit("order_book", [&] {
            return book_impl(http, code);
        });
        submit("client_type", [&] {
            return flow_impl(http, code);
        });
        submit("indexes", [&] {
            return indexes_impl(http);
        });
        submit("history", [&] {
            json history_errors = json::object();
            auto result         = history_impl(http, code, options.days, options.adjusted, history_errors);
            if (!history_errors.empty()) {
                std::lock_guard lock(err_mutex);
                for (auto it = history_errors.begin(); it != history_errors.end(); ++it) {
                    errors[it.key()] = it.value();
                }
            }
            return result;
        });
        submit("currencies", [&] {
            return market_currencies(http);
        });
        submit("economy", [&] {
            return economy_impl(http);
        });
        if (!options.skip.contains("statistics")) {
            submit("statistics", [&] {
                return instrument_statistics_impl(http, code);
            });
        }
        if (!options.skip.contains("breadth")) {
            submit("breadth", [&] {
                return market_breadth_impl(http, code);
            });
        }

        auto updated = take("identity");
        if (!updated.is_null()) {
            for (auto it = updated.begin(); it != updated.end(); ++it) {
                ident[it.key()] = it.value();
            }
            for (auto const name : {"symbol", "name", "isin"}) {
                if (field(instrument, name).is_null() || field_text(instrument, name).empty()) {
                    output["instrument"][name] = field(ident, name);
                }
            }
        }
        output["quote"] = take("quote");
        if (!options.skip.contains("statistics")) {
            output["statistics"] = take("statistics");
        }
        json const quote_snapshot = output["quote"];
        submit("fundamentals", [&, quote_snapshot, ident] {
            json local_errors = json::object();
            auto result =
              info_impl(http, code, ident, quote_snapshot.is_null() ? json::object() : quote_snapshot, local_errors);
            if (!local_errors.empty()) {
                std::lock_guard lock(err_mutex);
                for (auto it = local_errors.begin(); it != local_errors.end(); ++it) {
                    errors[it.key()] = it.value();
                }
            }
            return result;
        });
        output["order_book"]  = take("order_book");
        output["client_type"] = take("client_type");
        output["indexes"]     = take("indexes");
        output["history"]     = take("history");
        output["client_type"] = attach_session_values(
          output["client_type"],
          field(output["history"], "unadjusted").is_array() ? field(output["history"], "unadjusted") : json::array());
        auto const history_data = output["history"];
        if (!history_data.is_null()) {
            json rows = field(history_data, "adjusted");
            if (rows.is_null() || !rows.is_array() || rows.empty()) {
                rows = field(history_data, "unadjusted");
            }
            output["technicals"] = technicals_impl(rows);
            if (!options.skip.contains("corrections")) {
                output["corrections"] =
                  options.adjusted
                    ? corrections_impl(rows)
                    : json{
                        {"note",
                         "corrections need split/dividend-adjusted prices: run without "
                         "--unadjusted"}
                };
            }
        }
        if (!options.skip.contains("book")) {
            auto const session = last_completed_session(history_data, now);
            if (session) {
                submit("last_book", [&, session] {
                    return last_session_book(http, code, session->first, session->second);
                });
            }
        }
        json context = json::object();
        if (!options.skip.contains("breadth")) {
            auto const breadth = take("breadth");
            if (!breadth.is_null()) {
                context["breadth"] = breadth;
                auto const trend   = market_breadth_history_impl(http, code, breadth, 5);
                if (!trend.empty()) {
                    context["breadth_history"] = trend;
                }
            }
        }
        context["currencies"] = take("currencies");
        context["economy"]    = take("economy");
        if (futures.contains("last_book")) {
            output["last_book"] = take("last_book");
        }
        output["fundamentals"] = take("fundamentals");
        json filtered          = json::object();
        for (auto it = context.begin(); it != context.end(); ++it) {
            if (!it.value().is_null()) {
                filtered[it.key()] = it.value();
            }
        }
        output["market_context"] = filtered.empty() ? json(nullptr) : filtered;
        output["errors"]         = errors;
        if (output["quote"].is_null() &&
            (output["history"].is_null() || !field(output["history"], "unadjusted").is_array() ||
             field(output["history"], "unadjusted").empty()))
        {
            std::string details;
            for (auto it = errors.begin(); it != errors.end(); ++it) {
                if (!details.empty()) {
                    details += "; ";
                }
                details += text_of(it.value());
            }
            throw error("No price data available: " + details);
        }
        return output;
    }

    std::string markdown(json const& data) {
        auto const  when    = parse_moment(field_text(data, "fetched_at"));
        std::string fetched = "unknown";
        if (when) {
            fetched = jalali_stamp(*when);
        } else if (!field_text(data, "fetched_at").empty()) {
            fetched = field_text(data, "fetched_at");
        }
        std::vector<std::string> lines{
          std::format("Fetched at: {}", fetched),
          std::format("Market session open by schedule: {} (does not guarantee an instrument is trading)",
                      field(data, "market_session_open").is_boolean() && field(data, "market_session_open").get<bool>()
                        ? "True"
                        : "False"),
          "Prices and traded value are in rial, not toman. Missing values are unknown, not zero."};
        static std::array<std::pair<std::string_view, std::string_view>, 11> const sections{
          {
           {"Symbol / نماد", "instrument"},
           {"Live quote / قیمت و وضعیت", "quote"},
           {"Order book / صف خرید و فروش", "order_book"},
           {"Last session book / دفتر سفارش آخرین روز", "last_book"},
           {"Money flow / حقیقی و حقوقی", "client_type"},
           {"Market benchmarks / شاخص‌های بازار", "indexes"},
           {"Market context / دلار، اقتصاد و عمق بازار", "market_context"},
           {"Statistics / آمار معاملات", "statistics"},
           {"Technicals / تحلیل تکنیکال", "technicals"},
           {"Fundamentals / اطلاعات بنیادی", "fundamentals"},
           {"Codal / اطلاعیه‌ها و صورت‌های مالی", "codal"},
           }
        };
        for (auto const& [heading, key] : sections) {
            auto const value = field(data, key);
            if (value.is_null()) {
                continue;
            }
            lines.push_back("");
            lines.push_back(std::format("## {}", heading));
            if (key == "codal" && value.is_object()) {
                auto const cache_lines = codal_cache_lines(field(value, "cache"));
                lines.insert(lines.end(), cache_lines.begin(), cache_lines.end());
            }
            lines.push_back("```json");
            lines.push_back(json_compact(value));
            lines.push_back("```");
        }
        auto const hist = field(data, "history");
        if (!hist.is_null()) {
            std::vector<std::string> columns;
            std::vector<json>        rows;
            std::string              heading;
            if (hist.is_object() && hist.contains("unadjusted")) {
                auto const adjusted_rows = field(hist, "adjusted");
                if (adjusted_rows.is_array() && !adjusted_rows.empty()) {
                    columns = {
                      "date",
                      "first_rial",
                      "high_rial",
                      "low_rial",
                      "closing_rial",
                      "last_rial",
                      "adj_first_rial",
                      "adj_high_rial",
                      "adj_low_rial",
                      "adj_closing_rial",
                      "adj_last_rial",
                      "adj_factor",
                      "corporate_action",
                      "volume_shares",
                      "value_rial",
                      "trade_count"};
                    rows.assign(adjusted_rows.begin(), adjusted_rows.end());
                    heading = "## Price history / تاریخچه قیمت";
                } else {
                    columns               = {"date",
                                             "first_rial",
                                             "high_rial",
                                             "low_rial",
                                             "closing_rial",
                                             "last_rial",
                                             "volume_shares",
                                             "value_rial",
                                             "trade_count"};
                    auto const unadjusted = field(hist, "unadjusted");
                    rows.assign(unadjusted.begin(), unadjusted.end());
                    heading = "## Price history / تاریخچه قیمت (بدون تعدیل)";
                }
                for (auto const name : client_type_history_fields) {
                    columns.emplace_back(name);
                }
                lines.push_back("");
                lines.push_back(heading);
                lines.push_back("```csv");
                auto const block = csv_block(rows, columns);
                lines.insert(lines.end(), block.begin(), block.end());
                lines.push_back("```");
                auto const events = field(hist, "adjustment_events");
                auto const pa     = field(events, "price_adjustments");
                auto const sc     = field(events, "share_changes");
                if ((pa.is_array() && !pa.empty()) || (sc.is_array() && !sc.empty())) {
                    std::vector<json> event_rows;
                    for (auto const& a : pa) {
                        event_rows.push_back(json{
                          {"date", field(a, "date")},
                          {"type", "price_adjustment"},
                          {"details",
                           std::format("adj_ratio={:.4f} (pClosing_adj={}, pClosing_raw={})",
                           num_or(field(a, "adj_ratio")),
                           text_of(field(a, "pClosing_adj")),
                           text_of(field(a, "pClosing_raw")))}
                        });
                    }
                    for (auto const& s : sc) {
                        event_rows.push_back(json{
                          {"date", field(s, "date")},
                          {"type", "share_change"},
                          {"details",
                           std::format("ratio={:.4f} (old={:.0f}, new={:.0f})",
                           num_or(field(s, "ratio")),
                           num_or(field(s, "old_shares")),
                           num_or(field(s, "new_shares")))}
                        });
                    }
                    lines.push_back("");
                    lines.push_back("## Corporate action events / رویدادهای اقدامات شرکتی");
                    lines.push_back("```csv");
                    auto const block2 = csv_block(event_rows, {"date", "type", "details"});
                    lines.insert(lines.end(), block2.begin(), block2.end());
                    lines.push_back("```");
                }
            } else {
                columns = {"date",
                           "first_rial",
                           "high_rial",
                           "low_rial",
                           "closing_rial",
                           "last_rial",
                           "volume_shares",
                           "value_rial",
                           "trade_count"};
                for (auto const name : client_type_history_fields) {
                    columns.emplace_back(name);
                }
                rows.assign(hist.begin(), hist.end());
                lines.push_back("");
                lines.push_back("## Price history / تاریخچه قیمت (unadjusted)");
                lines.push_back("```csv");
                auto const block = csv_block(rows, columns);
                lines.insert(lines.end(), block.begin(), block.end());
                lines.push_back("```");
            }
        }
        if (!field(data, "corrections").is_null()) {
            auto const corr = corrections_markdown(field(data, "corrections"));
            lines.insert(lines.end(), corr.begin(), corr.end());
        }
        auto const errors = field(data, "errors");
        if (errors.is_object() && !errors.empty()) {
            lines.push_back("");
            lines.push_back("## Missing or failed sources / داده‌های ناموجود");
            lines.push_back("```json");
            lines.push_back(json_compact(errors));
            lines.push_back("```");
        }
        std::string out;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (i != 0) {
                out.push_back('\n');
            }
            out += lines[i];
        }
        return out;
    }

    std::string snapshot_meta_text(json const& meta, json const& data) {
        auto fetched = field_text(data, "fetched_at");
        if (fetched.empty()) {
            fetched = field_text(meta, "fetched_at");
        }
        std::string stale;
        auto const  when = parse_moment(fetched);
        if (when) {
            auto const now = tehran_now();
            stale          = (now - *when) > 86'400 ? "yes" : "no";
            fetched        = jalali_stamp(*when);
        } else {
            stale = fetched.empty() ? "" : "?";
        }
        auto        note = field_text(meta, "note");
        std::string collapsed;
        bool        in_space = false;
        for (char const c : note) {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                if (!collapsed.empty()) {
                    in_space = true;
                }
            } else {
                if (in_space) {
                    collapsed.push_back(' ');
                    in_space = false;
                }
                collapsed.push_back(c);
            }
        }
        std::vector<std::string> bits;
        for (auto const& [value, label] :
             std::array<std::pair<json, std::string_view>, 6>{
               {{field(meta, "symbol"), ""},
                {field(meta, "name"), ""},
                {field(meta, "isin"), "ISIN "},
                {field(meta, "ins_code"), "insCode "},
                {field(meta, "market"), ""},
                {field(meta, "source"), "source: "}}
        })
        {
            if (!value.is_null() && !text_of(value).empty()) {
                bits.push_back(std::string(label) + text_of(value));
            }
        }
        std::string instrument;
        if (!bits.empty()) {
            instrument = "Instrument: ";
            for (std::size_t i = 0; i < bits.size(); ++i) {
                if (i != 0) {
                    instrument += " — ";
                }
                instrument += bits[i];
            }
        }
        return std::format("{}\n{}\n{}\n{}", fetched.empty() ? "unknown" : fetched, stale, collapsed, instrument);
    }

    namespace {
        [[nodiscard]] json read_snapshot(std::filesystem::path const& path) {
            json doc;
            try {
                if (path == "-") {
                    std::string input((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
                    doc = json::parse(input);
                } else {
                    std::ifstream stream(path);
                    if (!stream) {
                        throw std::runtime_error("cannot open file");
                    }
                    doc = json::parse(stream);
                }
            } catch (std::exception const& exc) {
                throw error(std::format("{}: not valid JSON: {}", path.string(), exc.what()));
            }
            return doc;
        }
    } // namespace

    std::string snapshot_header(std::filesystem::path const& path) {
        auto const doc  = read_snapshot(path);
        auto const meta = field(doc, "meta");
        auto const data = field(doc, "data");
        if (!meta.is_object() || !data.is_object()) {
            throw error(
              std::format("{}: not a .stock snapshot (expected top-level meta and data objects)", path.string()));
        }
        return snapshot_meta_text(meta, data);
    }

    std::string snapshot_markdown(std::filesystem::path const& path) {
        auto const doc  = read_snapshot(path);
        auto const data = field(doc, "data");
        if (!data.is_object()) {
            throw error("data is not an object");
        }
        return markdown(data);
    }

    std::string all_markdown(client& http, std::string const& symbol, market_options const& options) {
        return markdown(collect(http, symbol, options));
    }

} // namespace dana::tools::tse
