#include "tse_find.hpp"

#include "tse_common.hpp"
#include "tse_resolver.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dana::tools::tse {

    namespace {

        using json = dana::tools::tse::json;

        // ------------------------------------------------------------------ errors

        struct [[nodiscard]] arg_error : error {
            using error::error;
        };

        // ------------------------------------------------------------------ tables

        [[nodiscard]] std::unordered_map<std::string, std::string> const& sector_codes() {
            static std::unordered_map<std::string, std::string> const table{
              {"01",                     "زراعت و خدمات وابسته"},
              {"10",                         "استخراج زغال سنگ"},
              {"11",   "استخراج نفت گاز و خدمات جنبي جز اکتشاف"},
              {"13",                    "استخراج کانه هاي فلزي"},
              {"14",                       "استخراج ساير معادن"},
              {"17",                                  "منسوجات"},
              {"19",     "دباغي، پرداخت چرم و ساخت انواع پاپوش"},
              {"20",                             "محصولات چوبي"},
              {"21",                            "محصولات كاغذي"},
              {"22",                      "انتشار، چاپ و تکثير"},
              {"23",      "فراورده هاي نفتي، كك و سوخت هسته اي"},
              {"25",                         "لاستيك و پلاستيك"},
              {"26", "توليد محصولات كامپيوتري الكترونيكي ونوري"},
              {"27",                              "فلزات اساسي"},
              {"28",                        "ساخت محصولات فلزي"},
              {"29",                     "ماشين آلات و تجهيزات"},
              {"31",             "ماشين آلات و دستگاه هاي برقي"},
              {"32",           "ساخت دستگاه ها و وسايل ارتباطي"},
              {"34",                       "خودرو و ساخت قطعات"},
              {"35",                   "ساير تجهيزات حمل و نقل"},
              {"38",                                "قند و شكر"},
              {"39",                "شرکتهاي چند رشته اي صنعتي"},
              {"40",               "عرضه برق، گاز، بخاروآب گرم"},
              {"42", "محصولات غذايي و آشاميدني به جز قند و شكر"},
              {"43",                    "مواد و محصولات دارويي"},
              {"44",                          "محصولات شيميايي"},
              {"45",                          "پيمانكاري صنعتي"},
              {"46", "تجارت عمده فروشي به جز وسايل نقليه موتور"},
              {"47",   "خرده فروشي،باستثناي وسايل نقليه موتوري"},
              {"49",                            "كاشي و سراميك"},
              {"50", "تجارت عمده وخرده فروشي وسائط نقليه موتور"},
              {"53",                          "سيمان، آهك و گچ"},
              {"54",                "ساير محصولات كاني غيرفلزي"},
              {"55",                            "هتل و رستوران"},
              {"56",                           "سرمايه گذاريها"},
              {"57",                  "بانكها و موسسات اعتباري"},
              {"58",                   "ساير واسطه گريهاي مالي"},
              {"59",    "اوراق حق تقدم استفاده از تسهيلات مسكن"},
              {"60",           "حمل ونقل، انبارداري و ارتباطات"},
              {"61",                            "حمل و نقل آبي"},
              {"64",                                  "مخابرات"},
              {"66", "بيمه وصندوق بازنشستگي به جزتامين اجتماعي"},
              {"67",      "فعاليتهاي كمكي به نهادهاي مالي واسط"},
              {"68",           "صندوق سرمايه گذاري قابل معامله"},
              {"69",                         "اوراق تامين مالي"},
              {"70",              "انبوه سازي، املاك و مستغلات"},
              {"71", "فعاليت مهندسي، تجزيه، تحليل و آزمايش فني"},
              {"72",         "رايانه و فعاليت هاي وابسته به آن"},
              {"73",                       "اطلاعات و ارتباطات"},
              {"74",                       "خدمات فني و مهندسي"},
              {"84",            "سلامت انسان و مددكاري اجتماعي"},
              {"90",        "فعاليت هاي هنري، سرگرمي و خلاقانه"},
              {"93",                 "فعاليتهاي فرهنگي و ورزشي"},
            };
            return table;
        }

        [[nodiscard]] std::unordered_map<std::string, std::string> const& board_codes() {
            static std::unordered_map<std::string, std::string> const table{
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
              {"Z1",                   "بازار دوم فرابورس"},
              {"13",                 "بازار ابزارهای مشձه"},
              {"20",                "بازار ابزارهای مشتقه"},
            };
            return table;
        }

        [[nodiscard]] std::map<long long, std::string> const& flow_names() {
            static std::map<long long, std::string> const table{
              {1,    "بورس"},
              {2, "فرابورس"},
              {3,   "مشتقه"},
              {4,    "پایه"},
              {6, "فرابورس"},
              {7, "فرابورس"},
            };
            return table;
        }

        [[nodiscard]] std::unordered_map<std::string, std::vector<long long>> const& flow_aliases() {
            static std::unordered_map<std::string, std::vector<long long>> const table{
              {     "borse",       {1}},
              {    "bourse",       {1}},
              {       "tse",       {1}},
              {         "1",       {1}},
              {   "frabors", {2, 6, 7}},
              {"farabourse", {2, 6, 7}},
              {       "ifb", {2, 6, 7}},
              {         "2",       {2}},
              { "moshtaghe",       {3}},
              {"derivative",       {3}},
              {         "3",       {3}},
              {      "paye",       {4}},
              {      "base",       {4}},
              {         "4",       {4}},
              {         "6",       {6}},
              {         "7",       {7}},
            };
            return table;
        }

        [[nodiscard]] std::unordered_map<long long, std::string> const& kind_types() {
            static std::unordered_map<long long, std::string> const table{
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
              {803,    "other"},
            };
            return table;
        }

        [[nodiscard]] std::unordered_map<std::string, std::string> const& type_aliases() {
            static std::unordered_map<std::string, std::string> const table{
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
              {  "اختيار",   "option"},
              {    "bond",     "bond"},
              {   "bonds",     "bond"},
              {    "اصاق",     "bond"},
              {   "right",    "right"},
              {  "rights",    "right"},
              {      "حق",    "right"},
              {  "future",   "future"},
              { "futures",   "future"},
              {     "آتي",   "future"},
              {"mortgage", "mortgage"},
              {     "تسه", "mortgage"},
              { "forward",  "forward"},
              {     "سلف",  "forward"},
              {   "other",    "other"},
              {     "all",      "all"},
              {     "همه",      "all"},
            };
            return table;
        }

        struct [[nodiscard]] column_info {
            std::string_view title;
            std::string_view kind;
        };

        [[nodiscard]] std::unordered_map<std::string_view, column_info> const& columns_table() {
            static std::unordered_map<std::string_view, column_info> const table{
              {    "symbol",  {"Symbol", "text"}},
              {      "name",    {"Name", "text"}},
              {      "type",    {"Type", "text"}},
              {    "market",  {"Market", "text"}},
              {     "board",   {"Board", "text"}},
              {    "sector",  {"Sector", "text"}},
              {      "code", {"insCode", "text"}},
              {      "isin",    {"ISIN", "text"}},
              {     "state",   {"State", "text"}},
              {      "macd",    {"MACD", "text"}},
              {       "chg",     {"Chg%", "pct"}},
              {   "to_ceil",  {"ToCeil%", "pct"}},
              {  "to_floor", {"ToFloor%", "pct"}},
              {"spread_pct",     {"Spr%", "pct"}},
              {      "y52h",   {"To52H%", "pct"}},
              {      "y52l",   {"To52L%", "pct"}},
              {     "price",    {"Price", "int"}},
              {      "last",     {"Last", "int"}},
              {      "open",     {"Open", "int"}},
              {       "low",       {"Lo", "int"}},
              {      "high",       {"Hi", "int"}},
              {       "eps",      {"EPS", "int"}},
              {    "spread",   {"Spread", "int"}},
              {    "trades",   {"Trades", "int"}},
              {        "pe",    {"P/E", "float"}},
              { "sector_pe", {"SecP/E", "float"}},
              {       "psr",    {"P/S", "float"}},
              {       "rsi",    {"RSI", "float"}},
              {     "sma20",    {"SMA20", "int"}},
              {     "sma50",    {"SMA50", "int"}},
              {    "sma200",   {"SMA200", "int"}},
              {    "streak",     {"Strk", "int"}},
              {     "surge",  {"Surge", "float"}},
              { "imbalance",  {"Imbal", "float"}},
              {       "vol",   {"Volume", "amt"}},
              {     "value",    {"Value", "amt"}},
              {      "mcap",   {"MktCap", "amt"}},
              {     "avg5d",    {"Avg5d", "amt"}},
              {  "bid_wall",  {"BidWall", "amt"}},
              {  "ask_wall",  {"AskWall", "amt"}},
              {"free_float",      {"FF%", "pct"}},
            };
            return table;
        }

        [[nodiscard]] std::vector<std::string> const& default_columns() {
            static std::vector<std::string> const
              columns{"symbol", "type", "market", "chg", "price", "pe", "vol", "value", "mcap", "sector"};
            return columns;
        }

        [[nodiscard]] std::vector<std::string> const& sorted_column_keys() {
            static std::vector<std::string> const keys = [] {
                std::vector<std::string> out;
                out.reserve(columns_table().size());
                for (auto const& [key, info] : columns_table()) {
                    (void) info;
                    out.emplace_back(key);
                }
                std::sort(out.begin(), out.end());
                return out;
            }();
            return keys;
        }

        [[nodiscard]] std::unordered_set<std::string_view> const& info_keys() {
            static std::unordered_set<std::string_view> const
              keys{"psr", "sector_pe", "avg5d", "y52h", "y52l", "free_float"};
            return keys;
        }

        [[nodiscard]] std::unordered_set<std::string_view> const& hist_keys() {
            static std::unordered_set<std::string_view> const
              keys{"rsi", "sma20", "sma50", "sma200", "macd", "streak", "surge"};
            return keys;
        }

        [[nodiscard]] std::unordered_set<std::string_view> const& book_keys() {
            static std::unordered_set<std::string_view> const keys{"bid_wall", "ask_wall", "imbalance"};
            return keys;
        }

        // ------------------------------------------------------------------ text helpers

        [[nodiscard]] bool is_space(char const c) noexcept {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
        }

        [[nodiscard]] std::string_view trim_view(std::string_view text) noexcept {
            while (!text.empty() && is_space(text.front())) {
                text.remove_prefix(1);
            }
            while (!text.empty() && is_space(text.back())) {
                text.remove_suffix(1);
            }
            return text;
        }

        [[nodiscard]] std::string trim(std::string_view text) {
            return std::string(trim_view(text));
        }

        [[nodiscard]] std::string lstrip(std::string_view text) {
            while (!text.empty() && is_space(text.front())) {
                text.remove_prefix(1);
            }
            return std::string(text);
        }

        [[nodiscard]] std::string lower_ascii(std::string_view text) {
            std::string out(text);
            for (char& c : out) {
                if (c >= 'A' && c <= 'Z') {
                    c = static_cast<char>(c - 'A' + 'a');
                }
            }
            return out;
        }

        [[nodiscard]] std::string upper_ascii(std::string_view text) {
            std::string out(text);
            for (char& c : out) {
                if (c >= 'a' && c <= 'z') {
                    c = static_cast<char>(c - 'a' + 'A');
                }
            }
            return out;
        }

        [[nodiscard]] bool contains(std::string_view haystack, std::string_view needle) noexcept {
            return haystack.find(needle) != std::string_view::npos;
        }

        [[nodiscard]] std::string join(std::vector<std::string> const& items, std::string_view separator) {
            std::string out;
            for (std::size_t i = 0; i < items.size(); ++i) {
                if (i != 0) {
                    out += separator;
                }
                out += items[i];
            }
            return out;
        }

        [[nodiscard]] std::vector<std::string> split_on_any(std::string_view text, std::string_view separators) {
            std::vector<std::string> out;
            std::size_t              index = 0;
            while (index < text.size()) {
                while (index < text.size() && separators.find(text[index]) != std::string_view::npos) {
                    ++index;
                }
                auto const start = index;
                while (index < text.size() && separators.find(text[index]) == std::string_view::npos) {
                    ++index;
                }
                if (index > start) {
                    out.emplace_back(text.substr(start, index - start));
                }
            }
            return out;
        }

        [[nodiscard]] std::vector<std::string> split_commas(std::string_view value) {
            std::vector<std::string> items;
            std::size_t              start = 0;
            while (start <= value.size()) {
                auto const comma = value.find(',', start);
                auto const end   = comma == std::string_view::npos ? value.size() : comma;
                auto       piece = trim(value.substr(start, end - start));
                if (!piece.empty()) {
                    items.push_back(std::move(piece));
                }
                if (comma == std::string_view::npos) {
                    break;
                }
                start = comma + 1;
            }
            return items;
        }

        [[nodiscard]] std::vector<std::string> split_list(std::vector<std::string> const& values) {
            std::vector<std::string> items;
            for (auto const& value : values) {
                auto parts = split_commas(value);
                items.insert(items.end(), std::make_move_iterator(parts.begin()), std::make_move_iterator(parts.end()));
            }
            return items;
        }

        [[nodiscard]] bool all_digits(std::string_view text) noexcept {
            if (text.empty()) {
                return false;
            }
            return std::all_of(text.begin(), text.end(), [](char const c) {
                return c >= '0' && c <= '9';
            });
        }

        // ------------------------------------------------------------------ json helpers

        [[nodiscard]] double num(json const& value) noexcept {
            return as_double(value).value_or(0.0);
        }

        [[nodiscard]] long long as_ll(json const& value) noexcept {
            return as_int(value).value_or(0);
        }

        [[nodiscard]] bool truthy(json const& value) {
            if (value.is_null()) {
                return false;
            }
            if (value.is_boolean()) {
                return value.get<bool>();
            }
            if (value.is_string()) {
                return !value.get_ref<std::string const&>().empty();
            }
            if (value.is_array()) {
                return !value.empty();
            }
            if (value.is_object()) {
                return true;
            }
            if (value.is_number()) {
                return num(value) != 0.0;
            }
            return false;
        }

        [[nodiscard]] std::string text_of(json const& value) {
            return scalar_text(value);
        }

        [[nodiscard]] std::string normal_of(json const& value) {
            return normal(scalar_text(value));
        }

        [[nodiscard]] json field_of(json const& row, std::string_view key) {
            if (!row.is_object()) {
                return nullptr;
            }
            auto const it = row.find(std::string(key));
            return it == row.end() ? json(nullptr) : *it;
        }

        [[nodiscard]] json sub(json const& a, json const& b) {
            if (a.is_number_integer() && b.is_number_integer()) {
                return json(a.get<long long>() - b.get<long long>());
            }
            return json(num(a) - num(b));
        }

        [[nodiscard]] json mul(json const& a, json const& b) {
            if (a.is_number_integer() && b.is_number_integer()) {
                return json(a.get<long long>() * b.get<long long>());
            }
            return json(num(a) * num(b));
        }

        // Truncate a JSON number toward zero (an integer cast).
        [[nodiscard]] json truncate(json const& value) {
            if (value.is_null()) {
                return nullptr;
            }
            return json(as_ll(value));
        }

        // ------------------------------------------------------------------ number parsing

        [[nodiscard]] bool parse_double_text(std::string_view text, double& out) noexcept {
            auto const trimmed = trim_view(text);
            if (trimmed.empty()) {
                return false;
            }
            bool negative = false;
            auto rest     = trimmed;
            if (rest.front() == '+' || rest.front() == '-') {
                negative = rest.front() == '-';
                rest.remove_prefix(1);
            }
            if (rest.empty()) {
                return false;
            }
            double     parsed = 0.0;
            auto const result = std::from_chars(rest.data(), rest.data() + rest.size(), parsed);
            if (result.ec != std::errc{} || result.ptr != rest.data() + rest.size()) {
                return false;
            }
            out = negative ? -parsed : parsed;
            return true;
        }

        [[nodiscard]] bool parse_int_text(std::string_view text, long long& out) noexcept {
            auto const trimmed = trim_view(text);
            if (trimmed.empty()) {
                return false;
            }
            bool negative = false;
            auto rest     = trimmed;
            if (rest.front() == '+' || rest.front() == '-') {
                negative = rest.front() == '-';
                rest.remove_prefix(1);
            }
            if (rest.empty()) {
                return false;
            }
            long long  parsed = 0;
            auto const result = std::from_chars(rest.data(), rest.data() + rest.size(), parsed);
            if (result.ec != std::errc{} || result.ptr != rest.data() + rest.size()) {
                return false;
            }
            out = negative ? -parsed : parsed;
            return true;
        }

        // A number with an optional k/m/b/t/p suffix (rial).
        [[nodiscard]] double parse_amount(std::string const& text) {
            auto const  trimmed = trim_view(text);
            std::size_t pos     = 0;
            if (pos < trimmed.size() && (trimmed[pos] == '+' || trimmed[pos] == '-')) {
                ++pos;
            }
            bool const negative = pos == 1 && trimmed[0] == '-';
            auto const start    = pos;
            while (pos < trimmed.size() &&
                   ((trimmed[pos] >= '0' && trimmed[pos] <= '9') || trimmed[pos] == '.' || trimmed[pos] == ','))
            {
                ++pos;
            }
            if (pos == start) {
                throw error(std::format("not a number: '{}'", text));
            }
            std::string digits;
            for (std::size_t i = start; i < pos; ++i) {
                if (trimmed[i] != ',') {
                    digits.push_back(trimmed[i]);
                }
            }
            double factor = 1.0;
            if (pos < trimmed.size()) {
                char const suffix = static_cast<char>(static_cast<unsigned char>(trimmed[pos]) | 0x20U);
                switch (suffix) {
                    case 'k': factor = 1e3; break;
                    case 'm': factor = 1e6; break;
                    case 'b': factor = 1e9; break;
                    case 't': factor = 1e12; break;
                    case 'p': factor = 1e15; break;
                    default: throw error(std::format("not a number: '{}'", text));
                }
                ++pos;
            }
            if (pos != trimmed.size()) {
                throw error(std::format("not a number: '{}'", text));
            }
            double     number = 0.0;
            auto const result = std::from_chars(digits.data(), digits.data() + digits.size(), number);
            if (result.ec != std::errc{} || result.ptr != digits.data() + digits.size()) {
                throw error(std::format("not a number: '{}'", text));
            }
            double const scaled = (negative ? -number : number) * factor;
            return scaled;
        }

        [[nodiscard]] double parse_ratio(std::string const& text) {
            std::string cleaned;
            cleaned.reserve(text.size());
            for (char const c : text) {
                if (c != ',') {
                    cleaned.push_back(c);
                }
            }
            double value = 0.0;
            if (!parse_double_text(cleaned, value)) {
                throw error(std::format("not a number: '{}'", text));
            }
            return value;
        }

        // ------------------------------------------------------------------ records

        [[nodiscard]] std::string guess_type(std::string_view isin, std::string_view name) {
            auto const prefix = std::string(isin.substr(0, std::min<std::size_t>(4, isin.size())));
            auto const starts = [isin](std::string_view needle) {
                return isin.starts_with(needle);
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
            if (prefix == "IRT1" || prefix == "IRT3" || prefix == "IRTK" || prefix == "IRTE") {
                return "fund";
            }
            if (prefix.starts_with("IRR")) {
                return "right";
            }
            if (starts("IRBK") || starts("IRBE") || prefix == "IRK1") {
                return "forward";
            }
            if (prefix == "IRB3") {
                return "bond";
            }
            auto const text = normal(name);
            if (contains(text, "اختيار") || contains(text, "اختیار")) {
                return "option";
            }
            if (contains(text, "صندوق")) {
                return "fund";
            }
            if (contains(text, "سلف")) {
                return "forward";
            }
            if (contains(text, "مرابحه") || contains(text, "مشاركت") || contains(text, "صكوك") ||
                contains(text, "اراد") || contains(text, "گام"))
            {
                return "bond";
            }
            if (contains(text, "آتي") || contains(text, "آتی")) {
                return "future";
            }
            if (contains(text, "تسه") || contains(text, "امتياز") || contains(text, "امتیاز")) {
                return "mortgage";
            }
            // `^ح\s*\.|^ح\s`: a leading ح followed by a dot or whitespace.
            if (text.starts_with("\xD8\xAD") && text.size() > 2) {
                char const next = text[2];
                if (next == '.' || is_space(next)) {
                    return "right";
                }
            }
            return "other";
        }

        struct [[nodiscard]] record {
            json data;
            json levels;
        };

        [[nodiscard]] record parse_row(json const& row) {
            auto const isin   = trim(text_of(pick(row, {"insID"})));
            auto const symbol = normal(text_of(pick(row, {"lva"})));
            auto const name   = normal(text_of(pick(row, {"lvc"})));
            auto       kind   = number(pick(row, {"yVal"}));
            auto       flow   = number(pick(row, {"flow"}));
            auto       close  = price(pick(row, {"pcl"}));
            if (close.is_null()) {
                close = price(pick(row, {"pdv"}));
            }
            auto const last   = price(pick(row, {"pdv"}));
            auto const prev   = price(pick(row, {"py"}));
            auto       change = number(pick(row, {"pcpc"}));
            if (change.is_null() && truthy(close) && truthy(prev)) {
                change = sub(close, prev);
            }
            auto const ceiling     = price(pick(row, {"pMax"}));
            auto const floor       = number(pick(row, {"pMin"}));
            auto const bid         = price(pick(row, {"pmd"}));
            auto const ask         = price(pick(row, {"pmo"}));
            auto const sector_code = trim(text_of(pick(row, {"csv"})));
            auto const board_code  = trim(text_of(pick(row, {"cGrValCot"})));
            auto const shares      = number(pick(row, {"ztd"}));
            auto const volume      = number(pick(row, {"qtj"}));
            auto const value       = number(pick(row, {"qtc"}));

            json to_ceil  = nullptr;
            json to_floor = nullptr;
            if (truthy(ceiling) && truthy(close)) {
                to_ceil = json(round_to((num(ceiling) / num(close) - 1.0) * 100.0, 2));
            }
            if (truthy(floor) && truthy(close)) {
                to_floor = json(round_to((1.0 - num(floor) / num(close)) * 100.0, 2));
            }
            std::vector<double> gaps;
            if (!to_ceil.is_null()) {
                gaps.push_back(num(to_ceil));
            }
            if (!to_floor.is_null()) {
                gaps.push_back(num(to_floor));
            }
            json near_limit = nullptr;
            if (!gaps.empty()) {
                near_limit = json(*std::min_element(gaps.begin(), gaps.end()));
            }

            std::string kind_type;
            if (!kind.is_null()) {
                auto const it = kind_types().find(as_ll(kind));
                if (it != kind_types().end()) {
                    kind_type = it->second;
                }
            }
            std::string flow_name;
            if (!flow.is_null()) {
                auto const it = flow_names().find(as_ll(flow));
                flow_name     = it != flow_names().end() ? it->second : std::format("flow{}", as_ll(flow));
            }

            json board = nullptr;
            if (auto const it = board_codes().find(board_code); it != board_codes().end() && !it->second.empty()) {
                board = it->second;
            } else if (!flow_name.empty()) {
                board = flow_name;
            } else if (!board_code.empty()) {
                board = board_code;
            }

            json sector = nullptr;
            if (auto const it = sector_codes().find(sector_code); it != sector_codes().end()) {
                sector = it->second;
            }

            json eps = number(pick(row, {"eps"}));
            if (!truthy(eps)) {
                eps = nullptr;
            }

            json data           = json::object();
            data["code"]        = text_of(pick(row, {"insCode"}));
            data["symbol"]      = symbol;
            data["name"]        = name;
            data["isin"]        = isin;
            data["kind"]        = truncate(kind);
            data["type"]        = kind_type.empty() ? guess_type(isin, name) : kind_type;
            data["flow"]        = truncate(flow);
            data["market"]      = flow.is_null() ? json(nullptr) : json(flow_name);
            data["board_code"]  = board_code;
            data["board"]       = std::move(board);
            data["sector_code"] = sector_code;
            data["sector"]      = std::move(sector);
            data["state"]       = pick(row, {"yMarNSC"});
            data["price"]       = close;
            data["last"]        = last;
            data["prev"]        = prev;
            data["open"]        = price(pick(row, {"pf"}));
            data["low"]         = price(pick(row, {"pmn"}));
            data["high"]        = price(pick(row, {"pmx"}));
            data["chg"]    = (truthy(close) && truthy(prev)) ? json(round_to((num(close) / num(prev) - 1.0) * 100.0, 2))
                                                             : json(nullptr);
            data["change"] = change;
            data["ceiling"]     = ceiling;
            data["floor"]       = floor;
            data["to_ceil"]     = to_ceil;
            data["to_floor"]    = to_floor;
            data["_near_limit"] = near_limit;
            data["bid"]         = bid;
            data["ask"]         = ask;
            data["spread"]      = (truthy(bid) && truthy(ask)) ? sub(ask, bid) : json(nullptr);
            data["spread_pct"] =
              (truthy(bid) && truthy(ask)) ? json(round_to((num(ask) / num(bid) - 1.0) * 100.0, 2)) : json(nullptr);
            data["vol"]    = volume;
            data["value"]  = value;
            data["trades"] = number(pick(row, {"ztt"}));
            data["eps"]    = eps;
            data["pe"]     = number(pick(row, {"pe"}));
            data["shares"] = shares;
            data["mcap"]   = (truthy(shares) && truthy(close)) ? mul(shares, close) : json(nullptr);
            data["traded"] = truthy(value) && num(value) > 0.0;

            auto levels = pick(row, {"blDs"});
            if (!levels.is_array()) {
                levels = json::array();
            }
            return record{std::move(data), std::move(levels)};
        }

        // ------------------------------------------------------------------ filters

        using predicate = std::function<bool(json const&)>;
        using filter    = std::pair<std::string, predicate>;

        void apply_filters(std::vector<record>&       records,
                           std::vector<filter> const& filters,
                           std::string_view           label,
                           std::vector<std::string>&  notes) {
            if (filters.empty()) {
                return;
            }
            auto const before = records.size();
            for (auto const& [name, test] : filters) {
                std::vector<record> kept;
                kept.reserve(records.size());
                try {
                    for (auto& item : records) {
                        if (test(item.data)) {
                            kept.push_back(std::move(item));
                        }
                    }
                } catch (std::exception const& exc) {
                    throw error(std::format("filter {} failed: {}", name, exc.what()));
                }
                records = std::move(kept);
            }
            if (records.size() != before) {
                notes.push_back(std::format("{}: {} filtered out", label, before - records.size()));
            }
        }

        // ------------------------------------------------------------------ options

        struct [[nodiscard]] options {
            std::vector<std::string>   type;
            std::vector<std::string>   market;
            std::vector<std::string>   board;
            std::vector<std::string>   sector;
            std::vector<std::string>   match;
            std::optional<std::string> watchlist;
            bool                       traded   = false;
            bool                       untraded = false;
            std::optional<std::string> states;

            std::optional<double> price_min;
            std::optional<double> price_max;
            std::optional<double> chg_min;
            std::optional<double> chg_max;
            bool                  up         = false;
            bool                  down       = false;
            bool                  at_ceiling = false;
            bool                  at_floor   = false;
            std::optional<double> near_limit;

            std::optional<double> pe_min;
            std::optional<double> pe_max;
            std::optional<double> eps_min;
            std::optional<double> eps_max;
            bool                  profitable  = false;
            bool                  loss_making = false;
            std::optional<double> mcap_min;
            std::optional<double> mcap_max;
            std::optional<double> ps_min;
            std::optional<double> ps_max;
            std::optional<double> sector_pe_min;
            std::optional<double> sector_pe_max;
            std::optional<double> near_52w_high;
            std::optional<double> near_52w_low;

            std::optional<double> vol_min;
            std::optional<double> vol_max;
            std::optional<double> val_min;
            std::optional<double> val_max;
            std::optional<double> trades_min;
            std::optional<double> trades_max;
            std::optional<double> spread_max;
            std::optional<double> spread_pct_max;
            std::optional<double> avg5d_min;
            std::optional<double> free_float_min;
            std::optional<double> vol_surge;

            bool                                     book = false;
            std::optional<double>                    bid_wall_min;
            std::optional<double>                    ask_wall_min;
            std::optional<std::pair<double, double>> imbalance;

            std::optional<double>      rsi_min;
            std::optional<double>      rsi_max;
            std::optional<long long>   above_sma;
            std::optional<long long>   below_sma;
            std::optional<std::string> cross;
            std::optional<std::string> cross_below;
            std::optional<std::string> macd;
            std::optional<long long>   streak_min;
            std::optional<long long>   streak_max;

            bool      info         = false;
            long long history_days = 260;
            long long max_enrich   = 300;
            long long jobs         = 8;

            std::string                format = "table";
            std::string                sort   = "value";
            bool                       asc    = false;
            std::optional<long long>   limit;
            std::optional<std::string> columns;
            bool                       stats = false;
            bool                       quiet = false;
            bool                       help  = false;
        };

        [[nodiscard]] std::string py_number_text(double value) {
            if (value == std::trunc(value) && std::abs(value) < 1e15) {
                return std::format("{:.0f}", value);
            }
            return std::format("{:g}", value);
        }

        [[nodiscard]] std::unordered_set<std::string> watchlist_filter(std::string const& path) {
            std::ifstream input(path, std::ios::binary);
            if (!input) {
                throw error(std::format("[Errno 2] No such file or directory: '{}'", path));
            }
            client                          http;
            std::unordered_set<std::string> symbols;
            std::string                     line;
            while (std::getline(input, line)) {
                auto const head   = line.substr(0, line.find('#'));
                auto const tokens = split_on_any(head, " \t\n\r\f\v");
                if (tokens.empty()) {
                    continue;
                }
                auto const token = normalize_input(tokens.front());
                symbols.insert(upper_ascii(normal(token)));
                symbols.insert(upper_ascii(token));

                std::vector<json> candidates;
                try {
                    resolve_options settings;
                    settings.offline    = true;
                    settings.allow_live = false;
                    settings.min_score  = 95.0;
                    auto list           = resolve_query(http, token, settings);
                    if (!list.empty()) {
                        json best;
                        if (!choose(list, best)) {
                            candidates.push_back(std::move(best));
                        } else {
                            candidates = std::move(list);
                        }
                    }
                } catch (...) {
                    candidates.clear();
                }
                for (auto const& candidate : candidates) {
                    auto const instrument = field_of(candidate, "instrument");
                    for (auto const key : {"ins_code", "isin", "symbol"}) {
                        auto const value = field_of(instrument, key);
                        if (truthy(value)) {
                            symbols.insert(upper_ascii(text_of(value)));
                        }
                    }
                }
            }
            return symbols;
        }

        [[nodiscard]] bool matches_watchlist(json const& data, std::unordered_set<std::string> const& symbols) {
            if (symbols.contains(upper_ascii(normal_of(field_of(data, "symbol"))))) {
                return true;
            }
            if (symbols.contains(upper_ascii(text_of(field_of(data, "isin"))))) {
                return true;
            }
            if (symbols.contains(text_of(field_of(data, "code")))) {
                return true;
            }
            return symbols.contains(text_of(field_of(data, "symbol")));
        }

        void build_filters(
          options const&       opts,
          std::vector<filter>& base,
          std::vector<filter>& info,
          std::vector<filter>& hist,
          std::vector<filter>& book) {
            auto add_numeric = [](std::vector<filter>&         bucket,
                                  std::string const&           label,
                                  std::string const&           key,
                                  std::optional<double> const& value,
                                  bool                         at_least) {
                if (!value) {
                    return;
                }
                double const threshold = *value;
                bucket.emplace_back(std::format("{} {}", label, py_number_text(threshold)),
                                    [key, threshold, at_least](json const& data) {
                                        auto const it = data.find(key);
                                        if (it == data.end() || it->is_null()) {
                                            return false;
                                        }
                                        auto const number = as_double(*it);
                                        if (!number) {
                                            return false;
                                        }
                                        return at_least ? *number >= threshold : *number <= threshold;
                                    });
            };

            // --type
            std::set<std::string> wanted_types;
            for (auto const& item : split_list(opts.type)) {
                auto const it = type_aliases().find(item);
                wanted_types.insert(it != type_aliases().end() ? it->second : item);
            }
            if (wanted_types.empty()) {
                wanted_types.insert("all");
            }
            if (!wanted_types.contains("all")) {
                std::set<std::string> known;
                for (auto const& [alias, canonical] : type_aliases()) {
                    (void) alias;
                    known.insert(canonical);
                }
                std::vector<std::string> unknown;
                for (auto const& item : wanted_types) {
                    if (!known.contains(item)) {
                        unknown.push_back(item);
                    }
                }
                if (!unknown.empty()) {
                    std::vector<std::string> choices;
                    for (auto const& item : known) {
                        if (item != "all") {
                            choices.push_back(item);
                        }
                    }
                    throw error(
                      std::format("unknown --type: {} (choose from {})", join(unknown, ", "), join(choices, ", ")));
                }
                std::vector<std::string> shown(wanted_types.begin(), wanted_types.end());
                base.emplace_back(std::format("type in {}", join(shown, ",")), [wanted_types](json const& data) {
                    auto const it = data.find("type");
                    return it != data.end() && it->is_string() && wanted_types.contains(it->get<std::string>());
                });
            }

            // --market
            std::set<long long> markets;
            for (auto const& item : split_list(opts.market)) {
                auto const it = flow_aliases().find(lower_ascii(item));
                if (it == flow_aliases().end()) {
                    std::vector<std::string> choices;
                    for (auto const& [alias, flows] : flow_aliases()) {
                        (void) flows;
                        choices.push_back(alias);
                    }
                    std::sort(choices.begin(), choices.end());
                    throw error(std::format("unknown --market: {} (choose from {})", item, join(choices, ", ")));
                }
                markets.insert(it->second.begin(), it->second.end());
            }
            if (!markets.empty()) {
                std::vector<std::string> shown;
                shown.reserve(markets.size());
                for (auto const value : markets) {
                    shown.push_back(std::to_string(value));
                }
                base.emplace_back(std::format("market in [{}]", join(shown, ", ")), [markets](json const& data) {
                    auto const it = data.find("flow");
                    if (it == data.end() || it->is_null()) {
                        return false;
                    }
                    auto const flow = as_int(*it);
                    return flow && markets.contains(*flow);
                });
            }

            // --board
            for (auto const& text : split_list(opts.board)) {
                auto const needle = lower_ascii(normal(text));
                base.emplace_back(std::format("board~{}", text), [needle](json const& data) {
                    auto const board = lower_ascii(normal_of(field_of(data, "board")));
                    auto const code  = lower_ascii(text_of(field_of(data, "board_code")));
                    return contains(board, needle) || needle == code;
                });
            }

            // --sector
            for (auto const& text : split_list(opts.sector)) {
                auto const                 needle = normal(text);
                std::optional<std::string> code;
                if (all_digits(needle)) {
                    code = needle.size() >= 2 ? needle : std::string(2 - needle.size(), '0') + needle;
                }
                base.emplace_back(std::format("sector~{}", text), [needle, code](json const& data) {
                    auto const sector      = lower_ascii(normal_of(field_of(data, "sector")));
                    auto const sector_code = text_of(field_of(data, "sector_code"));
                    if (contains(sector, needle)) {
                        return true;
                    }
                    if (code && *code == sector_code) {
                        return true;
                    }
                    return needle == sector_code;
                });
            }

            // --match
            for (auto const& text : split_list(opts.match)) {
                auto const needle = lower_ascii(normal(text));
                base.emplace_back(std::format("match~{}", text), [needle](json const& data) {
                    return contains(lower_ascii(normal_of(field_of(data, "symbol"))), needle) ||
                           contains(lower_ascii(normal_of(field_of(data, "name"))), needle);
                });
            }

            if (opts.watchlist && !opts.watchlist->empty()) {
                auto const symbols = watchlist_filter(*opts.watchlist);
                auto const label =
                  std::format("watchlist {}", std::filesystem::path(*opts.watchlist).filename().string());
                base.emplace_back(label, [symbols](json const& data) {
                    return matches_watchlist(data, symbols);
                });
            }
            if (opts.traded) {
                base.emplace_back("traded today", [](json const& data) {
                    return truthy(field_of(data, "traded"));
                });
            }
            if (opts.untraded) {
                base.emplace_back("not traded today", [](json const& data) {
                    return !truthy(field_of(data, "traded"));
                });
            }
            if (opts.up) {
                base.emplace_back("up on the day", [](json const& data) {
                    auto const it = data.find("chg");
                    return it != data.end() && !it->is_null() && num(*it) > 0.0;
                });
            }
            if (opts.down) {
                base.emplace_back("down on the day", [](json const& data) {
                    auto const it = data.find("chg");
                    return it != data.end() && !it->is_null() && num(*it) < 0.0;
                });
            }
            if (opts.states && !opts.states->empty()) {
                std::set<std::string> states;
                for (auto const& item : split_commas(*opts.states)) {
                    states.insert(upper_ascii(item));
                }
                std::vector<std::string> shown;
                shown.reserve(states.size());
                for (auto const& value : states) {
                    shown.push_back("'" + value + "'");
                }
                base.emplace_back(std::format("state in [{}]", join(shown, ", ")), [states](json const& data) {
                    return states.contains(upper_ascii(text_of(field_of(data, "state"))));
                });
            }

            add_numeric(base, "price-min", "price", opts.price_min, true);
            add_numeric(base, "price-max", "price", opts.price_max, false);
            add_numeric(base, "chg-min", "chg", opts.chg_min, true);
            add_numeric(base, "chg-max", "chg", opts.chg_max, false);
            add_numeric(base, "pe-min", "pe", opts.pe_min, true);
            add_numeric(base, "pe-max", "pe", opts.pe_max, false);
            add_numeric(base, "eps-min", "eps", opts.eps_min, true);
            add_numeric(base, "eps-max", "eps", opts.eps_max, false);
            add_numeric(base, "mcap-min", "mcap", opts.mcap_min, true);
            add_numeric(base, "mcap-max", "mcap", opts.mcap_max, false);
            add_numeric(base, "vol-min", "vol", opts.vol_min, true);
            add_numeric(base, "vol-max", "vol", opts.vol_max, false);
            add_numeric(base, "val-min", "value", opts.val_min, true);
            add_numeric(base, "val-max", "value", opts.val_max, false);
            add_numeric(base, "trades-min", "trades", opts.trades_min, true);
            add_numeric(base, "trades-max", "trades", opts.trades_max, false);
            add_numeric(base, "spread-max", "spread", opts.spread_max, false);
            add_numeric(base, "spread-pct-max", "spread_pct", opts.spread_pct_max, false);
            add_numeric(base, "near-limit", "_near_limit", opts.near_limit, false);

            if (opts.profitable) {
                base.emplace_back("profitable (EPS>0)", [](json const& data) {
                    auto const it = data.find("eps");
                    return it != data.end() && !it->is_null() && num(*it) > 0.0;
                });
            } else if (opts.loss_making) {
                base.emplace_back("loss-making (EPS<0)", [](json const& data) {
                    auto const it = data.find("eps");
                    return it != data.end() && !it->is_null() && num(*it) < 0.0;
                });
            }
            if (opts.at_ceiling) {
                base.emplace_back("at daily ceiling", [](json const& data) {
                    auto const it = data.find("to_ceil");
                    return it != data.end() && !it->is_null() && num(*it) <= 0.001;
                });
            }
            if (opts.at_floor) {
                base.emplace_back("at daily floor", [](json const& data) {
                    auto const it = data.find("to_floor");
                    return it != data.end() && !it->is_null() && num(*it) <= 0.001;
                });
            }

            add_numeric(info, "ps-min", "psr", opts.ps_min, true);
            add_numeric(info, "ps-max", "psr", opts.ps_max, false);
            add_numeric(info, "sector-pe-min", "sector_pe", opts.sector_pe_min, true);
            add_numeric(info, "sector-pe-max", "sector_pe", opts.sector_pe_max, false);
            add_numeric(info, "avg5d-min", "avg5d", opts.avg5d_min, true);
            add_numeric(info, "free-float-min", "free_float", opts.free_float_min, true);
            add_numeric(info, "near-52w-high", "y52h", opts.near_52w_high, false);
            add_numeric(info, "near-52w-low", "y52l", opts.near_52w_low, false);

            add_numeric(hist, "rsi-min", "rsi", opts.rsi_min, true);
            add_numeric(hist, "rsi-max", "rsi", opts.rsi_max, false);
            add_numeric(hist, "streak-min", "streak", opts.streak_min, true);
            add_numeric(hist, "streak-max", "streak", opts.streak_max, false);
            add_numeric(hist, "vol-surge", "surge", opts.vol_surge, true);

            if (opts.above_sma && *opts.above_sma != 0) {
                auto const key   = std::format("sma{}", *opts.above_sma);
                auto const label = std::format("above SMA{}", *opts.above_sma);
                hist.emplace_back(label, [key](json const& data) {
                    auto const price_it = data.find("price");
                    auto const sma_it   = data.find(key);
                    if (price_it == data.end() || sma_it == data.end() || !truthy(*price_it) || !truthy(*sma_it)) {
                        return false;
                    }
                    return num(*price_it) > num(*sma_it);
                });
            }
            if (opts.below_sma && *opts.below_sma != 0) {
                auto const key   = std::format("sma{}", *opts.below_sma);
                auto const label = std::format("below SMA{}", *opts.below_sma);
                hist.emplace_back(label, [key](json const& data) {
                    auto const price_it = data.find("price");
                    auto const sma_it   = data.find(key);
                    if (price_it == data.end() || sma_it == data.end() || !truthy(*price_it) || !truthy(*sma_it)) {
                        return false;
                    }
                    return num(*price_it) < num(*sma_it);
                });
            }
            if (opts.macd) {
                auto const want  = opts.macd->starts_with("b") ? std::string("bull") : std::string("bear");
                auto const label = std::format("macd {}", want);
                hist.emplace_back(label, [want](json const& data) {
                    auto const it = data.find("macd");
                    return it != data.end() && it->is_string() && it->get<std::string>() == want;
                });
            }

            auto add_cross = [&hist](std::optional<std::string> const& spec, std::string_view direction) {
                if (!spec || spec->empty()) {
                    return;
                }
                auto const parts = split_on_any(*spec, ", \t\n\r\f\v");
                if (parts.size() != 2) {
                    throw error(std::format("--cross expects two periods, e.g. 20,50 (got '{}')", *spec));
                }
                long long fast = 0;
                long long slow = 0;
                if (!parse_int_text(parts[0], fast) || !parse_int_text(parts[1], slow)) {
                    throw error(std::format("--cross expects two periods, e.g. 20,50 (got '{}')", *spec));
                }
                if (fast >= slow) {
                    throw error(std::format("--cross fast period must be shorter than slow ({} >= {})", fast, slow));
                }
                auto const key   = std::format("cross_{}_{}_{}", fast, slow, direction);
                auto const label = std::format("{} cross {}/{}", direction, fast, slow);
                hist.emplace_back(label, [key](json const& data) {
                    auto const it = data.find(key);
                    return it != data.end() && it->is_boolean() && it->get<bool>();
                });
            };
            add_cross(opts.cross, "bull");
            add_cross(opts.cross_below, "bear");

            if (opts.bid_wall_min) {
                double const threshold = *opts.bid_wall_min;
                book.emplace_back(std::format("bid wall >= {}", py_number_text(threshold)),
                                  [threshold](json const& data) {
                                      auto const it = data.find("bid_wall");
                                      return it != data.end() && !it->is_null() && num(*it) >= threshold;
                                  });
            }
            if (opts.ask_wall_min) {
                double const threshold = *opts.ask_wall_min;
                book.emplace_back(std::format("ask wall >= {}", py_number_text(threshold)),
                                  [threshold](json const& data) {
                                      auto const it = data.find("ask_wall");
                                      return it != data.end() && !it->is_null() && num(*it) >= threshold;
                                  });
            }
            if (opts.imbalance) {
                auto const [low, high] = *opts.imbalance;
                auto const label       = high == std::numeric_limits<double>::infinity()
                                           ? std::format("imbalance >= {:g}", low)
                                           : std::format("imbalance in [{:g}, {:g}]", low, high);
                book.emplace_back(label, [low, high](json const& data) {
                    auto const it = data.find("imbalance");
                    return it != data.end() && !it->is_null() && num(*it) >= low && num(*it) <= high;
                });
            }
        }

        // ------------------------------------------------------------------ enrichment

        void fetch_info(client& http, record& item) {
            auto const code = text_of(field_of(item.data, "code"));
            auto const row  = http.api(std::format("Instrument/GetInstrumentInfo/{}", code), "instrumentInfo");
            if (!row.is_object()) {
                throw error("No instrument info");
            }
            auto const earnings  = pick(row, {"eps"});
            json       sector_pe = earnings.is_object() ? value_of(earnings, {"sectorPE"}) : json(nullptr);
            json       psr       = earnings.is_object() ? value_of(earnings, {"psr"}) : json(nullptr);
            auto const sector    = pick(row, {"sector"});

            item.data["sector_pe"]  = !sector_pe.is_null() ? sector_pe : value_of(row, {"sectorPE"});
            item.data["psr"]        = !psr.is_null() ? psr : value_of(row, {"psr"});
            item.data["avg5d"]      = value_of(row, {"qTotTran5JAvg"});
            item.data["year_low"]   = value_of(row, {"minYear"});
            item.data["year_high"]  = value_of(row, {"maxYear"});
            item.data["week_low"]   = value_of(row, {"minWeek"});
            item.data["week_high"]  = value_of(row, {"maxWeek"});
            item.data["free_float"] = value_of(row, {"freeFloatPercent", "floatPercent"});

            if (sector.is_object() && truthy(pick(sector, {"lSecVal"}))) {
                item.data["sector"] = normal(text_of(pick(sector, {"lSecVal"})));
            }
            auto const board_title = pick(row, {"cgrValCotTitle"});
            if (truthy(board_title) && text_of(board_title) != "-'") {
                item.data["board"] = normal(text_of(board_title));
            }
            auto const flow_title = pick(row, {"flowTitle"});
            if (truthy(flow_title)) {
                item.data["market"] = normal(text_of(flow_title));
            }
            if (truthy(field_of(item.data, "year_high")) && truthy(field_of(item.data, "price"))) {
                double const high  = num(field_of(item.data, "year_high"));
                double const price = num(field_of(item.data, "price"));
                item.data["y52h"]  = json(round_to((high - price) / high * 100.0, 2));
            }
            if (truthy(field_of(item.data, "year_low")) && truthy(field_of(item.data, "price"))) {
                double const low   = num(field_of(item.data, "year_low"));
                double const price = num(field_of(item.data, "price"));
                item.data["y52l"]  = json(round_to((price - low) / low * 100.0, 2));
            }
            if (field_of(item.data, "eps").is_null()) {
                auto const estimate = earnings.is_object() ? value_of(earnings, {"estimatedEPS"}) : json(nullptr);
                if (truthy(estimate)) {
                    item.data["eps"] = estimate;
                }
            }
        }

        [[nodiscard]] std::vector<std::optional<double>> sma_series(std::vector<double> const& values,
                                                                    long long                  period) {
            std::vector<std::optional<double>> series;
            if (period <= 0 || values.size() < static_cast<std::size_t>(period)) {
                return series;
            }
            auto const count = static_cast<std::size_t>(period);
            series.assign(values.size(), std::nullopt);
            double window = 0.0;
            for (std::size_t i = 0; i < count; ++i) {
                window += values[i];
            }
            series[count - 1] = window / static_cast<double>(period);
            for (std::size_t i = count; i < values.size(); ++i) {
                window    += values[i] - values[i - count];
                series[i]  = window / static_cast<double>(period);
            }
            return series;
        }

        [[nodiscard]] std::vector<std::optional<double>> ema_series(std::vector<double> const& values,
                                                                    long long                  period) {
            std::vector<std::optional<double>> series;
            if (period <= 0 || values.size() < static_cast<std::size_t>(period)) {
                return series;
            }
            auto const   count  = static_cast<std::size_t>(period);
            double const factor = 2.0 / (static_cast<double>(period) + 1.0);
            series.assign(count - 1, std::nullopt);
            double sum = 0.0;
            for (std::size_t i = 0; i < count; ++i) {
                sum += values[i];
            }
            double previous = sum / static_cast<double>(period);
            series.push_back(previous);
            for (std::size_t i = count; i < values.size(); ++i) {
                previous = values[i] * factor + previous * (1.0 - factor);
                series.push_back(previous);
            }
            return series;
        }

        [[nodiscard]] std::optional<double> rsi_of(std::vector<double> const& closes, long long period = 14) {
            if (period <= 0 || closes.size() < static_cast<std::size_t>(period) + 1) {
                return std::nullopt;
            }
            double gains  = 0.0;
            double losses = 0.0;
            for (std::size_t i = closes.size() - static_cast<std::size_t>(period); i < closes.size(); ++i) {
                double const delta = closes[i] - closes[i - 1];
                if (delta >= 0.0) {
                    gains += delta;
                } else {
                    losses -= delta;
                }
            }
            if (losses == 0.0) {
                return 100.0;
            }
            double const relative = (gains / static_cast<double>(period)) / (losses / static_cast<double>(period));
            return round_to(100.0 - 100.0 / (1.0 + relative), 2);
        }

        [[nodiscard]] std::optional<bool>
        cross_flag(std::vector<double> const& closes, long long fast, long long slow, std::string_view direction) {
            auto const fast_series = sma_series(closes, fast);
            auto const slow_series = sma_series(closes, slow);
            if (fast_series.empty() || slow_series.empty()) {
                return std::nullopt;
            }
            auto const count = closes.size();
            auto const stop  = count > 11 ? count - 11 : 0;
            for (std::size_t index = count - 1; index > stop; --index) {
                auto const fast_prev = fast_series[index - 1];
                auto const slow_prev = slow_series[index - 1];
                auto const fast_now  = fast_series[index];
                auto const slow_now  = slow_series[index];
                if (!fast_prev || !slow_prev || !fast_now || !slow_now) {
                    continue;
                }
                bool const crossed_up   = *fast_prev <= *slow_prev && *fast_now > *slow_now;
                bool const crossed_down = *fast_prev >= *slow_prev && *fast_now < *slow_now;
                if (direction == "bull" && crossed_up) {
                    return true;
                }
                if (direction == "bear" && crossed_down) {
                    return true;
                }
                if (crossed_up || crossed_down) {
                    return false;
                }
            }
            return false;
        }

        struct [[nodiscard]] cross_spec {
            long long   fast = 0;
            long long   slow = 0;
            std::string direction;
        };

        void fetch_history(
          client&                        http,
          record&                        item,
          long long                      days,
          std::vector<cross_spec> const& crosses,
          std::vector<long long> const&  sma_periods) {
            auto const code = text_of(field_of(item.data, "code"));
            auto const payload =
              http.api(std::format("ClosingPrice/GetClosingPriceDailyList/{}/{}", code, days), "closingPriceDaily");
            if (!payload.is_array()) {
                throw error("Unexpected daily history result");
            }

            struct bar {
                std::string date;
                double      close  = 0.0;
                double      volume = 0.0;
            };

            std::vector<bar> bars;
            bars.reserve(payload.size());
            for (auto const& row : payload) {
                if (!row.is_object()) {
                    continue;
                }
                auto const date = trade_date(pick(row, {"dEven"}));
                if (date.empty()) {
                    continue;
                }
                auto const closing = price(pick(row, {"pClosing", "pc"}));
                auto const volume  = value_of(row, {"qTotTran5J", "tvol"});
                bars.push_back(bar{date, closing.is_null() ? 0.0 : num(closing), volume.is_null() ? 0.0 : num(volume)});
            }
            std::stable_sort(bars.begin(), bars.end(), [](bar const& a, bar const& b) {
                return a.date < b.date;
            });
            if (days != 0 && bars.size() > static_cast<std::size_t>(days)) {
                bars.erase(bars.begin(), bars.end() - static_cast<std::ptrdiff_t>(days));
            }

            std::vector<double> closes;
            std::vector<double> volumes;
            closes.reserve(bars.size());
            volumes.reserve(bars.size());
            for (auto const& entry : bars) {
                if (entry.close != 0.0) {
                    closes.push_back(entry.close);
                }
                volumes.push_back(entry.volume);
            }
            if (closes.size() < 3) {
                throw error("History too short");
            }

            item.data["rsi"] = jnum(rsi_of(closes));
            for (auto const period : sma_periods) {
                auto const series                       = sma_series(closes, period);
                auto const last                         = series.empty() ? std::optional<double>{} : series.back();
                item.data[std::format("sma{}", period)] = last ? json(round_to(*last, 2)) : json(nullptr);
            }

            auto const fast = ema_series(closes, 12);
            auto const slow = ema_series(closes, 26);
            if (fast.size() == slow.size() && !fast.empty()) {
                std::vector<double> macd_line;
                macd_line.reserve(fast.size());
                for (std::size_t i = 0; i < fast.size(); ++i) {
                    auto const fast_value = fast[i];
                    auto const slow_value = slow[i];
                    if (fast_value && slow_value) {
                        macd_line.push_back(*fast_value - *slow_value);
                    }
                }
                if (macd_line.size() >= 9) {
                    auto const signal = ema_series(macd_line, 9);
                    if (!signal.empty() && signal.back()) {
                        item.data["macd"] = macd_line.back() > *signal.back() ? "bull" : "bear";
                    }
                }
            }

            long long streak = 0;
            for (std::size_t index = closes.size() - 1; index > 0; --index) {
                bool const rising = closes[index] > closes[index - 1];
                if (streak == 0) {
                    streak = rising ? 1 : -1;
                    if (closes.back() == closes[closes.size() - 2]) {
                        break;
                    }
                } else if ((streak > 0) != rising) {
                    break;
                } else if (streak > 0) {
                    ++streak;
                } else {
                    --streak;
                }
            }
            item.data["streak"] = streak;

            std::vector<double> recent_volume;
            auto const          start = volumes.size() > 21 ? volumes.size() - 21 : 0;
            auto const          end   = volumes.empty() ? 0 : volumes.size() - 1;
            for (std::size_t i = start; i < end; ++i) {
                if (volumes[i] != 0.0) {
                    recent_volume.push_back(volumes[i]);
                }
            }
            if (!recent_volume.empty() && truthy(field_of(item.data, "vol"))) {
                double sum = 0.0;
                for (double const value : recent_volume) {
                    sum += value;
                }
                double const average = sum / static_cast<double>(recent_volume.size());
                item.data["surge"]   = json(round_to(num(field_of(item.data, "vol")) / average, 2));
            }

            for (auto const& spec : crosses) {
                auto const flag = cross_flag(closes, spec.fast, spec.slow, spec.direction);
                item.data[std::format("cross_{}_{}_{}", spec.fast, spec.slow, spec.direction)] =
                  flag ? json(*flag) : json(nullptr);
            }
        }

        void fetch_book(record& item) {
            std::vector<json> levels;
            for (auto const& level : item.levels) {
                if (level.is_object()) {
                    levels.push_back(level);
                }
            }
            std::vector<json> bids;
            std::vector<json> asks;
            double            bid_total = 0.0;
            double            ask_total = 0.0;
            for (auto const& level : levels) {
                auto const bid = field_of(level, "qmd");
                auto const ask = field_of(level, "qmo");
                bids.push_back(truthy(bid) ? bid : json(0));
                asks.push_back(truthy(ask) ? ask : json(0));
                bid_total += truthy(bid) ? num(bid) : 0.0;
                ask_total += truthy(ask) ? num(ask) : 0.0;
            }
            json bid_wall = nullptr;
            json ask_wall = nullptr;
            if (!bids.empty()) {
                auto const best = std::max_element(bids.begin(), bids.end(), [](json const& a, json const& b) {
                    return num(a) < num(b);
                });
                bid_wall        = *best;
            }
            if (!asks.empty()) {
                auto const best = std::max_element(asks.begin(), asks.end(), [](json const& a, json const& b) {
                    return num(a) < num(b);
                });
                ask_wall        = *best;
            }
            item.data["bid_wall"] = std::move(bid_wall);
            item.data["ask_wall"] = std::move(ask_wall);
            if (bid_total != 0.0 && ask_total != 0.0) {
                item.data["imbalance"] = json(round_to(bid_total / ask_total, 2));
            }
        }

        struct [[nodiscard]] enrich_counts {
            std::size_t fetched = 0;
            std::size_t skipped = 0;
        };

        [[nodiscard]] enrich_counts enrich(
          std::vector<record>&                records,
          std::function<void(record&)> const& worker,
          long long                           jobs,
          std::optional<long long>            cap,
          std::string_view                    stage,
          std::string&                        err) {
            if (records.empty()) {
                return {};
            }
            std::vector<record*> ordered;
            ordered.reserve(records.size());
            for (auto& item : records) {
                ordered.push_back(&item);
            }
            std::stable_sort(ordered.begin(), ordered.end(), [](record const* a, record const* b) {
                return num(field_of(a->data, "value")) > num(field_of(b->data, "value"));
            });

            auto target_count = ordered.size();
            if (cap && *cap >= 0) {
                target_count = std::min(target_count, static_cast<std::size_t>(*cap));
            }
            std::vector<record*> targets(ordered.begin(), ordered.begin() + static_cast<std::ptrdiff_t>(target_count));
            auto const           skipped = ordered.size() - targets.size();

            std::vector<std::string> failures;
            if (!targets.empty()) {
                std::atomic<std::size_t> next{0};
                std::mutex               mutex;
                auto const               workers =
                  std::min<std::size_t>(static_cast<std::size_t>(std::max<long long>(jobs, 1)), targets.size());
                std::vector<std::thread> threads;
                threads.reserve(workers);
                for (std::size_t worker_index = 0; worker_index < workers; ++worker_index) {
                    threads.emplace_back([&] {
                        for (;;) {
                            auto const index = next.fetch_add(1);
                            if (index >= targets.size()) {
                                break;
                            }
                            try {
                                worker(*targets[index]);
                            } catch (std::exception const& exc) {
                                std::lock_guard<std::mutex> guard(mutex);
                                failures.push_back(
                                  std::format("{}: {}", text_of(field_of(targets[index]->data, "symbol")), exc.what()));
                            } catch (...) {
                                std::lock_guard<std::mutex> guard(mutex);
                                failures.push_back(
                                  std::format("{}: unknown error", text_of(field_of(targets[index]->data, "symbol"))));
                            }
                        }
                    });
                }
                for (auto& thread : threads) {
                    thread.join();
                }
            }
            if (!failures.empty()) {
                err += std::format(
                  "warning: {}: {}/{} fetches failed (e.g. {})\n",
                  stage,
                  failures.size(),
                  targets.size(),
                  failures.front());
            }
            return {targets.size() - failures.size(), skipped};
        }

        // ------------------------------------------------------------------ sorting

        [[nodiscard]] int compare_values(json const& a, json const& b) {
            if (a.is_string() && b.is_string()) {
                auto const order = a.get_ref<std::string const&>().compare(b.get_ref<std::string const&>());
                return (order > 0) - (order < 0);
            }
            auto const left  = as_double(a);
            auto const right = as_double(b);
            if (left && right) {
                if (*left < *right) {
                    return -1;
                }
                if (*left > *right) {
                    return 1;
                }
                return 0;
            }
            auto const order = scalar_text(a).compare(scalar_text(b));
            return (order > 0) - (order < 0);
        }

        void sort_records(std::vector<record>& records, std::string const& key, bool ascending) {
            if (key == "name") {
                std::stable_sort(records.begin(), records.end(), [ascending](record const& a, record const& b) {
                    auto const left  = normal_of(field_of(a.data, "name"));
                    auto const right = normal_of(field_of(b.data, "name"));
                    return ascending ? left < right : left > right;
                });
                return;
            }
            if (!columns_table().contains(key)) {
                throw error(std::format("unknown --sort key '{}'; choose from {} or name",
                                        key,
                                        join(sorted_column_keys(), ", ")));
            }
            auto const missing = [](json const& value) {
                return value.is_null() || value.is_boolean();
            };
            std::stable_sort(records.begin(), records.end(), [&](record const& a, record const& b) {
                auto const left          = field_of(a.data, key);
                auto const right         = field_of(b.data, key);
                bool const left_missing  = missing(left);
                bool const right_missing = missing(right);
                if (left_missing != right_missing) {
                    return ascending ? right_missing : left_missing;
                }
                if (left_missing) {
                    return false;
                }
                auto const order = compare_values(left, right);
                return ascending ? order < 0 : order > 0;
            });
        }

        // ------------------------------------------------------------------ rendering

        [[nodiscard]] std::string grouped_integer(double value) {
            auto        text     = std::format("{:.0f}", value);
            bool const  negative = !text.empty() && text.front() == '-';
            auto const  digits   = negative ? std::string_view(text).substr(1) : std::string_view(text);
            std::string grouped;
            grouped.reserve(digits.size() + digits.size() / 3);
            int count = 0;
            for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
                if (count != 0 && count % 3 == 0) {
                    grouped.push_back(',');
                }
                grouped.push_back(*it);
                ++count;
            }
            std::reverse(grouped.begin(), grouped.end());
            return negative ? "-" + grouped : grouped;
        }

        [[nodiscard]] std::string fmt_amount(json const& value) {
            if (value.is_null()) {
                return "-";
            }
            double const amount = num(value);

            struct unit {
                double divisor;
                char   suffix;
            };

            static constexpr unit units[] = {
              {1e15, 'P'},
              {1e12, 'T'},
              { 1e9, 'B'},
              { 1e6, 'M'},
              { 1e3, 'K'},
            };
            for (auto const& entry : units) {
                if (std::abs(amount) >= entry.divisor) {
                    return std::format("{:.2f}{}", amount / entry.divisor, entry.suffix);
                }
            }
            return std::format("{:g}", amount);
        }

        [[nodiscard]] std::string fmt_cell(std::string_view key, json const& value, std::string_view kind) {
            if (value.is_null()) {
                return "-";
            }
            if (kind == "amt") {
                return fmt_amount(value);
            }
            if (kind == "pct") {
                return key == "chg" ? std::format("{:+.2f}", num(value)) : std::format("{:.2f}", num(value));
            }
            if (kind == "float") {
                if (key == "rsi") {
                    return std::format("{:.0f}", num(value));
                }
                if (key == "streak") {
                    return value.is_number_integer() ? std::format("{:+d}", value.get<long long>())
                                                     : std::format("{:.0f}", num(value));
                }
                if (key == "surge") {
                    return std::format("{:.2f}x", num(value));
                }
                if (key == "imbalance") {
                    return std::format("{:.2f}", num(value));
                }
                return std::format("{:.1f}", num(value));
            }
            if (kind == "int") {
                return key == "streak" ? std::format("{:+d}", as_ll(value)) : grouped_integer(num(value));
            }
            return scalar_text(value);
        }

        [[nodiscard]] std::size_t display_width(std::string_view text) noexcept {
            std::size_t count = 0;
            for (char const raw : text) {
                auto const c = static_cast<unsigned char>(raw);
                if ((c & 0xC0U) != 0x80U) {
                    ++count;
                }
            }
            return count;
        }

        [[nodiscard]] std::string ljust(std::string const& text, std::size_t width) {
            auto const current = display_width(text);
            return current >= width ? text : text + std::string(width - current, ' ');
        }

        [[nodiscard]] std::string rjust(std::string const& text, std::size_t width) {
            auto const current = display_width(text);
            return current >= width ? text : std::string(width - current, ' ') + text;
        }

        [[nodiscard]] std::string render_table(std::vector<record> const&      records,
                                               std::vector<std::string> const& cols) {
            std::vector<std::string> header;
            header.reserve(cols.size());
            for (auto const& key : cols) {
                header.emplace_back(columns_table().at(key).title);
            }
            std::vector<std::vector<std::string>> body;
            body.reserve(records.size());
            for (auto const& item : records) {
                std::vector<std::string> row;
                row.reserve(cols.size());
                for (auto const& key : cols) {
                    auto const& info = columns_table().at(key);
                    row.push_back(fmt_cell(key, field_of(item.data, key), info.kind));
                }
                body.push_back(std::move(row));
            }
            std::vector<std::size_t> widths;
            widths.reserve(header.size());
            for (auto const& text : header) {
                widths.push_back(display_width(text));
            }
            for (auto const& row : body) {
                for (std::size_t index = 0; index < row.size(); ++index) {
                    widths[index] = std::max(widths[index], display_width(row[index]));
                }
            }

            std::string out;
            for (std::size_t index = 0; index < header.size(); ++index) {
                if (index != 0) {
                    out += "  ";
                }
                out += ljust(header[index], widths[index]);
            }
            out += '\n';
            for (std::size_t index = 0; index < header.size(); ++index) {
                if (index != 0) {
                    out += "  ";
                }
                out += std::string(widths[index], '-');
            }
            for (auto const& row : body) {
                out += '\n';
                std::string line;
                for (std::size_t index = 0; index < row.size(); ++index) {
                    if (index != 0) {
                        line += "  ";
                    }
                    line += columns_table().at(cols[index]).kind == "text"
                              ? ljust(row[index], widths[index])
                              : rjust(row[index], widths[index]);
                }
                while (!line.empty() && is_space(line.back())) {
                    line.pop_back();
                }
                out += line;
            }
            return out;
        }

        [[nodiscard]] std::string render_md(std::vector<record> const& records, std::vector<std::string> const& cols) {
            std::vector<std::string> header;
            header.reserve(cols.size());
            for (auto const& key : cols) {
                header.emplace_back(columns_table().at(key).title);
            }
            std::vector<std::string> dashes(header.size(), "---");
            std::string              out = "| " + join(header, " | ") + " |\n| " + join(dashes, " | ") + " |";
            for (auto const& item : records) {
                std::vector<std::string> cells;
                cells.reserve(cols.size());
                for (auto const& key : cols) {
                    auto const& info = columns_table().at(key);
                    cells.push_back(fmt_cell(key, field_of(item.data, key), info.kind));
                }
                out += "\n| " + join(cells, " | ") + " |";
            }
            return out;
        }

        [[nodiscard]] std::string csv_field(std::string const& text) {
            if (text.find_first_of(",\"\r\n") == std::string::npos) {
                return text;
            }
            std::string out = "\"";
            for (char const c : text) {
                if (c == '"') {
                    out += "\"\"";
                } else {
                    out.push_back(c);
                }
            }
            out += '"';
            return out;
        }

        [[nodiscard]] std::string csv_text(json const& value) {
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

        [[nodiscard]] std::string render_csv(std::vector<record> const& records, std::vector<std::string> const& cols) {
            std::string out = join(cols, ",");
            for (auto const& item : records) {
                out += '\n';
                for (std::size_t index = 0; index < cols.size(); ++index) {
                    if (index != 0) {
                        out += ',';
                    }
                    out += csv_field(csv_text(field_of(item.data, cols[index])));
                }
            }
            return out;
        }

        struct [[nodiscard]] counter {
            std::vector<std::string>                     order;
            std::unordered_map<std::string, std::size_t> counts;

            void add(std::string const& key) {
                auto [it, inserted] = counts.try_emplace(key, 0);
                if (inserted) {
                    order.push_back(key);
                }
                ++it->second;
            }

            [[nodiscard]] std::vector<std::pair<std::string, std::size_t>> most_common(std::size_t limit = 0) const {
                std::vector<std::pair<std::string, std::size_t>> items;
                items.reserve(order.size());
                for (auto const& key : order) {
                    items.emplace_back(key, counts.at(key));
                }
                std::stable_sort(items.begin(), items.end(), [](auto const& a, auto const& b) {
                    return a.second > b.second;
                });
                if (limit != 0 && items.size() > limit) {
                    items.resize(limit);
                }
                return items;
            }
        };

        [[nodiscard]] std::string
        stats_summary(std::size_t total, std::vector<record> const& records, std::vector<std::string> const& notes) {
            std::string out = std::format("matched {} / {} instruments", records.size(), total);
            for (auto const& note : notes) {
                out += std::format("\n  {}", note);
            }
            if (records.empty()) {
                return out;
            }

            counter types;
            for (auto const& item : records) {
                types.add(text_of(field_of(item.data, "type")));
            }
            out                  += "\n  types: ";
            auto const type_rows  = types.most_common();
            for (std::size_t index = 0; index < type_rows.size(); ++index) {
                if (index != 0) {
                    out += ", ";
                }
                out += std::format("{} {}", type_rows[index].first, type_rows[index].second);
            }

            std::size_t up    = 0;
            std::size_t down  = 0;
            double      value = 0.0;
            for (auto const& item : records) {
                auto const change = num(field_of(item.data, "chg"));
                if (change > 0.0) {
                    ++up;
                } else if (change < 0.0) {
                    ++down;
                }
                value += num(field_of(item.data, "value"));
            }
            auto const flat  = records.size() - up - down;
            out             += std::format(
              "\n  advancers {}, decliners {}, unchanged {}; traded value {} rial",
              up,
              down,
              flat,
              fmt_amount(json(value)));

            std::vector<double> pes;
            for (auto const& item : records) {
                auto const pe = field_of(item.data, "pe");
                if (truthy(pe) && num(pe) > 0.0) {
                    pes.push_back(num(pe));
                }
            }
            if (!pes.empty()) {
                std::sort(pes.begin(), pes.end());
                double const median =
                  pes.size() % 2 == 1 ? pes[pes.size() / 2] : (pes[pes.size() / 2 - 1] + pes[pes.size() / 2]) / 2.0;
                out += std::format("\n  median P/E {:.1f} (n={})", median, pes.size());
            }

            counter sectors;
            for (auto const& item : records) {
                auto const sector = field_of(item.data, "sector");
                sectors.add(truthy(sector) ? text_of(sector) : std::string("?"));
            }
            out                    += "\n  top sectors: ";
            auto const sector_rows  = sectors.most_common(5);
            for (std::size_t index = 0; index < sector_rows.size(); ++index) {
                if (index != 0) {
                    out += ", ";
                }
                out += std::format("{} {}", sector_rows[index].first, sector_rows[index].second);
            }
            return out;
        }

        // ------------------------------------------------------------------ json output

        void write_json(std::string& out, json const& value, int indent, int level) {
            if (value.is_object()) {
                if (value.empty()) {
                    out += "{}";
                    return;
                }
                out        += '{';
                bool first  = true;
                for (auto it = value.begin(); it != value.end(); ++it) {
                    if (!first) {
                        out += indent > 0 ? "," : ", ";
                    }
                    first = false;
                    if (indent > 0) {
                        out += '\n';
                        out.append(static_cast<std::size_t>(indent) * static_cast<std::size_t>(level + 1), ' ');
                    }
                    out += json(it.key()).dump();
                    out += ": ";
                    write_json(out, it.value(), indent, level + 1);
                }
                if (indent > 0) {
                    out += '\n';
                    out.append(static_cast<std::size_t>(indent) * static_cast<std::size_t>(level), ' ');
                }
                out += '}';
                return;
            }
            if (value.is_array()) {
                if (value.empty()) {
                    out += "[]";
                    return;
                }
                out        += '[';
                bool first  = true;
                for (auto const& item : value) {
                    if (!first) {
                        out += indent > 0 ? "," : ", ";
                    }
                    first = false;
                    if (indent > 0) {
                        out += '\n';
                        out.append(static_cast<std::size_t>(indent) * static_cast<std::size_t>(level + 1), ' ');
                    }
                    write_json(out, item, indent, level + 1);
                }
                if (indent > 0) {
                    out += '\n';
                    out.append(static_cast<std::size_t>(indent) * static_cast<std::size_t>(level), ' ');
                }
                out += ']';
                return;
            }
            out += value.dump();
        }

        [[nodiscard]] std::string py_dump(json const& value, bool indent) {
            std::string out;
            write_json(out, value, indent ? 2 : 0, 0);
            return out;
        }

        // ------------------------------------------------------------------ shell words

        [[nodiscard]] std::vector<std::string> shlex_split(std::string_view text) {
            enum class state : std::uint8_t {
                space,
                token,
                quoted,
                escaped,
                escaped_quoted
            };
            std::vector<std::string> out;
            std::string              token;
            bool                     started = false;
            state                    current = state::space;
            char                     quote   = '\0';
            for (char const c : text) {
                switch (current) {
                    case state::space:
                        if (is_space(c)) {
                            break;
                        }
                        started = true;
                        if (c == '\'') {
                            current = state::quoted;
                            quote   = '\'';
                        } else if (c == '"') {
                            current = state::quoted;
                            quote   = '"';
                        } else if (c == '\\') {
                            current = state::escaped;
                        } else {
                            token.push_back(c);
                            current = state::token;
                        }
                        break;
                    case state::token:
                        if (is_space(c)) {
                            out.push_back(token);
                            token.clear();
                            started = false;
                            current = state::space;
                        } else if (c == '\'') {
                            current = state::quoted;
                            quote   = '\'';
                        } else if (c == '"') {
                            current = state::quoted;
                            quote   = '"';
                        } else if (c == '\\') {
                            current = state::escaped;
                        } else {
                            token.push_back(c);
                        }
                        break;
                    case state::quoted:
                        if (c == quote) {
                            current = state::token;
                        } else if (quote == '"' && c == '\\') {
                            current = state::escaped_quoted;
                        } else {
                            token.push_back(c);
                        }
                        break;
                    case state::escaped:
                        if (c != '\n') {
                            token.push_back(c);
                        }
                        current = state::token;
                        break;
                    case state::escaped_quoted:
                        if (c == '"') {
                            token.push_back('"');
                        } else {
                            token.push_back('\\');
                            token.push_back(c);
                        }
                        current = state::quoted;
                        break;
                }
            }
            if (current == state::quoted) {
                throw error("No closing quotation");
            }
            if (current == state::escaped) {
                throw error("No escaped character");
            }
            if (started) {
                out.push_back(token);
            }
            return out;
        }

        [[nodiscard]] std::string shlex_quote(std::string_view text) {
            if (text.empty()) {
                return "''";
            }
            bool safe = true;
            for (char const raw : text) {
                auto const c = static_cast<unsigned char>(raw);
                bool const plain =
                  (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '@' ||
                  c == '%' || c == '+' || c == '=' || c == ':' || c == ',' || c == '.' || c == '/' || c == '-';
                if (!plain) {
                    safe = false;
                    break;
                }
            }
            if (safe) {
                return std::string(text);
            }
            std::string out = "'";
            for (char const c : text) {
                if (c == '\'') {
                    out += "'\"'\"'";
                } else {
                    out.push_back(c);
                }
            }
            out += '\'';
            return out;
        }

        [[nodiscard]] std::string shlex_join(std::vector<std::string> const& words) {
            std::string out;
            for (std::size_t index = 0; index < words.size(); ++index) {
                if (index != 0) {
                    out.push_back(' ');
                }
                out += shlex_quote(words[index]);
            }
            return out;
        }

        [[nodiscard]] std::string tehran_iso_now() {
            auto const shifted = static_cast<std::time_t>(tehran_now() + 3LL * 3600 + 30LL * 60);
            std::tm    parts{};
            ::gmtime_r(&shifted, &parts);
            return std::format(
              "{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}+03:30",
              parts.tm_year + 1900,
              parts.tm_mon + 1,
              parts.tm_mday,
              parts.tm_hour,
              parts.tm_min,
              parts.tm_sec);
        }

        // ------------------------------------------------------------------ argument parsing

        [[nodiscard]] bool takes_value(std::string_view name) noexcept {
            static constexpr std::string_view flags[] = {
              "--type",          "--market",         "--board",         "--sector",         "--match",
              "--watchlist",     "--states",         "--price-min",     "--price-max",      "--chg-min",
              "--chg-max",       "--near-limit",     "--pe-min",        "--pe-max",         "--eps-min",
              "--eps-max",       "--mcap-min",       "--mcap-max",      "--ps-min",         "--ps-max",
              "--sector-pe-min", "--sector-pe-max",  "--near-52w-high", "--near-52w-low",   "--vol-min",
              "--vol-max",       "--val-min",        "--val-max",       "--trades-min",     "--trades-max",
              "--spread-max",    "--spread-pct-max", "--avg5d-min",     "--free-float-min", "--vol-surge",
              "--bid-wall-min",  "--ask-wall-min",   "--imbalance",     "--rsi-min",        "--rsi-max",
              "--above-sma",     "--below-sma",      "--cross",         "--cross-below",    "--macd",
              "--streak-min",    "--streak-max",     "--history-days",  "--max-enrich",     "--jobs",
              "--format",        "--sort",           "--limit",         "--columns",
            };
            return std::find(std::begin(flags), std::end(flags), name) != std::end(flags);
        }

        [[nodiscard]] std::pair<double, double> resolve_imbalance(std::string const& text) {
            auto const parts = split_on_any(trim(text), ":, \t\n\r\f\v");
            auto const parse = [&text](std::string const& part) {
                double value = 0.0;
                if (!parse_double_text(part, value)) {
                    throw error(std::format("expected LOW or LOW:HIGH (got '{}')", text));
                }
                return value;
            };
            double low  = 0.0;
            double high = 0.0;
            if (parts.size() == 1) {
                low  = parse(parts[0]);
                high = std::numeric_limits<double>::infinity();
            } else if (parts.size() == 2) {
                low  = parse(parts[0]);
                high = parse(parts[1]);
            } else {
                throw error(std::format("expected LOW or LOW:HIGH (got '{}')", text));
            }
            if (low > high) {
                std::swap(low, high);
            }
            return {low, high};
        }

        [[nodiscard]] options parse_args(std::vector<std::string> const& argv) {
            options     opts;
            std::size_t index = 0;

            auto value_of = [&](std::string_view name, std::optional<std::string> const& inline_value) -> std::string {
                if (inline_value) {
                    return *inline_value;
                }
                if (index + 1 >= argv.size()) {
                    throw arg_error(std::format("argument {}: expected one argument", name));
                }
                return argv[++index];
            };
            auto reject_inline = [](std::string_view name, std::optional<std::string> const& inline_value) {
                if (inline_value) {
                    throw arg_error(std::format("argument {}: ignored explicit argument '{}'", name, *inline_value));
                }
            };
            auto int_arg = [](std::string_view name, std::string const& text) {
                long long value = 0;
                if (!parse_int_text(text, value)) {
                    throw arg_error(std::format("argument {}: invalid int value: '{}'", name, text));
                }
                return value;
            };
            auto amount_arg = [](std::string_view name, std::string const& text) {
                try {
                    return parse_amount(text);
                } catch (error const& exc) {
                    throw arg_error(std::format("argument {}: {}", name, exc.what()));
                }
            };
            auto ratio_arg = [](std::string_view name, std::string const& text) {
                try {
                    return parse_ratio(text);
                } catch (error const& exc) {
                    throw arg_error(std::format("argument {}: {}", name, exc.what()));
                }
            };
            auto imbalance_arg = [](std::string_view name, std::string const& text) {
                try {
                    return resolve_imbalance(text);
                } catch (error const& exc) {
                    throw arg_error(std::format("argument {}: {}", name, exc.what()));
                }
            };

            while (index < argv.size()) {
                auto const& token = argv[index];
                if (token == "--") {
                    ++index;
                    if (index < argv.size()) {
                        std::vector<std::string> rest(argv.begin() + static_cast<std::ptrdiff_t>(index), argv.end());
                        throw arg_error(std::format("unrecognized arguments: {}", join(rest, " ")));
                    }
                    break;
                }
                if (token == "-h" || token == "--help") {
                    opts.help = true;
                    return opts;
                }
                if (!token.starts_with("--")) {
                    throw arg_error(std::format("unrecognized arguments: {}", token));
                }
                auto const                 equals = token.find('=');
                auto const                 name   = equals == std::string::npos ? token : token.substr(0, equals);
                std::optional<std::string> inline_value;
                if (equals != std::string::npos) {
                    inline_value = token.substr(equals + 1);
                }

                if (name == "--type") {
                    opts.type.push_back(value_of(name, inline_value));
                } else if (name == "--market") {
                    opts.market.push_back(value_of(name, inline_value));
                } else if (name == "--board") {
                    opts.board.push_back(value_of(name, inline_value));
                } else if (name == "--sector") {
                    opts.sector.push_back(value_of(name, inline_value));
                } else if (name == "--match") {
                    opts.match.push_back(value_of(name, inline_value));
                } else if (name == "--watchlist") {
                    opts.watchlist = value_of(name, inline_value);
                } else if (name == "--states") {
                    opts.states = value_of(name, inline_value);
                } else if (name == "--traded") {
                    reject_inline(name, inline_value);
                    opts.traded = true;
                } else if (name == "--untraded") {
                    reject_inline(name, inline_value);
                    opts.untraded = true;
                } else if (name == "--price-min") {
                    opts.price_min = amount_arg(name, value_of(name, inline_value));
                } else if (name == "--price-max") {
                    opts.price_max = amount_arg(name, value_of(name, inline_value));
                } else if (name == "--chg-min") {
                    opts.chg_min = ratio_arg(name, value_of(name, inline_value));
                } else if (name == "--chg-max") {
                    opts.chg_max = ratio_arg(name, value_of(name, inline_value));
                } else if (name == "--up") {
                    reject_inline(name, inline_value);
                    opts.up = true;
                } else if (name == "--down") {
                    reject_inline(name, inline_value);
                    opts.down = true;
                } else if (name == "--at-ceiling") {
                    reject_inline(name, inline_value);
                    opts.at_ceiling = true;
                } else if (name == "--at-floor") {
                    reject_inline(name, inline_value);
                    opts.at_floor = true;
                } else if (name == "--near-limit") {
                    opts.near_limit = ratio_arg(name, value_of(name, inline_value));
                } else if (name == "--pe-min") {
                    opts.pe_min = ratio_arg(name, value_of(name, inline_value));
                } else if (name == "--pe-max") {
                    opts.pe_max = ratio_arg(name, value_of(name, inline_value));
                } else if (name == "--eps-min") {
                    opts.eps_min = amount_arg(name, value_of(name, inline_value));
                } else if (name == "--eps-max") {
                    opts.eps_max = amount_arg(name, value_of(name, inline_value));
                } else if (name == "--profitable") {
                    reject_inline(name, inline_value);
                    opts.profitable = true;
                } else if (name == "--loss-making") {
                    reject_inline(name, inline_value);
                    opts.loss_making = true;
                } else if (name == "--mcap-min") {
                    opts.mcap_min = amount_arg(name, value_of(name, inline_value));
                } else if (name == "--mcap-max") {
                    opts.mcap_max = amount_arg(name, value_of(name, inline_value));
                } else if (name == "--ps-min") {
                    opts.ps_min = ratio_arg(name, value_of(name, inline_value));
                } else if (name == "--ps-max") {
                    opts.ps_max = ratio_arg(name, value_of(name, inline_value));
                } else if (name == "--sector-pe-min") {
                    opts.sector_pe_min = ratio_arg(name, value_of(name, inline_value));
                } else if (name == "--sector-pe-max") {
                    opts.sector_pe_max = ratio_arg(name, value_of(name, inline_value));
                } else if (name == "--near-52w-high") {
                    opts.near_52w_high = ratio_arg(name, value_of(name, inline_value));
                } else if (name == "--near-52w-low") {
                    opts.near_52w_low = ratio_arg(name, value_of(name, inline_value));
                } else if (name == "--vol-min") {
                    opts.vol_min = amount_arg(name, value_of(name, inline_value));
                } else if (name == "--vol-max") {
                    opts.vol_max = amount_arg(name, value_of(name, inline_value));
                } else if (name == "--val-min") {
                    opts.val_min = amount_arg(name, value_of(name, inline_value));
                } else if (name == "--val-max") {
                    opts.val_max = amount_arg(name, value_of(name, inline_value));
                } else if (name == "--trades-min") {
                    opts.trades_min = amount_arg(name, value_of(name, inline_value));
                } else if (name == "--trades-max") {
                    opts.trades_max = amount_arg(name, value_of(name, inline_value));
                } else if (name == "--spread-max") {
                    opts.spread_max = amount_arg(name, value_of(name, inline_value));
                } else if (name == "--spread-pct-max") {
                    opts.spread_pct_max = ratio_arg(name, value_of(name, inline_value));
                } else if (name == "--avg5d-min") {
                    opts.avg5d_min = amount_arg(name, value_of(name, inline_value));
                } else if (name == "--free-float-min") {
                    opts.free_float_min = ratio_arg(name, value_of(name, inline_value));
                } else if (name == "--vol-surge") {
                    opts.vol_surge = ratio_arg(name, value_of(name, inline_value));
                } else if (name == "--book") {
                    reject_inline(name, inline_value);
                    opts.book = true;
                } else if (name == "--bid-wall-min") {
                    opts.bid_wall_min = amount_arg(name, value_of(name, inline_value));
                } else if (name == "--ask-wall-min") {
                    opts.ask_wall_min = amount_arg(name, value_of(name, inline_value));
                } else if (name == "--imbalance") {
                    opts.imbalance = imbalance_arg(name, value_of(name, inline_value));
                } else if (name == "--rsi-min") {
                    opts.rsi_min = ratio_arg(name, value_of(name, inline_value));
                } else if (name == "--rsi-max") {
                    opts.rsi_max = ratio_arg(name, value_of(name, inline_value));
                } else if (name == "--above-sma") {
                    opts.above_sma = int_arg(name, value_of(name, inline_value));
                } else if (name == "--below-sma") {
                    opts.below_sma = int_arg(name, value_of(name, inline_value));
                } else if (name == "--cross") {
                    opts.cross = value_of(name, inline_value);
                } else if (name == "--cross-below") {
                    opts.cross_below = value_of(name, inline_value);
                } else if (name == "--macd") {
                    auto const value = value_of(name, inline_value);
                    if (value != "bull" && value != "bear") {
                        throw arg_error(
                          std::format("argument --macd: invalid choice: '{}' (choose from 'bull', 'bear')", value));
                    }
                    opts.macd = value;
                } else if (name == "--streak-min") {
                    opts.streak_min = int_arg(name, value_of(name, inline_value));
                } else if (name == "--streak-max") {
                    opts.streak_max = int_arg(name, value_of(name, inline_value));
                } else if (name == "--info") {
                    reject_inline(name, inline_value);
                    opts.info = true;
                } else if (name == "--history-days") {
                    opts.history_days = int_arg(name, value_of(name, inline_value));
                } else if (name == "--max-enrich") {
                    opts.max_enrich = int_arg(name, value_of(name, inline_value));
                } else if (name == "--jobs") {
                    opts.jobs = int_arg(name, value_of(name, inline_value));
                } else if (name == "--format") {
                    auto const value = value_of(name, inline_value);
                    if (value != "table" && value != "json" && value != "jsonl" && value != "csv" && value != "md" &&
                        value != "tse")
                    {
                        throw arg_error(std::format(
                          "argument --format: invalid choice: '{}' (choose from 'table', 'json', 'jsonl', 'csv', "
                          "'md', 'tse')",
                          value));
                    }
                    opts.format = value;
                } else if (name == "--sort") {
                    opts.sort = value_of(name, inline_value);
                } else if (name == "--asc") {
                    reject_inline(name, inline_value);
                    opts.asc = true;
                } else if (name == "--limit") {
                    opts.limit = int_arg(name, value_of(name, inline_value));
                } else if (name == "--columns") {
                    opts.columns = value_of(name, inline_value);
                } else if (name == "--stats") {
                    reject_inline(name, inline_value);
                    opts.stats = true;
                } else if (name == "--quiet") {
                    reject_inline(name, inline_value);
                    opts.quiet = true;
                } else {
                    throw arg_error(std::format("unrecognized arguments: {}", token));
                }
                ++index;
            }
            return opts;
        }

        [[nodiscard]] std::vector<std::string> strip_format_args(std::vector<std::string> const& argv) {
            std::vector<std::string> out;
            std::size_t              index = 0;
            while (index < argv.size()) {
                auto const& token = argv[index];
                if (token == "--format") {
                    index += 2;
                    continue;
                }
                if (token.starts_with("--format=")) {
                    ++index;
                    continue;
                }
                out.push_back(token);
                ++index;
            }
            return out;
        }

        [[nodiscard]] std::string py_str(json const& value) {
            if (value.is_string()) {
                return value.get<std::string>();
            }
            if (value.is_boolean()) {
                return value.get<bool>() ? "True" : "False";
            }
            if (value.is_null()) {
                return "None";
            }
            return value.dump();
        }

        [[nodiscard]] std::vector<std::string> load_args_file(std::string const& path) {
            std::ifstream input(path, std::ios::binary);
            if (!input) {
                throw error(std::format("{}: [Errno 2] No such file or directory", path));
            }
            std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            auto const  stripped = lstrip(text);
            if (!stripped.empty() && (stripped.front() == '{' || stripped.front() == '[')) {
                json payload;
                try {
                    payload = json::parse(text);
                } catch (json::parse_error const& exc) {
                    throw error(std::format("{}: invalid JSON: {}", path, exc.what()));
                }
                if (payload.is_object()) {
                    auto args = payload.contains("args") ? payload["args"] : json(nullptr);
                    if (args.is_null() && payload.contains("meta") && payload["meta"].is_object()) {
                        args = field_of(payload["meta"], "args");
                    }
                    if (!args.is_array()) {
                        throw error(std::format("{}: no embedded screen arguments (meta.args)", path));
                    }
                    std::vector<std::string> out;
                    out.reserve(args.size());
                    for (auto const& arg : args) {
                        out.push_back(py_str(arg));
                    }
                    return out;
                }
                if (payload.is_array()) {
                    throw error(std::format(
                      "{}: results-only snapshot embeds no screen arguments; pass the filter "
                      "flags explicitly",
                      path));
                }
                throw error(std::format("{}: expected a JSON object or array", path));
            }
            return shlex_split(text);
        }

        [[nodiscard]] std::pair<std::vector<std::string>, std::vector<std::string>> extract_file_args(
          std::vector<std::string> const& argv) {
            std::vector<std::string> file_args;
            std::vector<std::string> rest;
            std::size_t              index = 0;
            while (index < argv.size()) {
                auto const& token = argv[index];
                if (takes_value(token)) {
                    rest.push_back(token);
                    ++index;
                    if (index < argv.size()) {
                        rest.push_back(argv[index]);
                        ++index;
                    }
                    continue;
                }
                if (!token.starts_with('-') && (token.ends_with(".tse") || token.ends_with(".args"))) {
                    std::error_code error_code;
                    if (!std::filesystem::is_regular_file(token, error_code)) {
                        throw error(std::format("no such screen file: {}", token));
                    }
                    auto loaded = load_args_file(token);
                    file_args.insert(file_args.end(),
                                     std::make_move_iterator(loaded.begin()),
                                     std::make_move_iterator(loaded.end()));
                    ++index;
                    continue;
                }
                rest.push_back(token);
                ++index;
            }
            return {strip_format_args(file_args), rest};
        }

        [[nodiscard]] std::string help_text() {
            return R"EOF(usage: tse.find [options] [FILE]

Screen the whole Tehran Stock Exchange (TSETMC) against filters.

One bulk request returns every instrument with quotes, P/E, EPS, volume,
value, trade count, spread, price limits, sector/board codes and market cap;
filtering happens locally. Filters that need 52-week/average-volume data
(--ps-*, --sector-pe-*, --avg5d-*, --near-52w-*) or daily history (--rsi-*,
--above-sma, --cross, --macd, --streak-*, --vol-surge) enrich only the
survivors, in parallel, capped by --max-enrich; order-book filters
(--bid-wall-*, --ask-wall-*, --imbalance) use one extra bulk request carrying
5-level best limits.

universe:
  --type LIST           stock, fund, option, bond, right, future, mortgage,
                        forward, other, all (comma-separated/repeatable;
                        default: stock)
  --market LIST         borse, frabors, paye, moshtaghe, 6, 7 (or raw flow
                        codes; repeatable)
  --board TEXT          board title substring or code (repeatable)
  --sector TEXT         industry substring or code, e.g. 'فلزات' or 27
                        (repeatable)
  --match TEXT          symbol/company-name substring (repeatable)
  --watchlist FILE      restrict to symbols/ISINs/insCodes listed in FILE
  --traded              only instruments that traded today
  --untraded            only instruments with no trades today
  --states LIST         raw TSETMC state codes (e.g. NO,BY,OL,BK)

price and change:
  --price-min N         last/close price floor (rial)
  --price-max N         last/close price ceiling (rial)
  --chg-min P           minimum change vs previous close (%)
  --chg-max P           maximum change vs previous close (%)
  --up                  up on the day
  --down                down on the day
  --at-ceiling          locked at the daily upper limit
  --at-floor            locked at the daily lower limit
  --near-limit P        within P% of either daily price limit

valuation:
  --pe-min N            minimum P/E
  --pe-max N            maximum P/E (negative P/E = losses)
  --eps-min N           minimum EPS (rial)
  --eps-max N           maximum EPS (rial)
  --profitable          EPS > 0
  --loss-making         EPS < 0
  --mcap-min N          market-cap floor (shares x price, rial)
  --mcap-max N          market-cap ceiling (rial)
  --ps-min N            minimum P/S (needs instrument info fetch)
  --ps-max N            maximum P/S (needs instrument info fetch)
  --sector-pe-min N     minimum sector P/E (info fetch)
  --sector-pe-max N     maximum sector P/E (info fetch)
  --near-52w-high P     within P% of the 52-week high (info fetch)
  --near-52w-low P      within P% of the 52-week low (info fetch)

liquidity and size:
  --vol-min N           traded volume floor (shares)
  --vol-max N           traded volume ceiling (shares)
  --val-min N           traded value floor (rial)
  --val-max N           traded value ceiling (rial)
  --trades-min N        minimum number of trades
  --trades-max N        maximum number of trades
  --spread-max N        max bid/ask spread (rial, needs a quote)
  --spread-pct-max P    max bid/ask spread (%)
  --avg5d-min N         5-day average volume floor (info fetch)
  --free-float-min P    free-float % floor (info fetch)
  --vol-surge X         today's volume / 20-day average >= X (history fetch)

order book:
  --book                include 5-level best limits from a second bulk request
  --bid-wall-min N      largest bid level (shares); implies --book
  --ask-wall-min N      largest ask level (shares); implies --book
  --imbalance LOW[:HIGH]
                        bid/ask volume ratio; LOW alone means at least LOW,
                        LOW:HIGH is a range (implies --book)

technical (history fetch):
  --rsi-min N           minimum RSI(14)
  --rsi-max N           maximum RSI(14)
  --above-sma N         price above SMA(N) (e.g. 50, 200)
  --below-sma N         price below SMA(N)
  --cross FAST,SLOW     golden cross of SMA FAST over SLOW in the last 10
                        sessions
  --cross-below FAST,SLOW
                        death cross of SMA FAST under SLOW in the last 10
                        sessions
  --macd bull|bear      MACD(12,26,9) above/below its signal line
  --streak-min N        N or more consecutive up closes (negative: down)
  --streak-max N        N or fewer consecutive up closes (negative: down)

fetch control:
  --info                force instrument-info enrichment (52w range, P/S,
                        sector P/E)
  --history-days N      daily history window for indicators (default 260)
  --max-enrich N        max survivors to enrich per stage, most liquid first
                        (0 = unlimited, default 300)
  --jobs N              parallel enrichment requests (default 8)

output:
  --format FMT          table, json, jsonl, csv, md, tse (jsonl = one object
                        per line; tse = .tse snapshot embedding the screen
                        args)
  --sort KEY            sort key (column name or 'name'; default value)
  --asc                 sort ascending instead of descending
  --limit N             max rows (default 50 for table/md, 0 = all)
  --columns LIST        comma-separated column keys
  --stats               print a summary block to stderr
  --quiet               suppress the stderr match footer
  -h, --help            show this help message and exit

examples:
  tse.find --type stock --sector "فلزات" --val-min 10b
  tse.find --pe-max 8 --chg-min 0 --traded --limit 20
  tse.find --rsi-max 30 --above-sma 50 --type stock --sort rsi
  tse.find --near-limit 1 --up --format csv --limit 0
  tse.find --sector 34 --mcap-min 1t --book --bid-wall-min 1m --stats
  tse.find --watchlist ~/my-symbols.txt --macd bull --format jsonl --limit 0
  tse.find --traded --chg-min 1 --vol-min 1m --format tse > findings.tse
  tse.find findings.tse --limit 10          # re-run the snapshot's screen
  tse.find findings.tse --format jsonl | fzf --multi

amounts accept k/m/b/t/p suffixes (10b = 10,000,000,000 rial,
1t = 1,000,000,000,000); percentages are plain numbers (8 means 8%).
table/md output defaults to the first 50 matches, json/jsonl/tse/csv print
everything; --limit 0 disables the cap.
jsonl is one compact JSON object per line (fzf-friendly); tse writes a .tse
snapshot with meta.args embedded so this command can re-run it later.

FILE arguments ending in .tse or .args are loaded and their screen arguments
are applied first; flags given on the command line override them.)EOF";
        }

        [[nodiscard]] std::vector<json> rows_from_stdin(std::string const& text) {
            auto payload = json::parse(text, nullptr, false);
            if (payload.is_discarded()) {
                throw error("stdin: invalid JSON");
            }
            json list = nullptr;
            if (payload.is_array()) {
                list = std::move(payload);
            } else if (payload.is_object()) {
                for (auto const key : {"marketwatch", "instruments", "results"}) {
                    auto const it = payload.find(std::string(key));
                    if (it != payload.end() && it->is_array()) {
                        list = *it;
                        break;
                    }
                }
            }
            if (!list.is_array()) {
                throw error("stdin: expected a market-watch array");
            }
            std::vector<json> rows;
            rows.reserve(list.size());
            for (auto const& row : list) {
                if (row.is_object()) {
                    rows.push_back(row);
                }
            }
            if (rows.empty()) {
                throw error("Market watch returned no instruments");
            }
            return rows;
        }

        [[nodiscard]] find_output run_screen(
          options                         opts,
          std::vector<std::string> const& file_args,
          std::vector<std::string> const& rest,
          std::string const&              stdin_data) {
            if (opts.up && opts.down) {
                throw arg_error("--up and --down are mutually exclusive");
            }
            if (opts.profitable && opts.loss_making) {
                throw arg_error("--profitable and --loss-making are mutually exclusive");
            }
            if (opts.history_days < 30) {
                throw arg_error("--history-days must be at least 30");
            }
            if (opts.limit && *opts.limit < 0) {
                throw arg_error("--limit must be 0 or positive");
            }
            if (opts.type.empty()) {
                opts.type = {"stock"};
            }
            if (opts.jobs < 1) {
                throw arg_error("--jobs must be at least 1");
            }
            if (opts.max_enrich < 0) {
                throw arg_error("--max-enrich must be 0 (unlimited) or positive");
            }
            auto const cap =
              opts.max_enrich == 0 ? std::optional<long long>{} : std::optional<long long>{opts.max_enrich};

            std::vector<std::string> columns;
            if (opts.columns && !opts.columns->empty()) {
                columns = split_commas(*opts.columns);
            } else {
                columns = default_columns();
            }
            std::vector<std::string> unknown;
            for (auto const& key : columns) {
                if (!columns_table().contains(key)) {
                    unknown.push_back(key);
                }
            }
            if (!unknown.empty()) {
                std::sort(unknown.begin(), unknown.end());
                throw error(std::format("unknown --columns key(s): {}", join(unknown, ", ")));
            }

            std::vector<filter> base;
            std::vector<filter> info;
            std::vector<filter> hist;
            std::vector<filter> book;
            build_filters(opts, base, info, hist, book);

            bool need_info = !info.empty() || opts.info;
            bool need_hist = !hist.empty();
            bool need_book =
              !book.empty() || opts.book || opts.bid_wall_min.has_value() || opts.ask_wall_min.has_value();
            for (auto const& key : columns) {
                if (info_keys().contains(key)) {
                    need_info = true;
                }
                if (hist_keys().contains(key)) {
                    need_hist = true;
                }
                if (book_keys().contains(key)) {
                    need_book = true;
                }
            }

            std::set<long long> requested_sma;
            if (opts.above_sma && *opts.above_sma != 0) {
                requested_sma.insert(*opts.above_sma);
            }
            if (opts.below_sma && *opts.below_sma != 0) {
                requested_sma.insert(*opts.below_sma);
            }
            for (auto const& key : columns) {
                if (key.size() > 3 && key.starts_with("sma") && all_digits(std::string_view(key).substr(3))) {
                    requested_sma.insert(std::stoll(key.substr(3)));
                }
            }
            std::set<long long> sma_periods{20, 50, 200};
            sma_periods.insert(requested_sma.begin(), requested_sma.end());
            std::vector<long long> const sma_list(sma_periods.begin(), sma_periods.end());

            std::vector<cross_spec> cross_specs;
            auto const              add_cross_spec =
              [&cross_specs](std::optional<std::string> const& spec, std::string_view direction) {
                  if (!spec || spec->empty()) {
                      return;
                  }
                  auto const parts = split_on_any(*spec, ", \t\n\r\f\v");
                  if (parts.size() != 2) {
                      return;
                  }
                  long long fast = 0;
                  long long slow = 0;
                  if (!parse_int_text(parts[0], fast) || !parse_int_text(parts[1], slow)) {
                      return;
                  }
                  if (fast < slow) {
                      cross_specs.push_back(cross_spec{fast, slow, std::string(direction)});
                  }
              };
            add_cross_spec(opts.cross, "bull");
            add_cross_spec(opts.cross_below, "bear");

            long long needed_bars = 0;
            for (auto const period : requested_sma) {
                needed_bars = std::max(needed_bars, period);
            }
            for (auto const& spec : cross_specs) {
                needed_bars = std::max(needed_bars, spec.slow);
            }
            if (needed_bars > opts.history_days) {
                throw arg_error(
                  std::format("--history-days must be at least {} for the requested moving averages/crosses",
                              needed_bars));
            }

            client            http;
            std::vector<json> rows =
              stdin_data.empty() ? fetch_market_watch(http, need_book) : rows_from_stdin(stdin_data);
            std::vector<record> records;
            records.reserve(rows.size());
            for (auto const& row : rows) {
                records.push_back(parse_row(row));
            }
            auto const               total = records.size();
            std::vector<std::string> notes;
            std::string              err;

            apply_filters(records, base, "filters", notes);

            if (need_info && !records.empty()) {
                auto const counts = enrich(
                  records,
                  [&http](record& item) {
                      fetch_info(http, item);
                  },
                  opts.jobs,
                  cap,
                  "instrument info",
                  err);
                if (!info.empty()) {
                    std::vector<record> kept;
                    kept.reserve(records.size());
                    for (auto& item : records) {
                        bool const present =
                          item.data.contains("sector_pe") || item.data.contains("psr") || item.data.contains("avg5d") ||
                          item.data.contains("y52h") || item.data.contains("year_high");
                        if (present) {
                            kept.push_back(std::move(item));
                        }
                    }
                    records = std::move(kept);
                    apply_filters(records, info, "info filters", notes);
                }
                if (counts.skipped != 0) {
                    notes.push_back(
                      std::format("{} least-liquid survivors had no info fetch (--max-enrich)", counts.skipped));
                }
            }

            if (need_hist && !records.empty()) {
                auto const counts = enrich(
                  records,
                  [&http, days = opts.history_days, &cross_specs, &sma_list](record& item) {
                      fetch_history(http, item, days, cross_specs, sma_list);
                  },
                  opts.jobs,
                  cap,
                  "history",
                  err);
                apply_filters(records, hist, "history filters", notes);
                if (counts.skipped != 0) {
                    notes.push_back(
                      std::format("{} least-liquid survivors had no history fetch (--max-enrich)", counts.skipped));
                }
            }

            if (need_book && !records.empty()) {
                for (auto& item : records) {
                    fetch_book(item);
                }
                apply_filters(records, book, "book filters", notes);
            }

            if (!opts.columns || opts.columns->empty()) {
                std::vector<std::string> extra;
                if (need_hist) {
                    extra.insert(extra.end(), {"rsi", "macd", "sma50"});
                }
                if (need_info) {
                    extra.insert(extra.end(), {"sector_pe", "psr"});
                }
                if (need_book) {
                    extra.insert(extra.end(), {"bid_wall", "ask_wall", "imbalance"});
                }
                for (auto const& key : extra) {
                    if (
                      columns_table().contains(key) && std::find(columns.begin(), columns.end(), key) == columns.end())
                    {
                        columns.push_back(key);
                    }
                }
            }

            sort_records(records, opts.sort, opts.asc);

            long long limit = 0;
            if (opts.limit) {
                limit = *opts.limit;
            } else if (opts.format == "table" || opts.format == "md") {
                limit = 50;
            }
            if (limit > 0 && records.size() > static_cast<std::size_t>(limit)) {
                records.resize(static_cast<std::size_t>(limit));
            }

            std::vector<json> payload;
            payload.reserve(records.size());
            for (auto const& item : records) {
                auto copy = item.data;
                copy.erase("_near_limit");
                payload.push_back(std::move(copy));
            }

            std::string out;
            if (opts.format == "json") {
                out = py_dump(json(payload), true) + "\n";
            } else if (opts.format == "jsonl") {
                for (std::size_t index = 0; index < payload.size(); ++index) {
                    if (index != 0) {
                        out += '\n';
                    }
                    out += py_dump(payload[index], false);
                }
                if (!payload.empty()) {
                    out += '\n';
                }
            } else if (opts.format == "tse") {
                std::vector<std::string> screen_args = file_args;
                screen_args.insert(screen_args.end(), rest.begin(), rest.end());
                screen_args            = strip_format_args(screen_args);
                json meta              = json::object();
                meta["version"]        = 1;
                meta["screened_at"]    = tehran_iso_now();
                meta["command"]        = "tse.find " + shlex_join(screen_args);
                meta["args"]           = screen_args;
                meta["total_universe"] = total;
                meta["matched"]        = payload.size();
                json document          = json::object();
                document["meta"]       = std::move(meta);
                document["results"]    = json(payload);
                out                    = py_dump(document, true) + "\n";
            } else if (opts.format == "csv") {
                out = render_csv(records, columns) + "\n";
            } else if (opts.format == "md") {
                out = render_md(records, columns) + "\n";
            } else {
                out = (records.empty() ? std::string("no matches") : render_table(records, columns)) + "\n";
            }

            if (opts.stats) {
                err += stats_summary(total, records, notes) + "\n";
            } else if (!opts.quiet) {
                err += std::format("matched {} / {} instruments\n", records.size(), total);
            }
            return {std::move(out), std::move(err), 0};
        }

    } // namespace

    find_output find_run_full(std::vector<std::string> const& args, std::string const& stdin_data) {
        try {
            auto const [file_args, rest]    = extract_file_args(args);
            std::vector<std::string> merged = file_args;
            merged.insert(merged.end(), rest.begin(), rest.end());
            auto opts = parse_args(merged);
            if (opts.help) {
                return {help_text() + "\n", {}, 0};
            }
            return run_screen(std::move(opts), file_args, rest, stdin_data);
        } catch (arg_error const& exc) {
            return {"", std::format("tse.find: error: {}\n", exc.what()), 2};
        } catch (std::exception const& exc) {
            return {"", std::format("tse.find: {}\n", exc.what()), 1};
        }
    }

    std::string find_run(std::vector<std::string> const& args, std::string const& stdin_data) {
        auto result = find_run_full(args, stdin_data);
        if (result.code != 0) {
            auto message = result.err;
            while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
                message.pop_back();
            }
            throw error(message);
        }
        return result.out;
    }

} // namespace dana::tools::tse
