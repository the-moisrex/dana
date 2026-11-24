#include "tse_resolver.hpp"

#include "finglish.hpp"
#include "tse_similarity.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dana::tools::tse {

    namespace {

        namespace fs = std::filesystem;

        [[nodiscard]] std::string as_string(json const& value) {
            return scalar_text(value);
        }

        [[nodiscard]] json field(json const& object, std::string_view key) {
            if (!object.is_object()) {
                return nullptr;
            }
            auto const it = object.find(std::string(key));
            return it == object.end() ? json(nullptr) : *it;
        }

        [[nodiscard]] std::string field_text(json const& object, std::string_view key) {
            return as_string(field(object, key));
        }

        [[nodiscard]] bool falsy(json const& value) {
            if (value.is_null()) {
                return true;
            }
            if (value.is_boolean()) {
                return !value.get<bool>();
            }
            if (value.is_string()) {
                return value.get<std::string>().empty();
            }
            if (value.is_array()) {
                return value.empty();
            }
            if (value.is_number()) {
                return value.get<double>() == 0.0;
            }
            return false;
        }

        [[nodiscard]] json clean(json const& value) {
            if (value.is_null()) {
                return nullptr;
            }
            auto       text  = as_string(value);
            auto const first = text.find_first_not_of(" \t\n\r\f\v");
            if (first == std::string::npos) {
                return nullptr;
            }
            auto const last = text.find_last_not_of(" \t\n\r\f\v");
            return text.substr(first, last - first + 1);
        }

        [[nodiscard]] bool persian(std::string_view text) {
            for (std::size_t i = 0; i + 1 < text.size(); ++i) {
                auto const c = static_cast<unsigned char>(text[i]);
                if (c == 0xD8 || c == 0xD9 || c == 0xDA || c == 0xDB) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] std::optional<std::string> latin_only(std::string_view text) {
            if (text.empty() || persian(text)) {
                return std::nullopt;
            }
            std::string cleaned;
            cleaned.reserve(text.size());
            for (char const raw : text) {
                auto const c = static_cast<unsigned char>(raw);
                if (c >= 'A' && c <= 'Z') {
                    cleaned.push_back(static_cast<char>(c - 'A' + 'a'));
                } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
                    cleaned.push_back(static_cast<char>(c));
                }
            }
            if (cleaned.empty()) {
                return std::nullopt;
            }
            return cleaned;
        }

        [[nodiscard]] std::string fold_latin(std::string_view text) {
            std::string out;
            out.reserve(text.size());
            for (char const c : text) {
                if (!out.empty() && out.back() == c) {
                    continue;
                }
                out.push_back(c);
            }
            return out;
        }

        [[nodiscard]] std::optional<std::string> reverse_persian(std::optional<std::string> const& latin) {
            if (!latin || latin->empty()) {
                return std::nullopt;
            }
            static finglish_transliterator const transliterator;
            auto                                 candidate = normal(transliterator.finglish_to_persian(*latin));
            if (candidate.empty()) {
                return std::nullopt;
            }
            return candidate;
        }

        [[nodiscard]] std::pair<std::vector<std::string>, std::vector<std::string>> latin_keys(
          std::string_view symbol,
          std::string_view name) {
            static finglish_transliterator const transliterator;

            auto tokenize = [](std::string_view text, std::size_t min_len) {
                std::set<std::string> keys;
                if (text.empty()) {
                    return keys;
                }
                auto const               raw = transliterator.persian_to_finglish(text);
                std::vector<std::string> tokens;
                std::string              current;
                for (char const raw_char : raw) {
                    auto const c = static_cast<unsigned char>(raw_char);
                    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
                        current.push_back(static_cast<char>(c));
                    } else if (!current.empty()) {
                        tokens.push_back(current);
                        current.clear();
                    }
                }
                if (!current.empty()) {
                    tokens.push_back(current);
                }
                std::string joined;
                for (auto const& token : tokens) {
                    joined += token;
                    if (token.size() >= min_len) {
                        keys.insert(token);
                        keys.insert(fold_latin(token));
                    }
                }
                if (joined.size() >= min_len) {
                    keys.insert(joined);
                    keys.insert(fold_latin(joined));
                }
                return keys;
            };

            auto symbol_keys = tokenize(symbol, 2);
            auto name_keys   = tokenize(name, 4);
            for (auto const& key : symbol_keys) {
                name_keys.erase(key);
            }
            return {std::vector<std::string>(symbol_keys.begin(), symbol_keys.end()),
                    std::vector<std::string>(name_keys.begin(), name_keys.end())};
        }

        [[nodiscard]] std::string guess_type(json const& isin_value, std::string_view name) {
            auto const isin   = isin_value.is_string() ? isin_value.get<std::string>() : std::string{};
            auto const starts = [&isin](std::string_view prefix) {
                return isin.starts_with(prefix);
            };
            if (starts("IRO9") || starts("IROA") || starts("IROB") || starts("IROF")) {
                return "option";
            }
            if (starts("IRO4")) {
                return "future";
            }
            if (starts("IRO6")) {
                return "mortgage";
            }
            if (starts("IRO1") || starts("IRO3") || starts("IRO5") || starts("IRO7")) {
                return "stock";
            }
            auto const head = isin.substr(0, std::min<std::size_t>(4, isin.size()));
            if (head == "IRT1" || head == "IRT3" || head == "IRTK" || head == "IRTE") {
                return "fund";
            }
            if (starts("IRR")) {
                return "right";
            }
            if (starts("IRBK") || starts("IRBE") || head == "IRK1") {
                return "forward";
            }
            if (head == "IRB3") {
                return "bond";
            }
            auto const text = normal(name);
            if (text.find("اختيار") != std::string::npos || text.find("اختیار") != std::string::npos) {
                return "option";
            }
            if (text.find("صندوق") != std::string::npos) {
                return "fund";
            }
            if (text.find("سلف") != std::string::npos) {
                return "forward";
            }
            for (auto const word : {"مرابحه", "مشاركت", "مشارکت", "صكوك", "صکوک", "اراد", "گام"}) {
                if (text.find(word) != std::string::npos) {
                    return "bond";
                }
            }
            if (text.find("آتي") != std::string::npos || text.find("آتی") != std::string::npos) {
                return "future";
            }
            if (text.find("تسه") != std::string::npos || text.find("امتياز") != std::string::npos ||
                text.find("امتیاز") != std::string::npos)
            {
                return "mortgage";
            }
            if (text.starts_with("ح.") || text.starts_with("ح ")) {
                return "right";
            }
            return text.empty() ? "stock" : "other";
        }

        [[nodiscard]] std::pair<std::string, std::string> parse_identifier(std::string_view text) {
            std::string raw(text);
            auto const  first = raw.find_first_not_of(" \t\n\r\f\v");
            if (first == std::string::npos) {
                return {"text", ""};
            }
            auto const last = raw.find_last_not_of(" \t\n\r\f\v");
            raw             = raw.substr(first, last - first + 1);

            auto const              normalized = normalize_input(raw);
            static std::regex const isin_re(R"(IR[A-Z0-9]{10})");
            std::smatch             match;
            if (std::regex_search(normalized, match, isin_re)) {
                auto value = match.str();
                for (char& c : value) {
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                }
                return {"isin", value};
            }
            static std::regex const code_re(R"([0-9]{12,})");
            if (std::regex_search(normalized, match, code_re)) {
                return {"ins_code", match.str()};
            }
            return {"text", raw};
        }

        [[nodiscard]] double market_rank(json const& flow, json const& isin_value) {
            if (!flow.is_null()) {
                auto const value = as_int(flow);
                if (value) {
                    static std::unordered_map<long long, double> const ranks{
                      {1, 0.0},
                      {2, 1.0},
                      {6, 1.0},
                      {7, 1.0},
                      {4, 2.0},
                      {3, 3.0}
                    };
                    auto const it = ranks.find(*value);
                    return it != ranks.end() ? it->second : 2.5;
                }
            }
            auto code = isin_value.is_string() ? isin_value.get<std::string>() : std::string{};
            for (char& c : code) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            if (code.starts_with("IRO7")) {
                return 2.0;
            }
            if (code.starts_with("IRO9") || code.starts_with("IRO4") || code.starts_with("IRO6") ||
                code.starts_with("IROA") || code.starts_with("IROB") || code.starts_with("IROF"))
            {
                return 3.0;
            }
            if (code.starts_with("IRO3")) {
                return 1.0;
            }
            if (code.starts_with("IRO1") || code.starts_with("IRO2") || code.starts_with("IRO5")) {
                return 0.0;
            }
            return 2.5;
        }

        [[nodiscard]] double board_rank(json const& board_code) {
            auto code = board_code.is_string() ? board_code.get<std::string>() : std::string{};
            for (char& c : code) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            return (code == "P1" || code == "P2" || code == "C1" || code == "L1") ? 1.0 : 0.0;
        }

        [[nodiscard]] std::string board_title(std::string_view code) {
            static std::unordered_map<std::string, std::string> const titles{
              {"1A",    "بازار ابزارهاي نوين مالي فرابورس"},
              {"1N",        "بازار اول (تابلوی اصلی) بورس"},
              {"1Z",                   "بازار اول فرابورس"},
              {"2A",    "بازار ابزارهاي نوين مالي فرابورس"},
              {"2Z",                   "بازار دوم فرابورس"},
              {"41",            "بازار ابزارهاي نوين مالي"},
              {"4A",    "بازار ابزارهاي نوين مالي فرابورس"},
              {"51",         "بازار صندوق های قابل معامله"},
              {"5A",         "بازار صندوق های قابل معامله"},
              {"5N",         "بازار صندوق های قابل معامله"},
              {"61",                "بازار ابزارهای مشتقه"},
              {"71",                "بازار ابزارهای مشتقه"},
              {"91",                "بازار ابزارهای مشتقه"},
              {"C1",           "بازار پایه نارنجی فرابورس"},
              {"D1",                   "بازار دوم فرابورس"},
              {"EQ", "بازار سایر اوراق بهادار قابل معامله"},
              {"F1",                      "بازار عادی آتی"},
              {"G1",        "بازار دوم (تابلوی اصلی) بورس"},
              {"H1",         "بازار صندوق های قابل معامله"},
              {"HN",         "بازار صندوق های قابل معامله"},
              {"I1",    "بازار ابزارهاي نوين مالي فرابورس"},
              {"I2",    "بازار ابزارهاي نوين مالي فرابورس"},
              {"K2",                   "بازار دوم فرابورس"},
              {"L1",             "بازار پایه قرمز فرابورس"},
              {"M1",    "بازار ابزارهاي نوين مالي فرابورس"},
              {"M2",    "بازار ابزارهاي نوين مالي فرابورس"},
              {"N1",        "بازار اول (تابلوی فرعی) بورس"},
              {"N2",        "بازار دوم (تابلوی فرعی) بورس"},
              {"N4",                    "بازار اوراق بدهی"},
              {"NS",          "بازار سرمایه گذاری حرفه ای"},
              {"O1",               "بازار خرده فروشی بورس"},
              {"P1",              "بازار پایه زرد فرابورس"},
              {"P2",              "بازار پایه زرد فرابورس"},
              {"Q1",                           "بورس کالا"},
              {"QS",                    "صندوق های کالایی"},
              {"T1",    "بازار ابزارهاي نوين مالي فرابورس"},
              {"T2",    "بازار ابزارهاي نوين مالي فرابورس"},
              {"V1",                 "بازار نوآفرین - رشد"},
              {"W1",          "بازار نوآفرین - دانش بنیان"},
              {"Z1",                   "بازار دوم فرابورس"}
            };
            auto upper = std::string(code);
            for (char& c : upper) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            auto const it = titles.find(upper);
            return it != titles.end() ? it->second : std::string{};
        }

        [[nodiscard]] std::string flow_name(json const& flow) {
            auto const value = as_int(flow);
            if (!value) {
                return {};
            }
            switch (*value) {
                case 1: return "بورس";
                case 2:
                case 6:
                case 7: return "فرابورس";
                case 3: return "مشتقه";
                case 4: return "پایه";
                default: return {};
            }
        }

        [[nodiscard]] json market_instrument(json const& row) {
            auto const code = clean(pick(row, {"insCode"}));
            if (code.is_null()) {
                return nullptr;
            }
            auto const symbol = normal(as_string(pick(row, {"lva", "lVal18AFC"})));
            auto const name   = normal(as_string(pick(row, {"lvc", "lVal30"})));
            auto       isin   = clean(pick(row, {"insID", "cIsin", "instrumentID"}));
            if (!isin.is_null()) {
                auto upper = as_string(isin);
                for (char& c : upper) {
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                }
                static std::regex const isin_re(R"(IR[A-Z0-9]{10})");
                if (!std::regex_match(upper, isin_re)) {
                    isin = upper.starts_with("IR") ? json(upper) : json(nullptr);
                }
            }
            auto const flow   = number(pick(row, {"flow"}));
            auto const kind   = number(pick(row, {"yVal"}));
            auto const shares = number(pick(row, {"ztd"}));
            auto       close  = price(pick(row, {"pcl"}));
            if (close.is_null()) {
                close = price(pick(row, {"pdv"}));
            }
            auto const value      = number(pick(row, {"qtc"}));
            auto const board_code = clean(pick(row, {"cGrValCot", "cgrValCot"}));

            auto const [symbol_keys, name_keys] = latin_keys(symbol, name);

            json record;
            record["ins_code"]   = code;
            record["symbol"]     = symbol;
            record["name"]       = name;
            record["isin"]       = isin;
            record["flow"]       = flow;
            auto market          = clean(pick(row, {"flowTitle"}));
            record["market"]     = market.is_null() ? json(flow_name(flow)) : market;
            record["board_code"] = board_code;
            auto board           = clean(pick(row, {"cgrValCotTitle"}));
            record["board"]      = board.is_null() ? json(board_title(as_string(board_code))) : board;
            record["kind"]       = kind;
            if (kind.is_null()) {
                record["type"] = guess_type(isin, name);
            } else {
                static std::unordered_map<long long, std::string> const kinds{
                  {300,    "stock"},
                  {303,    "stock"},
                  {309,    "stock"},
                  {313,    "stock"},
                  {305,     "fund"},
                  {380,     "fund"},
                  {315,     "fund"},
                  {311,   "option"},
                  {312,   "option"},
                  {320,   "option"},
                  {321,   "option"},
                  {304,   "future"},
                  {307, "mortgage"},
                  {400,    "right"},
                  {403,    "right"},
                  {404,    "right"},
                  {306,     "bond"},
                  {706,     "bond"},
                  {301,     "bond"},
                  {206,     "bond"},
                  {208,     "bond"},
                  {200,  "forward"},
                  {308,  "forward"},
                  {701,  "forward"},
                  {201,    "other"},
                  {801,    "other"},
                  {802,    "other"},
                  {803,    "other"}
                };
                auto const kind_value = as_int(kind);
                auto const it         = kind_value ? kinds.find(*kind_value) : kinds.end();
                record["type"]        = it != kinds.end() ? it->second : guess_type(isin, name);
            }
            record["traded"]     = !value.is_null() && value.get<double>() > 0;
            record["value"]      = value.is_null() ? json(nullptr) : json(static_cast<long long>(value.get<double>()));
            record["mcap"]       = (!shares.is_null() && !close.is_null())
                                     ? json(static_cast<long long>(shares.get<double>() * close.get<double>()))
                                     : json(nullptr);
            record["latin"]      = symbol_keys;
            record["name_latin"] = name_keys;
            return record;
        }

        [[nodiscard]] json search_instrument(json const& row) {
            auto const code = clean(pick(row, {"insCode"}));
            if (code.is_null()) {
                return nullptr;
            }
            auto const symbol                   = normal(as_string(pick(row, {"lVal18AFC", "lVal18"})));
            auto const name                     = normal(as_string(pick(row, {"lVal30"})));
            auto const isin                     = clean(pick(row, {"cIsin", "insID"}));
            auto const flow                     = number(pick(row, {"flow"}));
            auto const board_code               = clean(pick(row, {"cgrValCot"}));
            auto const [symbol_keys, name_keys] = latin_keys(symbol, name);
            json record;
            record["ins_code"]   = code;
            record["symbol"]     = symbol;
            record["name"]       = name;
            record["isin"]       = isin;
            record["flow"]       = flow;
            auto market          = clean(pick(row, {"flowTitle"}));
            record["market"]     = market.is_null() ? json(flow_name(flow)) : market;
            record["board_code"] = board_code;
            auto board           = clean(pick(row, {"cgrValCotTitle"}));
            record["board"]      = board.is_null() ? json(board_title(as_string(board_code))) : board;
            record["kind"]       = nullptr;
            record["type"]       = guess_type(isin, name);
            record["traded"]     = false;
            record["value"]      = nullptr;
            record["mcap"]       = nullptr;
            record["latin"]      = symbol_keys;
            record["name_latin"] = name_keys;
            return record;
        }

        [[nodiscard]] json
        synthetic(json const& ins_code, json const& isin, std::string_view symbol, std::string_view name) {
            auto const [symbol_keys, name_keys] = latin_keys(symbol, name);
            json record;
            record["ins_code"]   = ins_code.is_null() ? json("") : ins_code;
            record["symbol"]     = std::string(symbol);
            record["name"]       = std::string(name);
            record["isin"]       = isin;
            record["flow"]       = nullptr;
            record["market"]     = nullptr;
            record["board_code"] = nullptr;
            record["board"]      = nullptr;
            record["kind"]       = nullptr;
            record["type"]       = guess_type(isin, name);
            record["traded"]     = false;
            record["value"]      = nullptr;
            record["mcap"]       = nullptr;
            record["latin"]      = symbol_keys;
            record["name_latin"] = name_keys;
            return record;
        }

        [[nodiscard]] std::string iso_now() {
            auto const now = std::chrono::system_clock::now();
            auto const tt  = std::chrono::system_clock::to_time_t(now);
            std::tm    tm{};
            ::gmtime_r(&tt, &tm);
            return std::format(
              "{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}+00:00",
              tm.tm_year + 1900,
              tm.tm_mon + 1,
              tm.tm_mday,
              tm.tm_hour,
              tm.tm_min,
              tm.tm_sec);
        }

        [[nodiscard]] std::optional<std::time_t> parse_iso(std::string_view text) {
            if (text.size() < 19) {
                return std::nullopt;
            }
            auto const read_field = [&text](std::size_t pos, int& out) -> std::size_t {
                auto const [ptr, ec] = std::from_chars(text.data() + pos, text.data() + text.size(), out);
                if (ec != std::errc{}) {
                    return 0;
                }
                return static_cast<std::size_t>(ptr - text.data());
            };
            int        year       = 0;
            int        month      = 0;
            int        day        = 0;
            int        hour       = 0;
            int        minute     = 0;
            int        second     = 0;
            auto const after_year = read_field(0, year);
            if (!after_year || text[after_year] != '-') {
                return std::nullopt;
            }
            auto const after_month = read_field(after_year + 1, month);
            if (!after_month || text[after_month] != '-') {
                return std::nullopt;
            }
            auto const after_day = read_field(after_month + 1, day);
            if (!after_day || text[after_day] != 'T') {
                return std::nullopt;
            }
            auto const after_hour = read_field(after_day + 1, hour);
            if (!after_hour || text[after_hour] != ':') {
                return std::nullopt;
            }
            auto const after_minute = read_field(after_hour + 1, minute);
            if (!after_minute || text[after_minute] != ':') {
                return std::nullopt;
            }
            auto const after_second = read_field(after_minute + 1, second);
            if (!after_second || after_second != 19) {
                return std::nullopt;
            }
            std::tm tm{};
            tm.tm_year = year - 1900;
            tm.tm_mon  = month - 1;
            tm.tm_mday = day;
            tm.tm_hour = hour;
            tm.tm_min  = minute;
            tm.tm_sec  = second;
            return ::timegm(&tm);
        }

        [[nodiscard]] bool cache_fresh(json const& data, int ttl_days) {
            auto const fetched = field_text(data, "fetched_at");
            auto const moment  = parse_iso(fetched);
            if (!moment) {
                return false;
            }
            auto const   now      = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            double const age_days = static_cast<double>(now - *moment) / 86400.0;
            return age_days < static_cast<double>(ttl_days);
        }

        [[nodiscard]] std::vector<json> build_index(client& http) {
            auto const                      rows = fetch_market_watch(http);
            std::vector<json>               index;
            std::unordered_set<std::string> seen;
            for (auto const& row : rows) {
                auto const record = market_instrument(row);
                if (record.is_null()) {
                    continue;
                }
                auto const code = field_text(record, "ins_code");
                if (!seen.insert(code).second) {
                    continue;
                }
                index.push_back(record);
            }
            return index;
        }

        [[nodiscard]] std::size_t merge(std::vector<json>& index, std::vector<json> const& records) {
            std::unordered_map<std::string, std::size_t> known;
            for (std::size_t i = 0; i < index.size(); ++i) {
                known[field_text(index[i], "ins_code")] = i;
            }
            std::size_t added = 0;
            for (auto const& record : records) {
                if (record.is_null()) {
                    continue;
                }
                auto const code = field_text(record, "ins_code");
                auto const it   = known.find(code);
                if (it == known.end()) {
                    known[code] = index.size();
                    index.push_back(record);
                    ++added;
                    continue;
                }
                auto& current = index[it->second];
                for (auto field_it = record.begin(); field_it != record.end(); ++field_it) {
                    if (falsy(field_it.value())) {
                        continue;
                    }
                    auto const existing = current.find(field_it.key());
                    if (existing == current.end() || falsy(*existing)) {
                        current[field_it.key()] = field_it.value();
                    }
                }
            }
            return added;
        }

        [[nodiscard]] std::vector<json> lookup_live(client& http, std::string const& term) {
            if (term.empty()) {
                return {};
            }
            std::vector<json> records;
            try {
                for (auto const& row : search(http, term)) {
                    if (!row.is_object() || field_text(row, "ins_code").empty()) {
                        continue;
                    }
                    auto const record = search_instrument(row);
                    if (!record.is_null()) {
                        records.push_back(record);
                    }
                }
            } catch (...) {
                return {};
            }
            return records;
        }

        [[nodiscard]] std::string
        best_latin_key(std::string const& query, std::vector<std::string> const& keys, bool& exact) {
            exact = false;
            if (keys.empty()) {
                return {};
            }
            if (std::find(keys.begin(), keys.end(), query) != keys.end()) {
                exact = true;
                return query;
            }
            std::string best;
            for (auto const& key : keys) {
                if (key.starts_with(query) && (best.empty() || key.size() < best.size())) {
                    best = key;
                }
            }
            if (!best.empty()) {
                return best;
            }
            for (auto const& key : keys) {
                if (key.find(query) != std::string::npos && (best.empty() || key.size() < best.size())) {
                    best = key;
                }
            }
            if (!best.empty()) {
                return best;
            }
            double best_score = -1.0;
            for (auto const& key : keys) {
                double const score = ratio(query, key);
                if (score > best_score) {
                    best_score = score;
                    best       = key;
                }
            }
            return best;
        }

        [[nodiscard]] std::string completion_token(json const&                       instrument,
                                                   std::optional<std::string> const& latin_query) {
            auto const symbol = field_text(instrument, "symbol");
            auto       suffix = field(instrument, "isin");
            if (suffix.is_null() || as_string(suffix).empty()) {
                suffix = field(instrument, "ins_code");
            }
            auto const suffix_text = as_string(suffix);
            if (!latin_query) {
                return token_for(instrument);
            }
            bool                     exact = false;
            auto const               keys  = field(instrument, "latin");
            std::vector<std::string> latin;
            if (keys.is_array()) {
                for (auto const& key : keys) {
                    latin.push_back(as_string(key));
                }
            }
            auto const  key = best_latin_key(*latin_query, latin, exact);
            std::string name;
            if (exact || (!key.empty() && latin_query->size() >= 4 && ratio(*latin_query, key) >= finglish_key_score)) {
                name = *latin_query;
            } else if (!key.empty()) {
                name = key;
            } else {
                name = symbol;
            }
            if (!name.empty() && !suffix_text.empty()) {
                return name + "-" + suffix_text;
            }
            return !name.empty() ? name : suffix_text;
        }

        [[nodiscard]] std::vector<std::string> string_list(json const& value) {
            std::vector<std::string> out;
            if (value.is_array()) {
                for (auto const& item : value) {
                    out.push_back(as_string(item));
                }
            }
            return out;
        }

        [[nodiscard]] bool rank_less(json const& a, json const& b) {
            auto const ai     = field(a, "instrument");
            auto const bi     = field(b, "instrument");
            auto const tier_a = as_int(field(a, "tier")).value_or(0);
            auto const tier_b = as_int(field(b, "tier")).value_or(0);
            if (tier_a != tier_b) {
                return tier_a < tier_b;
            }
            auto const score_a = as_double(field(a, "score")).value_or(0.0);
            auto const score_b = as_double(field(b, "score")).value_or(0.0);
            if (score_a != score_b) {
                return score_a > score_b;
            }
            auto const mr_a = as_double(field(a, "market_rank")).value_or(0.0);
            auto const mr_b = as_double(field(b, "market_rank")).value_or(0.0);
            if (mr_a != mr_b) {
                return mr_a < mr_b;
            }
            auto const traded_a = field(ai, "traded").is_boolean() && field(ai, "traded").get<bool>();
            auto const traded_b = field(bi, "traded").is_boolean() && field(bi, "traded").get<bool>();
            if (traded_a != traded_b) {
                return traded_a;
            }
            auto const br_a = board_rank(field(ai, "board_code"));
            auto const br_b = board_rank(field(bi, "board_code"));
            if (br_a != br_b) {
                return br_a < br_b;
            }
            auto const value_a = as_double(field(ai, "value")).value_or(0.0);
            auto const value_b = as_double(field(bi, "value")).value_or(0.0);
            if (value_a != value_b) {
                return value_a > value_b;
            }
            auto const mcap_a = as_double(field(ai, "mcap")).value_or(0.0);
            auto const mcap_b = as_double(field(bi, "mcap")).value_or(0.0);
            return mcap_a > mcap_b;
        }

        [[nodiscard]] std::vector<json> score_index(
          std::vector<json> const&          index,
          std::string const&                query,
          std::optional<std::string> const& query_latin,
          std::optional<std::string> const& reverse,
          double                            min_score) {
            std::vector<json> candidates;
            for (auto const& instrument : index) {
                auto const  symbol           = field_text(instrument, "symbol");
                auto const  name             = field_text(instrument, "name");
                int         tier             = 2;
                double      score            = 0.0;
                std::string matched          = "fuzzy";
                double      symbol_key_score = 0.0;
                auto const  latin            = string_list(field(instrument, "latin"));
                if (query_latin) {
                    for (auto const& key : latin) {
                        symbol_key_score = std::max(symbol_key_score, ratio(*query_latin, key));
                    }
                }
                if (!query.empty() && (query == symbol || query == name)) {
                    tier    = 0;
                    score   = 100.0;
                    matched = "exact";
                } else if (query_latin && (std::find(latin.begin(), latin.end(), *query_latin) != latin.end() ||
                                           (reverse && (*reverse == symbol || *reverse == name))))
                {
                    tier    = 1;
                    score   = 100.0;
                    matched = "finglish";
                } else if (
                  query_latin && query_latin->size() >= finglish_query_min && symbol_key_score >= finglish_key_score)
                {
                    tier    = 1;
                    score   = symbol_key_score;
                    matched = "finglish";
                }
                if (tier == 2) {
                    double best = 0.0;
                    if (!query.empty()) {
                        if (!symbol.empty()) {
                            best = std::max(best, wratio(query, symbol));
                        }
                        if (!name.empty()) {
                            best = std::max(best, wratio(query, name));
                        }
                    }
                    if (query_latin) {
                        best = std::max(best, symbol_key_score);
                        for (auto const& key : string_list(field(instrument, "name_latin"))) {
                            best = std::max(best, ratio(*query_latin, key));
                        }
                        if (reverse) {
                            if (!symbol.empty()) {
                                best = std::max(best, wratio(*reverse, symbol));
                            }
                            if (!name.empty()) {
                                best = std::max(best, wratio(*reverse, name));
                            }
                        }
                    }
                    score = best;
                }
                if (tier <= 1 || score >= min_score) {
                    candidates.push_back(make_candidate(instrument, tier, score, matched));
                }
            }
            return candidates;
        }

    } // namespace

    json make_candidate(json instrument, int tier, double score, std::string_view matched) {
        json candidate;
        candidate["instrument"] = std::move(instrument);
        candidate["tier"]       = tier;
        candidate["score"]      = round_to(score, 1);
        candidate["market_rank"] =
          market_rank(field(candidate["instrument"], "flow"), field(candidate["instrument"], "isin"));
        candidate["matched"] = std::string(matched);
        return candidate;
    }

    std::filesystem::path default_cache_path() {
        std::string root;
        auto const* xdg = std::getenv("XDG_CACHE_HOME"); // NOLINT(concurrency-mt-unsafe): read once at startup.
        if (xdg != nullptr && *xdg != '\0') {
            root = xdg;
        } else {
            auto const* home = std::getenv("HOME"); // NOLINT(concurrency-mt-unsafe): read once at startup.
            if (home != nullptr && *home != '\0') {
                root = std::string(home) + "/.cache";
            } else {
                root = ".cache";
            }
        }
        return fs::path(root) / "tse" / "instruments.json";
    }

    std::optional<json> read_cache(std::filesystem::path const& path) {
        std::ifstream input(path);
        if (!input) {
            return std::nullopt;
        }
        auto data = json::parse(input, nullptr, false);
        if (data.is_discarded() || !data.is_object()) {
            return std::nullopt;
        }
        auto const version = as_int(field(data, "version"));
        if (!version || *version != cache_version) {
            return std::nullopt;
        }
        if (!field(data, "instruments").is_array()) {
            return std::nullopt;
        }
        return std::optional<json>{std::move(data)};
    }

    bool write_cache(std::filesystem::path const&      path,
                     std::vector<json> const&          instruments,
                     std::string_view                  source,
                     std::optional<std::string> const& fetched_at) {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        json document;
        document["version"]     = cache_version;
        document["fetched_at"]  = fetched_at ? *fetched_at : iso_now();
        document["source"]      = std::string(source);
        document["count"]       = instruments.size();
        document["instruments"] = instruments;
        auto const tmp          = path.parent_path() / (path.filename().string() + ".tmp");
        {
            std::ofstream output(tmp, std::ios::binary | std::ios::trunc);
            if (!output) {
                return false;
            }
            output << document.dump() << '\n';
            if (!output) {
                return false;
            }
        }
        fs::rename(tmp, path, ec);
        return !ec;
    }

    index_info
    get_index(client& http, resolve_options const& options, std::optional<std::filesystem::path> cache_path) {
        auto const path = cache_path ? *cache_path : default_cache_path();
        auto       data = options.no_cache ? std::optional<json>{} : read_cache(path);
        if (data && !options.refresh) {
            bool const fresh = cache_fresh(*data, std::max(options.ttl_days, 0));
            if (fresh || options.offline) {
                return {
                  field(*data, "instruments").get<std::vector<json>>(),
                  json{{"source", "cache"},
                       {"fresh", fresh},
                       {"fetched_at", field(*data, "fetched_at")},
                       {"path", path.string()}}
                };
            }
        }
        if (options.offline) {
            if (data) {
                return {
                  field(*data, "instruments").get<std::vector<json>>(),
                  json{{"source", "cache-stale"},
                       {"fresh", false},
                       {"fetched_at", field(*data, "fetched_at")},
                       {"path", path.string()}}
                };
            }
            return {
              {},
              json{{"source", "none"}, {"fresh", false}, {"path", path.string()}}
            };
        }
        std::vector<json> instruments;
        try {
            instruments = build_index(http);
        } catch (...) {
            if (data) {
                return {
                  field(*data, "instruments").get<std::vector<json>>(),
                  json{{"source", "cache-stale"},
                       {"fresh", false},
                       {"fetched_at", field(*data, "fetched_at")},
                       {"path", path.string()}}
                };
            }
            throw;
        }
        (void) write_cache(path, instruments, "marketwatch");
        return {
          instruments,
          json{{"source", "marketwatch"}, {"fresh", true}, {"path", path.string()}}
        };
    }

    std::vector<json> resolve_query(
      client&                              http,
      std::string const&                   query,
      resolve_options const&               options,
      std::optional<std::filesystem::path> cache_path,
      std::optional<std::vector<json>>     index) {
        auto const path = cache_path ? *cache_path : default_cache_path();
        json       meta{
          {"fetched_at", nullptr}
        };
        std::vector<json> instruments;
        if (index) {
            instruments = *index;
        } else {
            auto loaded = get_index(http, options, path);
            instruments = std::move(loaded.instruments);
            meta        = std::move(loaded.meta);
        }

        if (!options.types.empty()) {
            static std::unordered_map<std::string, std::string> const aliases{
              {   "stock",    "stock"},
              {  "stocks",    "stock"},
              {  "equity",    "stock"},
              {     "سهم",    "stock"},
              {    "fund",     "fund"},
              {   "funds",     "fund"},
              {     "etf",     "fund"},
              {   "صندوق",     "fund"},
              {  "option",   "option"},
              { "options",   "option"},
              {    "bond",     "bond"},
              {   "bonds",     "bond"},
              {   "right",    "right"},
              {  "rights",    "right"},
              {      "حق",    "right"},
              {  "future",   "future"},
              { "futures",   "future"},
              {"mortgage", "mortgage"}
            };
            std::unordered_set<std::string> wanted;
            for (auto item : options.types) {
                for (char& c : item) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                auto const it = aliases.find(item);
                wanted.insert(it != aliases.end() ? it->second : item);
            }
            std::vector<json> filtered;
            for (auto const& inst : instruments) {
                if (wanted.contains(field_text(inst, "type"))) {
                    filtered.push_back(inst);
                }
            }
            instruments = std::move(filtered);
        }
        if (!options.markets.empty()) {
            static std::unordered_map<std::string, std::vector<long long>> const aliases{
              {    "bourse",       {1}},
              {       "tse",       {1}},
              {         "1",       {1}},
              {"farabourse", {2, 6, 7}},
              {       "ifb", {2, 6, 7}},
              {      "fara", {2, 6, 7}},
              {      "paye",       {4}},
              {      "base",       {4}},
              {         "4",       {4}},
              {"derivative",       {3}},
              { "moshtaghe",       {3}},
              {         "3",       {3}},
              {         "6",       {6}},
              {         "7",       {7}}
            };
            std::unordered_set<long long> wanted_flows;
            for (auto item : options.markets) {
                for (char& c : item) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                auto const it = aliases.find(item);
                if (it != aliases.end()) {
                    wanted_flows.insert(it->second.begin(), it->second.end());
                }
            }
            std::vector<json> filtered;
            for (auto const& inst : instruments) {
                auto const flow = as_int(field(inst, "flow"));
                if (flow && wanted_flows.contains(*flow)) {
                    filtered.push_back(inst);
                }
            }
            instruments = std::move(filtered);
        }

        auto const [kind, value] = parse_identifier(query);
        if (kind == "isin" || kind == "ins_code") {
            auto const key   = kind == "isin" ? "isin" : "ins_code";
            auto       upper = value;
            for (char& c : upper) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            std::vector<json> matches;
            for (auto const& inst : instruments) {
                auto candidate = field_text(inst, key);
                for (char& c : candidate) {
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                }
                if (candidate == upper) {
                    matches.push_back(inst);
                }
            }
            if (matches.empty() && options.allow_live && !options.offline) {
                if (kind == "ins_code") {
                    return {make_candidate(synthetic(value, nullptr, "", ""), 0, 100.0, "exact")};
                }
                json found = nullptr;
                try {
                    found = resolve_isin(http, value);
                } catch (...) {
                    found = nullptr;
                }
                if (!found.is_null()) {
                    auto record = synthetic(
                      field(found, "ins_code"),
                      field(found, "isin"),
                      field_text(found, "symbol"),
                      field_text(found, "name"));
                    record["flow"]   = field(found, "flow");
                    record["market"] = field(found, "market");
                    (void) merge(instruments, {record});
                    (void) write_cache(path, instruments, "merged", as_string(field(meta, "fetched_at")));
                    matches.push_back(record);
                }
            }
            std::stable_sort(matches.begin(), matches.end(), [](json const& a, json const& b) {
                auto const ra = market_rank(field(a, "flow"), field(a, "isin"));
                auto const rb = market_rank(field(b, "flow"), field(b, "isin"));
                if (ra != rb) {
                    return ra < rb;
                }
                return as_double(field(a, "value")).value_or(0.0) > as_double(field(b, "value")).value_or(0.0);
            });
            std::vector<json> out;
            out.reserve(matches.size());
            for (auto const& inst : matches) {
                out.push_back(make_candidate(inst, 0, 100.0, "exact"));
            }
            return out;
        }

        auto const normalized  = normal(value);
        auto const query_latin = latin_only(value);
        auto const reverse     = reverse_persian(query_latin);
        auto       candidates  = score_index(instruments, normalized, query_latin, reverse, options.min_score);
        if (candidates.empty() && options.allow_live && !options.offline) {
            auto const term    = (query_latin && reverse) ? *reverse : normalized;
            auto const records = lookup_live(http, term);
            if (!records.empty()) {
                (void) merge(instruments, records);
                (void) write_cache(path, instruments, "merged", as_string(field(meta, "fetched_at")));
                candidates = score_index(instruments, normalized, query_latin, reverse, options.min_score);
            }
        }
        std::stable_sort(candidates.begin(), candidates.end(), rank_less);
        if (options.limit > 0 && candidates.size() > options.limit) {
            candidates.resize(options.limit);
        }
        return candidates;
    }

    json resolve_one(client&                              http,
                     std::string const&                   query,
                     resolve_options const&               options_in,
                     std::optional<std::filesystem::path> cache_path) {
        auto options      = options_in;
        options.min_score = fuzzy_accept_score;
        auto candidates   = resolve_query(http, query, options, std::move(cache_path));
        if (candidates.empty()) {
            throw not_found(std::format("no instrument matches '{}'", query));
        }
        json best;
        if (choose(candidates, best)) {
            throw ambiguous_error(
              std::format("'{}' is ambiguous; candidates: {}", query, format_candidates(candidates)),
              candidates);
        }
        return field(best, "instrument");
    }

    std::vector<std::pair<std::string, std::string>>
    complete_query(std::string const& prefix, std::vector<json> const& index, std::size_t limit) {
        auto const first = prefix.find_first_not_of(" \t\n\r\f\v");
        if (first == std::string::npos) {
            return {};
        }
        auto const [kind, value] = parse_identifier(prefix);
        auto normalized          = normal(prefix);
        auto query_latin         = latin_only(prefix);
        auto reverse             = reverse_persian(query_latin);

        std::optional<std::string> isin_prefix;
        std::optional<std::string> code_prefix;
        std::optional<std::string> isin_suffix;
        std::optional<std::string> name_part;

        auto upper_of = [](std::string text) {
            for (char& c : text) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            return text;
        };

        if (kind == "isin") {
            isin_prefix = upper_of(value);
        } else if (kind == "ins_code") {
            code_prefix = value;
        } else {
            auto const raw   = prefix.substr(first, prefix.find_last_not_of(" \t\n\r\f\v") - first + 1);
            bool const ascii = std::all_of(raw.begin(), raw.end(), [](char c) {
                return static_cast<unsigned char>(c) < 0x80;
            });
            if (ascii) {
                auto const upper = upper_of(raw);
                if (upper.starts_with("IR") && upper.size() >= 2) {
                    isin_prefix = upper;
                } else if (std::all_of(upper.begin(),
                                       upper.end(),
                                       [](char c) {
                                           return c >= '0' && c <= '9';
                                       }))
                {
                    code_prefix = upper;
                }
            }
            if (!isin_prefix && !code_prefix) {
                static std::regex const suffix_re(R"(^(.*?)[-:@(\s]+(IR[A-Za-z0-9]*)$)", std::regex::icase);
                std::smatch             match;
                if (std::regex_match(raw, match, suffix_re)) {
                    auto       part   = match[1].str();
                    auto const pfirst = part.find_first_not_of(" \t\n\r\f\v");
                    if (pfirst != std::string::npos) {
                        part = part.substr(pfirst, part.find_last_not_of(" \t\n\r\f\v") - pfirst + 1);
                    }
                    name_part   = part;
                    isin_suffix = upper_of(match[2].str());
                    normalized  = normal(part);
                    query_latin = latin_only(part);
                    reverse     = reverse_persian(query_latin);
                }
            }
        }

        struct row {
            int         score;
            double      market_rank;
            double      value;
            std::string token;
            std::string description;
        };

        std::vector<row>                rows;
        std::unordered_set<std::string> seen;

        for (auto const& instrument : index) {
            auto const symbol = field_text(instrument, "symbol");
            auto const name   = field_text(instrument, "name");
            auto       isin   = upper_of(field_text(instrument, "isin"));
            auto const code   = field_text(instrument, "ins_code");
            auto const keys   = query_latin ? [&] {
                auto all = string_list(field(instrument, "latin"));
                for (auto const& key : string_list(field(instrument, "name_latin"))) {
                    all.push_back(key);
                }
                return all;
            }()
                                            : std::vector<std::string>{};

            auto any_starts = [&keys](std::string const& q) {
                return std::any_of(keys.begin(), keys.end(), [&q](std::string const& key) {
                    return key.starts_with(q);
                });
            };
            auto any_contains = [&keys](std::string const& q) {
                return std::any_of(keys.begin(), keys.end(), [&q](std::string const& key) {
                    return key.find(q) != std::string::npos;
                });
            };

            int score = -1;
            if (isin_suffix) {
                if (!isin.starts_with(*isin_suffix)) {
                    continue;
                }
                bool const name_hit =
                  (!normalized.empty() && (symbol.starts_with(normalized) || name.starts_with(normalized))) ||
                  (query_latin && any_starts(*query_latin));
                score = name_hit ? 0 : 1;
            } else if (
              (isin_prefix && isin.starts_with(*isin_prefix)) || (code_prefix && code.starts_with(*code_prefix)))
            {
                score = 0;
            } else if (!normalized.empty() && symbol.starts_with(normalized)) {
                score = 1;
            } else if ((query_latin && any_starts(*query_latin)) || (reverse && symbol.starts_with(*reverse))) {
                score = 2;
            } else if (!normalized.empty() && name.starts_with(normalized)) {
                score = 3;
            } else if (!normalized.empty() && symbol.find(normalized) != std::string::npos) {
                score = 4;
            } else if (
              (query_latin && any_contains(*query_latin)) || (reverse && symbol.find(*reverse) != std::string::npos))
            {
                score = 5;
            } else if (!normalized.empty() && name.find(normalized) != std::string::npos) {
                score = 6;
            } else if (query_latin && query_latin->size() >= finglish_query_min) {
                double best = 0.0;
                for (auto const& key : keys) {
                    best = std::max(best, ratio(*query_latin, key));
                }
                if (best >= finglish_key_score) {
                    score = 7;
                }
            }
            if (score < 0) {
                continue;
            }

            std::string token;
            if (isin_suffix) {
                auto const suffix = !isin.empty() ? isin : code;
                token             = name_part ? *name_part + "-" + suffix : suffix;
            } else if (query_latin && !isin_prefix && !code_prefix) {
                token = completion_token(instrument, query_latin);
            } else {
                token = token_for(instrument);
            }
            if (token.empty() || !seen.insert(token).second) {
                continue;
            }
            rows.push_back(row{score,
                               market_rank(field(instrument, "flow"), field(instrument, "isin")),
                               -as_double(field(instrument, "value")).value_or(0.0),
                               token,
                               description_for(instrument)});
        }

        std::stable_sort(rows.begin(), rows.end(), [](row const& a, row const& b) {
            if (a.score != b.score) {
                return a.score < b.score;
            }
            if (a.market_rank != b.market_rank) {
                return a.market_rank < b.market_rank;
            }
            if (a.value != b.value) {
                return a.value < b.value;
            }
            if (a.token != b.token) {
                return a.token < b.token;
            }
            return a.description < b.description;
        });

        std::vector<std::pair<std::string, std::string>> out;
        for (std::size_t i = 0; i < rows.size() && i < limit; ++i) {
            out.emplace_back(rows[i].token, rows[i].description);
        }
        return out;
    }

    std::string token_for(json const& instrument) {
        auto const symbol = field_text(instrument, "symbol");
        auto       suffix = field_text(instrument, "isin");
        if (suffix.empty()) {
            suffix = field_text(instrument, "ins_code");
        }
        if (!symbol.empty() && !suffix.empty()) {
            return symbol + "-" + suffix;
        }
        return !symbol.empty() ? symbol : suffix;
    }

    std::string description_for(json const& instrument) {
        auto symbol = field_text(instrument, "symbol");
        if (symbol.empty()) {
            symbol = field_text(instrument, "ins_code");
        }
        if (symbol.empty()) {
            symbol = "?";
        }
        std::string out  = symbol;
        auto const  name = field_text(instrument, "name");
        if (!name.empty() && name != symbol) {
            out += " — " + name;
        }
        auto where = field_text(instrument, "board");
        if (where.empty()) {
            where = field_text(instrument, "market");
        }
        std::vector<std::string> tail;
        if (!where.empty()) {
            tail.push_back(where);
        }
        auto const code = field_text(instrument, "ins_code");
        if (!code.empty()) {
            tail.push_back("insCode " + code);
        }
        if (!tail.empty()) {
            std::string joined;
            for (std::size_t i = 0; i < tail.size(); ++i) {
                if (i != 0) {
                    joined += "; ";
                }
                joined += tail[i];
            }
            out += " (" + joined + ")";
        }
        return out;
    }

    json public_instrument(json const& instrument) {
        json out = json::object();
        for (auto it = instrument.begin(); it != instrument.end(); ++it) {
            if (it.key() == "latin" || it.key() == "name_latin") {
                continue;
            }
            out[it.key()] = it.value();
        }
        return out;
    }

    std::string format_candidates(std::vector<json> const& candidates) {
        std::string out;
        for (std::size_t i = 0; i < candidates.size() && i < 10; ++i) {
            auto const instrument = field(candidates[i], "instrument");
            auto       label      = field_text(instrument, "symbol");
            if (label.empty()) {
                label = field_text(instrument, "name");
            }
            if (label.empty()) {
                label = "?";
            }
            auto where = field_text(instrument, "board");
            if (where.empty()) {
                where = field_text(instrument, "market");
            }
            auto suffix = field_text(instrument, "ins_code");
            if (!where.empty()) {
                if (suffix.empty()) {
                    suffix = where;
                } else {
                    suffix += ' ';
                    suffix += where;
                }
            }
            if (i != 0) {
                out += ", ";
            }
            out += label;
            out += " (";
            out += suffix;
            out += ')';
        }
        return out.empty() ? "none" : out;
    }

    bool choose(std::vector<json> const& candidates, json& best) {
        if (candidates.empty()) {
            return true;
        }
        auto const&       top = candidates.front();
        std::vector<json> contenders{top};
        for (std::size_t i = 1; i < candidates.size(); ++i) {
            auto const& candidate = candidates[i];
            if (as_int(field(candidate, "tier")).value_or(0) != as_int(field(top, "tier")).value_or(0) ||
                as_double(field(top, "score")).value_or(0.0) - as_double(field(candidate, "score")).value_or(0.0) >
                  ambiguity_margin)
            {
                break;
            }
            contenders.push_back(candidate);
        }
        double best_rank = as_double(field(contenders.front(), "market_rank")).value_or(0.0);
        for (auto const& candidate : contenders) {
            best_rank = std::min(best_rank, as_double(field(candidate, "market_rank")).value_or(0.0));
        }
        std::vector<json> winners;
        for (auto const& candidate : contenders) {
            if (as_double(field(candidate, "market_rank")).value_or(0.0) == best_rank) {
                winners.push_back(candidate);
            }
        }
        if (winners.size() == 1) {
            best = winners.front();
            return false;
        }
        std::vector<json> traded;
        for (auto const& candidate : winners) {
            auto const instrument = field(candidate, "instrument");
            if (field(instrument, "traded").is_boolean() && field(instrument, "traded").get<bool>()) {
                traded.push_back(candidate);
            }
        }
        if (traded.size() == 1) {
            best = traded.front();
            return false;
        }
        std::stable_sort(winners.begin(), winners.end(), [](json const& a, json const& b) {
            auto const ai = field(a, "instrument");
            auto const bi = field(b, "instrument");
            auto const va = as_double(field(ai, "value")).value_or(0.0);
            auto const vb = as_double(field(bi, "value")).value_or(0.0);
            if (va != vb) {
                return va > vb;
            }
            return as_double(field(ai, "mcap")).value_or(0.0) > as_double(field(bi, "mcap")).value_or(0.0);
        });
        best = winners.front();
        return true;
    }

} // namespace dana::tools::tse
