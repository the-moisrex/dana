#include "tse_common.hpp"

#include "../ai/http.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <format>
#include <limits>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace dana::tools::tse {

    namespace {

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

        [[nodiscard]] std::string trim_copy(std::string_view text) {
            return std::string(trim_view(text));
        }

        [[nodiscard]] bool ws_at(std::string_view text, std::size_t pos, std::size_t& len) noexcept {
            auto const c = static_cast<unsigned char>(text[pos]);
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
                len = 1;
                return true;
            }
            if (c == 0xC2 && pos + 1 < text.size() && static_cast<unsigned char>(text[pos + 1]) == 0xA0) {
                len = 2;
                return true;
            }
            if (c == 0xE2 && pos + 2 < text.size()) {
                auto const d = static_cast<unsigned char>(text[pos + 1]);
                auto const e = static_cast<unsigned char>(text[pos + 2]);
                if (d == 0x80 && ((e >= 0x80 && e <= 0x8A) || e == 0xAF || e == 0xA8 || e == 0xA9)) {
                    len = 3;
                    return true;
                }
                if (d == 0x81 && e == 0x9F) {
                    len = 3;
                    return true;
                }
            }
            if (c == 0xE3 && pos + 2 < text.size() && static_cast<unsigned char>(text[pos + 1]) == 0x80 &&
                static_cast<unsigned char>(text[pos + 2]) == 0x80)
            {
                len = 3;
                return true;
            }
            return false;
        }

        [[nodiscard]] std::string strip_unicode_ws(std::string const& text) {
            std::size_t start = 0;
            while (start < text.size()) {
                std::size_t len = 0;
                if (!ws_at(text, start, len)) {
                    break;
                }
                start += len;
            }
            std::size_t end   = start;
            std::size_t index = start;
            while (index < text.size()) {
                std::size_t len = 0;
                if (ws_at(text, index, len)) {
                    index += len;
                } else {
                    end = ++index;
                }
            }
            return text.substr(start, end - start);
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

        [[nodiscard]] std::string host_of(std::string_view url) {
            auto const scheme = url.find("://");
            auto       rest   = scheme == std::string_view::npos ? url : url.substr(scheme + 3);
            auto const slash  = rest.find('/');
            if (slash != std::string_view::npos) {
                rest = rest.substr(0, slash);
            }
            auto const at = rest.rfind('@');
            if (at != std::string_view::npos) {
                rest = rest.substr(at + 1);
            }
            auto const colon = rest.find(':');
            if (colon != std::string_view::npos) {
                rest = rest.substr(0, colon);
            }
            return std::string(rest);
        }

        [[nodiscard]] std::tm tehran_tm(std::int64_t unix_seconds) noexcept {
            std::time_t const shifted = static_cast<std::time_t>(unix_seconds + 3LL * 3600 + 30LL * 60);
            std::tm           out{};
            ::gmtime_r(&shifted, &out);
            return out;
        }

        [[nodiscard]] std::int64_t days_from_civil(int year, int month, int day) noexcept {
            year                -= month <= 2 ? 1 : 0;
            long long const era  = (year >= 0 ? year : year - 399) / 400;
            unsigned const  yoe  = static_cast<unsigned>(year - era * 400);
            unsigned const  doy  = static_cast<unsigned>((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
            unsigned const  doe  = yoe * 365 + yoe / 4 - yoe / 100 + doy;
            return static_cast<std::int64_t>(era) * 146'097 + static_cast<std::int64_t>(doe) - 719'468;
        }

        [[nodiscard]] bool parse_int(std::string_view text, long long& out) noexcept {
            if (text.empty()) {
                return false;
            }
            auto const* first  = text.data();
            auto const* last   = text.data() + text.size();
            auto const  result = std::from_chars(first, last, out);
            return result.ec == std::errc{} && result.ptr == last;
        }

        [[nodiscard]] bool parse_double(std::string_view text, double& out) noexcept {
            if (text.empty()) {
                return false;
            }
            auto const* first  = text.data();
            auto const* last   = text.data() + text.size();
            auto const  result = std::from_chars(first, last, out);
            return result.ec == std::errc{} && result.ptr == last;
        }

        [[nodiscard]] std::string marketwatch_path(bool with_book = false) {
            std::string path = std::format("ClosingPrice/GetMarketWatch?market=0&withBestLimits={}&hEven=0&RefID=0",
                                           with_book ? "true" : "false");
            for (int i = 0; i < 9; ++i) {
                path += std::format("&paperTypes%5B%5D={}", i + 1);
            }
            return path;
        }

        [[nodiscard]] bool book_is_real(json const& levels) {
            if (!levels.is_array()) {
                return false;
            }
            for (auto const& level : levels) {
                if (!level.is_object()) {
                    continue;
                }
                if (!pick(level, {"qmd", "qmo", "pmd", "pmo"}).is_null()) {
                    return true;
                }
            }
            return false;
        }

    } // namespace

    // ------------------------------------------------------------------ values

    std::string normal(std::string_view text) {
        std::string out;
        out.reserve(text.size());
        for (std::size_t i = 0; i < text.size();) {
            auto const c = static_cast<unsigned char>(text[i]);
            if (c == 0xDB && i + 1 < text.size()) {
                auto const d = static_cast<unsigned char>(text[i + 1]);
                if (d >= 0xB0 && d <= 0xB9) {
                    out.push_back(static_cast<char>('0' + (d - 0xB0)));
                    i += 2;
                    continue;
                }
            }
            if (c == 0xD9 && i + 1 < text.size()) {
                auto const d = static_cast<unsigned char>(text[i + 1]);
                if (d >= 0xA0 && d <= 0xA9) {
                    out.push_back(static_cast<char>('0' + (d - 0xA0)));
                    i += 2;
                    continue;
                }
                if (d == 0x8A) {
                    out += "\xDB\x8C";
                    i   += 2;
                    continue;
                }
                if (d == 0x83) {
                    out += "\xDA\xA9";
                    i   += 2;
                    continue;
                }
            }
            if (c == 0xE2 && i + 2 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0x80 &&
                static_cast<unsigned char>(text[i + 2]) == 0x8C)
            {
                out.push_back(' ');
                i += 3;
                continue;
            }
            out.push_back(text[i]);
            ++i;
        }
        return strip_unicode_ws(out);
    }

    json pick(json const& row, std::initializer_list<std::string_view> names) {
        if (!row.is_object()) {
            return nullptr;
        }
        for (auto const name : names) {
            auto const wanted = lower_ascii(name);
            for (auto it = row.begin(); it != row.end(); ++it) {
                if (lower_ascii(it.key()) != wanted) {
                    continue;
                }
                if (!it.value().is_null() && !(it.value().is_string() && it.value().get<std::string>().empty())) {
                    return it.value();
                }
            }
        }
        return nullptr;
    }

    json number(json const& value) {
        if (value.is_null() || value.is_boolean()) {
            return nullptr;
        }
        if (value.is_number_integer()) {
            return value;
        }
        if (value.is_number_unsigned()) {
            return value.get<unsigned long long>() <=
                       static_cast<unsigned long long>(std::numeric_limits<long long>::max())
                     ? json(value.get<long long>())
                     : json(value.get<unsigned long long>());
        }
        if (value.is_number_float()) {
            double const d = value.get<double>();
            if (std::isfinite(d) && std::floor(d) == d && std::abs(d) < 9.2e18) {
                return json(static_cast<long long>(d));
            }
            return value;
        }
        if (!value.is_string()) {
            return nullptr;
        }
        std::string text = normal(value.get<std::string>());
        std::string cleaned;
        cleaned.reserve(text.size());
        for (char const c : text) {
            if (c == ',' || c == ' ') {
                continue;
            }
            cleaned.push_back(c);
        }
        // U+066C / U+066B Arabic thousands/decimal separators.
        std::string stripped;
        stripped.reserve(cleaned.size());
        for (std::size_t i = 0; i < cleaned.size();) {
            if (static_cast<unsigned char>(cleaned[i]) == 0xD9 && i + 1 < cleaned.size() &&
                static_cast<unsigned char>(cleaned[i + 1]) == 0xAC)
            {
                i += 2;
                continue;
            }
            stripped.push_back(cleaned[i]);
            ++i;
        }
        if (stripped.size() >= 2 && stripped.front() == '(' && stripped.back() == ')') {
            stripped = "-" + stripped.substr(1, stripped.size() - 2);
        }
        if (!stripped.empty() && stripped.front() == '+') {
            stripped.erase(stripped.begin());
        }
        if (stripped.find('.') != std::string::npos) {
            double parsed = 0.0;
            if (!parse_double(stripped, parsed)) {
                return nullptr;
            }
            return parsed;
        }
        long long parsed = 0;
        if (!parse_int(stripped, parsed)) {
            return nullptr;
        }
        return parsed;
    }

    json price(json const& value) {
        auto parsed = number(value);
        if (parsed.is_null()) {
            return nullptr;
        }
        if (parsed.is_number_integer() && parsed.get<long long>() == 0) {
            return nullptr;
        }
        if (parsed.is_number_float() && parsed.get<double>() == 0.0) {
            return nullptr;
        }
        return parsed;
    }

    json value_of(json const& row, std::initializer_list<std::string_view> names) {
        return number(pick(row, names));
    }

    json unwrap(json const& payload, std::string_view key) {
        if (!payload.is_object()) {
            throw error(std::format("Unexpected API response for {}", key));
        }
        auto result = pick(payload, {key});
        if (result.is_null() && payload.size() == 1) {
            result = payload.begin().value();
        }
        return result;
    }

    std::optional<double> as_double(json const& value) {
        if (value.is_number_integer()) {
            return static_cast<double>(value.get<long long>());
        }
        if (value.is_number_unsigned()) {
            return static_cast<double>(value.get<unsigned long long>());
        }
        if (value.is_number_float()) {
            return value.get<double>();
        }
        return std::nullopt;
    }

    std::optional<long long> as_int(json const& value) {
        if (value.is_number_integer()) {
            return value.get<long long>();
        }
        if (value.is_number_unsigned()) {
            return static_cast<long long>(value.get<unsigned long long>());
        }
        if (value.is_number_float()) {
            return static_cast<long long>(value.get<double>());
        }
        return std::nullopt;
    }

    json jnum(std::optional<double> value) {
        return value ? json(*value) : json(nullptr);
    }

    json jint(std::optional<long long> value) {
        return value ? json(*value) : json(nullptr);
    }

    json jint(long long value) {
        return json(value);
    }

    json jnum(double value) {
        return json(value);
    }

    double round_to(double value, int digits) {
        double const factor = std::pow(10.0, digits);
        return std::nearbyint(value * factor) / factor;
    }

    std::string scalar_text(json const& value) {
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

    // ------------------------------------------------------------------ calendar

    jdate gregorian_to_jalali(int gy, int gm, int gd) noexcept {
        static constexpr std::array<int, 12> month_days{0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
        int const                            gy2 = gm > 2 ? gy + 1 : gy;
        long long days  = 355'666 + 365LL * gy + (gy2 + 3) / 4 - (gy2 + 99) / 100 + (gy2 + 399) / 400 + gd +
                          month_days[static_cast<std::size_t>(gm - 1)];
        long long jy    = -1595 + 33 * (days / 12'053);
        days           %= 12'053;
        jy             += 4 * (days / 1461);
        days           %= 1461;
        if (days > 365) {
            jy   += (days - 1) / 365;
            days  = (days - 1) % 365;
        }
        int jm = 0;
        int jd = 0;
        if (days < 186) {
            jm = static_cast<int>(1 + days / 31);
            jd = static_cast<int>(1 + days % 31);
        } else {
            jm = static_cast<int>(7 + (days - 186) / 30);
            jd = static_cast<int>(1 + (days - 186) % 30);
        }
        return {static_cast<int>(jy), jm, jd};
    }

    gdate jalali_to_gregorian(int jy, int jm, int jd) noexcept {
        jy             += 1595;
        long long days  = -355'668 + 365LL * jy + (jy / 33) * 8LL + ((jy % 33) + 3) / 4 + jd;
        days           += jm < 7 ? (jm - 1) * 31LL : (jm - 7) * 30LL + 186LL;
        long long gy    = 400 * (days / 146'097);
        days           %= 146'097;
        if (days > 36'524) {
            days -= 1;
            gy   += 100 * (days / 36'524);
            days %= 36'524;
            if (days >= 365) {
                days += 1;
            }
        }
        gy   += 4 * (days / 1461);
        days %= 1461;
        if (days > 365) {
            gy   += (days - 1) / 365;
            days  = (days - 1) % 365;
        }
        int                                  gd   = static_cast<int>(days + 1);
        bool const                           leap = (gy % 4 == 0 && gy % 100 != 0) || gy % 400 == 0;
        static constexpr std::array<int, 13> month_days{0, 31, 0, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        int                                  gm = 0;
        while (gm < 13) {
            int limit = month_days[static_cast<std::size_t>(gm)];
            if (gm == 2) {
                limit = leap ? 29 : 28;
            }
            if (gd <= limit) {
                break;
            }
            gd -= limit;
            ++gm;
        }
        return {static_cast<int>(gy), gm, gd};
    }

    std::string jalali_day(std::int64_t unix_seconds) {
        auto const tm = tehran_tm(unix_seconds);
        auto const j  = gregorian_to_jalali(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        return std::format("{}{:02d}{:02d}", j.year, j.month, j.day);
    }

    std::string jalali_stamp(std::int64_t unix_seconds) {
        auto const tm = tehran_tm(unix_seconds);
        auto const j  = gregorian_to_jalali(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        return std::format(
          "{}/{:02d}/{:02d} {:02d}:{:02d}:{:02d}",
          j.year,
          j.month,
          j.day,
          tm.tm_hour,
          tm.tm_min,
          tm.tm_sec);
    }

    std::string trade_date(json const& value) {
        auto const              text = normal(scalar_text(value));
        static std::regex const re(R"(^(\d{4})-?(\d{2})-?(\d{2})$)");
        std::smatch             match;
        if (std::regex_match(text, match, re)) {
            auto const j =
              gregorian_to_jalali(std::stoi(match[1].str()), std::stoi(match[2].str()), std::stoi(match[3].str()));
            return std::format("{}/{:02d}/{:02d}", j.year, j.month, j.day);
        }
        return text.empty() ? std::string{} : text;
    }

    std::string trade_time(json const& value) {
        auto const text = normal(scalar_text(value));
        if (text.empty()) {
            return {};
        }
        bool const digits = std::all_of(text.begin(), text.end(), [](char c) {
            return c >= '0' && c <= '9';
        });
        if (!digits || text.size() > 6) {
            return {};
        }
        long long parsed = 0;
        if (!parse_int(text, parsed) || parsed == 0) {
            return {};
        }
        auto const padded = std::format("{:06d}", parsed);
        return std::format("{}:{}:{}", padded.substr(0, 2), padded.substr(2, 2), padded.substr(4, 2));
    }

    std::optional<std::int64_t> parse_moment(std::string_view text) {
        auto const trimmed = trim_view(text);
        if (trimmed.empty()) {
            return std::nullopt;
        }
        static std::regex const jalali_re(R"(^(\d{4})/(\d{2})/(\d{2})[ T](\d{2}):(\d{2}):(\d{2})$)");
        std::string             owned(trimmed);
        std::smatch             match;
        if (std::regex_match(owned, match, jalali_re)) {
            auto const g =
              jalali_to_gregorian(std::stoi(match[1].str()), std::stoi(match[2].str()), std::stoi(match[3].str()));
            auto const seconds = days_from_civil(g.year, g.month, g.day) * 86'400 + std::stoi(match[4].str()) * 3600LL +
                                 std::stoi(match[5].str()) * 60LL + std::stoi(match[6].str());
            return seconds - (3LL * 3600 + 30LL * 60);
        }
        static std::regex const iso_re(
          R"(^(\d{4})-(\d{2})-(\d{2})[T ](\d{2}):(\d{2})(?::(\d{2}))?(?:\.\d+)?(Z|[+-]\d{2}:?\d{2})?$)");
        if (!std::regex_match(owned, match, iso_re)) {
            return std::nullopt;
        }
        auto const seconds =
          days_from_civil(std::stoi(match[1].str()), std::stoi(match[2].str()), std::stoi(match[3].str())) * 86'400 +
          std::stoi(match[4].str()) * 3600LL + std::stoi(match[5].str()) * 60LL +
          (match[6].matched ? std::stoi(match[6].str()) : 0);
        auto const zone = match[7].str();
        if (zone.empty()) {
            return seconds - (3LL * 3600 + 30LL * 60);
        }
        if (zone == "Z" || zone == "z") {
            return seconds;
        }
        int         sign    = zone[0] == '-' ? -1 : 1;
        int         hours   = 0;
        int         minutes = 0;
        std::string digits;
        for (std::size_t i = 1; i < zone.size(); ++i) {
            if (zone[i] != ':') {
                digits.push_back(zone[i]);
            }
        }
        if (digits.size() >= 4) {
            hours   = std::stoi(digits.substr(0, 2));
            minutes = std::stoi(digits.substr(2, 2));
        }
        return seconds - sign * (hours * 3600LL + minutes * 60LL);
    }

    std::int64_t tehran_now() noexcept {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
    }

    bool market_session_open(std::int64_t unix_seconds) noexcept {
        auto const tm      = tehran_tm(unix_seconds);
        int const  wd      = tm.tm_wday;                                          // 0=Sunday .. 6=Saturday
        bool const day_ok  = wd == 6 || wd == 0 || wd == 1 || wd == 2 || wd == 3; // Sat..Wed
        int const  minutes = tm.tm_hour * 60 + tm.tm_min;
        return day_ok && minutes >= 9 * 60 && minutes < 12 * 60 + 30;
    }

    // ------------------------------------------------------------------ http

    std::string client::get(std::string const& url, std::string_view accept, std::size_t max_bytes) {
        ai::http_request request;
        request.url     = url;
        request.headers = {
          {"User-Agent", std::string(user_agent)},
          {    "Accept",     std::string(accept)}
        };
        request.timeout_seconds = 30;
        request.retries         = 2;
        try {
            auto const response = ai::perform(request);
            if (response.body.size() > max_bytes) {
                throw error(std::format("Response too large: {}", url));
            }
            auto const host = host_of(url);
            if (response.body.find("تأیید کاربر") != std::string::npos ||
                response.body.find("تایید کاربر") != std::string::npos)
            {
                throw error(std::format("{} requires browser verification; try again later", host));
            }
            if (response.body.find("دسترسی شما") != std::string::npos ||
                response.body.find("مسدود") != std::string::npos ||
                response.body.find("General Error Detected") != std::string::npos)
            {
                throw error(std::format("Access blocked by {}; try an Iranian IP without a VPN", host));
            }
            return response.body;
        } catch (ai::http_error const& exc) {
            auto const host = host_of(url);
            if (exc.status() == 429) {
                throw error(std::format("Rate limited by {} (HTTP 429); wait a few minutes and rerun", host));
            }
            if (exc.status() == 403) {
                throw error(std::format("Access denied by {} (HTTP 403); check your Iranian IP and VPN", host));
            }
            if (exc.status() >= 400) {
                throw error(std::format("{}: HTTP {}", host, exc.status()));
            }
            throw error(std::format("Cannot reach {}: {}", host, exc.what()));
        }
    }

    json client::json_get(std::string const& url, std::size_t max_bytes) {
        auto const text = get(url, "application/json", max_bytes);
        auto       data = json::parse(text, nullptr, false);
        if (data.is_discarded()) {
            throw error(std::format("Non-JSON response from {} (possibly blocked)", host_of(url)));
        }
        return data;
    }

    json client::api(std::string_view path, std::string_view key) {
        return unwrap(json_get(std::format("{}/{}", tsetmc_base, path)), key);
    }

    // ------------------------------------------------------------------ identifiers

    bool is_isin(std::string_view text) noexcept {
        if (text.size() != 12 || !text.starts_with("IR")) {
            return false;
        }
        return std::all_of(text.begin() + 2, text.end(), [](char c) {
            return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        });
    }

    std::string normalize_input(std::string_view symbol) {
        auto text = trim_copy(symbol);
        if (text.empty()) {
            return text;
        }
        static std::regex const easytrader(
          R"(easytrader\.ir/(?:easy-chart|stock-details)/([A-Za-z]{2}[A-Za-z0-9]{10}))");
        static std::regex const tsetmc(R"(tsetmc\.com/instInfo/(\d{15,}))");
        static std::regex const codal(R"(codal\.ir/ReportList\.aspx\?[^#]*?\bSymbol=([^&#]+))", std::regex::icase);
        static std::regex const bare(R"(^[A-Za-z]{2}[A-Za-z0-9]{10}$)");
        static std::regex const embedded_isin(R"(IR[A-Z0-9]{10})");
        static std::regex const embedded_code(R"(([0-9]{12,}))");
        std::smatch             match;
        if (std::regex_search(text, match, easytrader)) {
            auto out = match[1].str();
            for (char& c : out) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            return out;
        }
        if (std::regex_search(text, match, tsetmc)) {
            return match[1].str();
        }
        if (std::regex_search(text, match, codal)) {
            return url_decode_plus(match[1].str());
        }
        if (std::regex_match(text, bare)) {
            auto out = text;
            for (char& c : out) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            return out;
        }
        if (std::regex_search(text, match, embedded_isin)) {
            return match[0].str();
        }
        if (std::regex_search(text, match, embedded_code)) {
            return match[1].str();
        }
        return text;
    }

    json resolve_isin(client& http, std::string const& isin) {
        auto const rows = unwrap(http.json_get(std::format("{}/{}", tsetmc_base, marketwatch_path())), "marketwatch");
        if (!rows.is_array()) {
            throw error("Unexpected market watch result");
        }
        for (auto const& row : rows) {
            if (!row.is_object()) {
                continue;
            }
            auto const candidate = normal(scalar_text(pick(row, {"insID", "instrumentID"})));
            auto       upper     = candidate;
            for (char& c : upper) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            if (upper != isin) {
                continue;
            }
            auto const ins_code = scalar_text(pick(row, {"insCode"}));
            if (ins_code.empty()) {
                break;
            }
            return json{
              {"ins_code", ins_code},
              {"symbol", normal(scalar_text(pick(row, {"lva", "lVal18AFC"})))},
              {"name", normal(scalar_text(pick(row, {"lvc", "lVal30"})))},
              {"isin", isin},
              {"flow", nullptr},
              {"market", nullptr}
            };
        }
        return nullptr;
    }

    std::vector<json> search(client& http, std::string_view query) {
        auto const normalized = normalize_input(query);
        if (normalized.size() == 12 && is_isin(normalized)) {
            auto found = resolve_isin(http, normalized);
            if (!found.is_null()) {
                return {std::move(found)};
            }
            return {};
        }
        auto const rows =
          http.api(std::format("Instrument/GetInstrumentSearch/{}", url_encode(normalized)), "instrumentSearch");
        if (!rows.is_array()) {
            throw error("Unexpected instrument search result");
        }
        std::vector<json> out;
        for (auto const& row : rows) {
            if (!row.is_object() || pick(row, {"insCode"}).is_null()) {
                continue;
            }
            out.push_back(json{
              {"ins_code",        scalar_text(pick(row,              {"insCode"}))},
              {  "symbol", normal(scalar_text(pick(row, {"lVal18AFC", "lVal18"})))},
              {    "name", normal(scalar_text(pick(row,              {"lVal30"})))},
              {    "isin",                    pick(row,                 {"cIsin"})},
              {    "flow",                value_of(row,                  {"flow"})},
              {  "market",                    pick(row,             {"flowTitle"})}
            });
        }
        return out;
    }

    std::vector<json> fetch_market_watch(client& http, bool with_book) {
        std::vector<json>                            merged;
        std::unordered_map<std::string, std::size_t> index;
        for (int attempt = 0; attempt < 6; ++attempt) {
            auto const rows = http.api(marketwatch_path(with_book), "marketwatch");
            if (!rows.is_array() || rows.empty()) {
                throw error("Unexpected market watch result");
            }
            for (auto const& row : rows) {
                if (!row.is_object()) {
                    continue;
                }
                auto const code = scalar_text(pick(row, {"insCode"}));
                if (code.empty()) {
                    continue;
                }
                auto const it = index.find(code);
                if (it == index.end()) {
                    index.emplace(code, merged.size());
                    merged.push_back(row);
                    continue;
                }
                auto& current = merged[it->second];
                for (auto field = row.begin(); field != row.end(); ++field) {
                    auto const existing = current.find(field.key());
                    bool const missing =
                      existing == current.end() || existing->is_null() ||
                      (existing->is_string() && existing->get<std::string>().empty()) ||
                      (existing->is_number() && existing->get<double>() == 0.0) ||
                      (existing->is_array() && existing->empty());
                    if (missing) {
                        current[field.key()] = field.value();
                    }
                }
                if (book_is_real(pick(row, {"blDs"})) && !book_is_real(pick(current, {"blDs"}))) {
                    current["blDs"] = pick(row, {"blDs"});
                }
            }
            std::size_t kinds = 0;
            for (auto const& row : merged) {
                if (!pick(row, {"yVal"}).is_null()) {
                    ++kinds;
                }
            }
            bool const have_kinds = static_cast<double>(kinds) >= static_cast<double>(merged.size()) * 0.98;
            bool       have_book  = true;
            if (with_book) {
                have_book = false;
                for (auto const& row : merged) {
                    if (book_is_real(pick(row, {"blDs"}))) {
                        have_book = true;
                        break;
                    }
                }
            }
            if (have_kinds && have_book) {
                break;
            }
        }
        if (merged.empty()) {
            throw error("Market watch returned no instruments");
        }
        return merged;
    }

    std::string url_encode(std::string_view text) {
        static constexpr char hex[] = "0123456789ABCDEF";
        std::string           out;
        out.reserve(text.size());
        for (unsigned char const c : text) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                c == '.' || c == '~')
            {
                out.push_back(static_cast<char>(c));
            } else {
                out.push_back('%');
                out.push_back(hex[c >> 4U]);
                out.push_back(hex[c & 0x0FU]);
            }
        }
        return out;
    }

    std::string url_decode_plus(std::string_view text) {
        std::string out;
        out.reserve(text.size());
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '+') {
                out.push_back(' ');
            } else if (text[i] == '%' && i + 2 < text.size()) {
                auto const hexval = [](char c) -> int {
                    if (c >= '0' && c <= '9') {
                        return c - '0';
                    }
                    if (c >= 'a' && c <= 'f') {
                        return c - 'a' + 10;
                    }
                    if (c >= 'A' && c <= 'F') {
                        return c - 'A' + 10;
                    }
                    return -1;
                };
                int const hi = hexval(text[i + 1]);
                int const lo = hexval(text[i + 2]);
                if (hi >= 0 && lo >= 0) {
                    out.push_back(static_cast<char>(hi * 16 + lo));
                    i += 2;
                } else {
                    out.push_back(text[i]);
                }
            } else {
                out.push_back(text[i]);
            }
        }
        return out;
    }

} // namespace dana::tools::tse
