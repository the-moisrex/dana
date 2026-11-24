#include "tse_codal.hpp"

#include "tse_common.hpp"
#include "tse_resolver.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <regex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dana::tools::tse {

    namespace {

        // ---------------------------------------------------------------- constants

        constexpr std::string_view codal_search_base    = "https://search.codal.ir/api/search/v2/q";
        constexpr std::string_view codal_report_base    = "https://www.codal.ir";
        constexpr double           host_spacing_seconds = 4.0;
        constexpr std::size_t      max_announcements    = 20;
        constexpr std::size_t      body_limit           = 6'000'000;
        constexpr int              codal_cache_version  = 1;
        constexpr double           default_max_age      = 6 * 3600.0;
        constexpr double           default_attempts     = 5.0;
        constexpr double           default_deadline     = 60.0;
        constexpr double           retry_backoff        = 1.5;
        constexpr double           retry_backoff_max    = 8.0;
        constexpr double           retry_jitter         = 0.75;
        constexpr double           host_cooldown        = 4.0;
        constexpr std::string_view cache_kind_search    = "search";
        constexpr std::string_view cache_kind_report    = "report";

        constexpr std::array<std::string_view, 9> transient_markers = {
          "Cannot reach",
          "Connection ",
          "timed out",
          "reset by peer",
          "HTTP 5",
          "Rate limited",
          "requires browser verification",
          "Access blocked",
          "Non-JSON response",
        };

        // Only transport-level failures are retried; a rate limit or a
        // verification page is the site's answer, not a flaky edge.
        constexpr std::array<std::string_view, 7> retry_markers = {
          "Cannot reach",
          "Connection ",
          "timed out",
          "reset by peer",
          "HTTP 5",
          "HTTP 408",
          "HTTP 425",
        };

        // ---------------------------------------------------------------- text

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

        [[nodiscard]] std::string lower_ascii(std::string_view text) {
            std::string out(text);
            for (char& c : out) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return out;
        }

        [[nodiscard]] std::string env_text(std::string const& name) {
            auto const* raw = std::getenv(name.c_str()); // NOLINT(concurrency-mt-unsafe): read once at startup; no
                                                         // concurrent env mutation.
            return raw != nullptr ? std::string(raw) : std::string{};
        }

        [[nodiscard]] double env_number(std::string const& name, double fallback) {
            auto const raw = trim_copy(env_text(name));
            if (raw.empty()) {
                return fallback;
            }
            double value         = 0.0;
            auto const [ptr, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
            if (ec != std::errc{} || ptr != raw.data() + raw.size()) {
                return fallback;
            }
            return value > 0.0 ? value : fallback;
        }

        [[nodiscard]] int env_int(std::string const& name, int fallback) {
            auto const raw = trim_copy(env_text(name));
            if (raw.empty()) {
                return fallback;
            }
            int value            = 0;
            auto const [ptr, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
            if (ec != std::errc{} || ptr != raw.data() + raw.size()) {
                return fallback;
            }
            return value > 0 ? value : fallback;
        }

        // `90`, `30m`, `6h`, `2d` -> seconds.
        [[nodiscard]] std::optional<double> parse_duration(std::string_view text) {
            static std::regex const re(R"(^\s*(\d+(?:\.\d+)?)\s*([smhd]?)\s*$)");
            std::string             owned(text);
            std::smatch             match;
            if (!std::regex_match(owned, match, re)) {
                return std::nullopt;
            }
            double     scale = 1.0;
            auto const unit  = match[2].str();
            if (unit == "m") {
                scale = 60.0;
            } else if (unit == "h") {
                scale = 3600.0;
            } else if (unit == "d") {
                scale = 86400.0;
            }
            return std::stod(match[1].str()) * scale;
        }

        [[nodiscard]] double env_max_age() {
            auto const parsed = parse_duration(trim_copy(env_text("CODAL_MAX_AGE")));
            return parsed ? *parsed : default_max_age;
        }

        [[nodiscard]] json human_age(std::optional<double> seconds) {
            if (!seconds) {
                return nullptr;
            }
            double const value = *seconds;
            if (value < 90.0) {
                return std::format("{}s", static_cast<long long>(value));
            }
            if (value < 90.0 * 60.0) {
                return std::format("{}m", static_cast<long long>(value / 60.0));
            }
            if (value < 48.0 * 3600.0) {
                return std::format("{}h", static_cast<long long>(value / 3600.0));
            }
            return std::format("{}d", static_cast<long long>(value / 86400.0));
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
            return !value.empty();
        }

        [[nodiscard]] std::string compact_dump(json const& value) {
            return value.dump(-1, ' ', false);
        }

        // Python's `re.sub(r"[^\w-]+", "-", normal(text)).strip("-") or "x"`.
        [[nodiscard]] bool is_word_codepoint(char32_t c) noexcept {
            if (c < 0x80) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                       c == '-';
            }
            switch (c) {
                case 0x00A0:
                case 0x00AB:
                case 0x00BB:
                case 0x00B7:
                case 0x060C:
                case 0x061B:
                case 0x061F:
                case 0x066A:
                case 0x066B:
                case 0x066C:
                case 0x066D:
                case 0x2010:
                case 0x2011:
                case 0x2012:
                case 0x2013:
                case 0x2014:
                case 0x2015:
                case 0x2018:
                case 0x2019:
                case 0x201A:
                case 0x201B:
                case 0x201C:
                case 0x201D:
                case 0x201E:
                case 0x201F:
                case 0x2026:
                case 0x2028:
                case 0x2029:
                case 0x202F:
                case 0x205F:
                case 0x3000: return false;
                default: return true;
            }
        }

        [[nodiscard]] std::string slug(std::string_view text) {
            auto const  normalized = normal(text);
            std::string out;
            out.reserve(normalized.size());
            bool pending_dash = false;
            for (std::size_t i = 0; i < normalized.size();) {
                auto const  c      = static_cast<unsigned char>(normalized[i]);
                char32_t    value  = c;
                std::size_t length = 1;
                if (c >= 0xF0 && i + 3 < normalized.size()) {
                    value  = static_cast<char32_t>(c & 0x07U);
                    length = 4;
                } else if (c >= 0xE0 && i + 2 < normalized.size()) {
                    value  = static_cast<char32_t>(c & 0x0FU);
                    length = 3;
                } else if (c >= 0xC0 && i + 1 < normalized.size()) {
                    value  = static_cast<char32_t>(c & 0x1FU);
                    length = 2;
                }
                for (std::size_t j = 1; j < length; ++j) {
                    value =
                      (value << 6U) | static_cast<char32_t>(static_cast<unsigned char>(normalized[i + j]) & 0x3FU);
                }
                if (is_word_codepoint(value)) {
                    if (pending_dash && !out.empty()) {
                        out.push_back('-');
                    }
                    pending_dash = false;
                    out.append(normalized, i, length);
                } else {
                    pending_dash = true;
                }
                i += length;
            }
            if (out.empty()) {
                return "x";
            }
            auto const first = out.find_first_not_of('-');
            if (first == std::string::npos) {
                return "x";
            }
            auto const last = out.find_last_not_of('-');
            return out.substr(first, last - first + 1);
        }

        [[nodiscard]] std::string sha1_hex(std::string_view data) {
            std::uint32_t             h[5] = {0x6745'2301u, 0xEFCD'AB89u, 0x98BA'DCFEu, 0x1032'5476u, 0xC3D2'E1F0u};
            std::vector<std::uint8_t> message(data.begin(), data.end());
            auto const                bit_len = static_cast<std::uint64_t>(message.size()) * 8;
            message.push_back(0x80);
            while (message.size() % 64 != 56) {
                message.push_back(0);
            }
            for (int i = 7; i >= 0; --i) {
                message.push_back(static_cast<std::uint8_t>((bit_len >> (i * 8U)) & 0xFFU));
            }
            auto const rol = [](std::uint32_t value, int bits) {
                return (value << static_cast<unsigned>(bits)) | (value >> static_cast<unsigned>(32 - bits));
            };
            for (std::size_t chunk = 0; chunk < message.size(); chunk += 64) {
                std::uint32_t w[80];
                for (int i = 0; i < 16; ++i) {
                    auto const base = chunk + static_cast<std::size_t>(i) * 4;
                    w[i]            = (static_cast<std::uint32_t>(message[base]) << 24U) |
                                      (static_cast<std::uint32_t>(message[base + 1]) << 16U) |
                                      (static_cast<std::uint32_t>(message[base + 2]) << 8U) |
                                      static_cast<std::uint32_t>(message[base + 3]);
                }
                for (int i = 16; i < 80; ++i) {
                    w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
                }
                std::uint32_t a = h[0];
                std::uint32_t b = h[1];
                std::uint32_t c = h[2];
                std::uint32_t d = h[3];
                std::uint32_t e = h[4];
                for (int i = 0; i < 80; ++i) {
                    std::uint32_t f = 0;
                    std::uint32_t k = 0;
                    if (i < 20) {
                        f = (b & c) | (~b & d);
                        k = 0x5A82'7999u;
                    } else if (i < 40) {
                        f = b ^ c ^ d;
                        k = 0x6ED9'EBA1u;
                    } else if (i < 60) {
                        f = (b & c) | (b & d) | (c & d);
                        k = 0x8F1B'BCDCu;
                    } else {
                        f = b ^ c ^ d;
                        k = 0xCA62'C1D6u;
                    }
                    auto const temp = rol(a, 5) + f + e + k + w[i];
                    e               = d;
                    d               = c;
                    c               = rol(b, 30);
                    b               = a;
                    a               = temp;
                }
                h[0] += a;
                h[1] += b;
                h[2] += c;
                h[3] += d;
                h[4] += e;
            }
            std::string out;
            out.reserve(40);
            for (auto const word : h) {
                out += std::format("{:08x}", word);
            }
            return out;
        }

        // ---------------------------------------------------------------- urls

        [[nodiscard]] std::string_view authority_of(std::string_view url) noexcept {
            auto const scheme = url.find("://");
            auto const start  = scheme == std::string_view::npos ? 0 : scheme + 3;
            auto const end    = url.find_first_of("/?#", start);
            return url.substr(start, (end == std::string_view::npos ? url.size() : end) - start);
        }

        [[nodiscard]] std::string host_of(std::string_view url) {
            auto       authority = authority_of(url);
            auto const at        = authority.rfind('@');
            if (at != std::string_view::npos) {
                authority.remove_prefix(at + 1);
            }
            auto const colon = authority.find(':');
            if (colon != std::string_view::npos) {
                authority = authority.substr(0, colon);
            }
            return lower_ascii(authority);
        }

        [[nodiscard]] std::string url_path(std::string_view url) {
            auto const scheme = url.find("://");
            auto const start  = scheme == std::string_view::npos ? 0 : scheme + 3;
            auto const slash  = url.find('/', start);
            if (slash == std::string_view::npos) {
                return {};
            }
            auto const end = url.find_first_of("?#", slash);
            return std::string(url.substr(slash, (end == std::string_view::npos ? url.size() : end) - slash));
        }

        // `urllib.parse.urljoin(codal_report_base + "/", path)` restricted to the
        // Codal hosts the report pages actually use.
        [[nodiscard]] std::optional<std::string> codal_url(json const& value, bool report) {
            if (!value.is_string()) {
                return std::nullopt;
            }
            auto const path = value.get<std::string>();
            if (path.empty()) {
                return std::nullopt;
            }
            std::string joined;
            if (path.starts_with("https://") || path.starts_with("http://")) {
                joined = path;
            } else if (path.starts_with("//")) {
                joined = "https:" + path;
            } else if (path.starts_with('/')) {
                joined = std::string(codal_report_base) + path;
            } else {
                joined = std::string(codal_report_base) + "/" + path;
            }
            auto const scheme_end = joined.find("://");
            if (scheme_end == std::string::npos || lower_ascii(joined.substr(0, scheme_end)) != "https") {
                return std::nullopt;
            }
            auto const host  = host_of(joined);
            bool const known = host == "www.codal.ir" || host == "codal.ir" ||
                               (!report && (host == "cdn.codal.ir" || host == "excel.codal.ir"));
            if (!known) {
                return std::nullopt;
            }
            return joined;
        }

        [[nodiscard]] std::string with_sheet_id(std::string const& url, int sheet_id) {
            auto const  scheme_end  = url.find("://");
            auto const  auth_start  = scheme_end == std::string::npos ? 0 : scheme_end + 3;
            auto const  path_start  = url.find('/', auth_start);
            auto const  query_start = url.find('?', path_start == std::string::npos ? auth_start : path_start);
            std::string base        = url.substr(0, query_start == std::string::npos ? url.size() : query_start);

            std::vector<std::pair<std::string, std::string>> query;
            if (query_start != std::string::npos) {
                auto const             fragment  = url.find('#', query_start);
                auto const             query_end = fragment == std::string::npos ? url.size() : fragment;
                std::string_view const raw(url.data() + query_start + 1, query_end - query_start - 1);
                std::size_t            pos = 0;
                while (pos <= raw.size()) {
                    auto const amp = raw.find('&', pos);
                    auto const part =
                      raw.substr(pos, amp == std::string_view::npos ? std::string_view::npos : amp - pos);
                    if (!part.empty()) {
                        auto const eq = part.find('=');
                        auto key = url_decode_plus(part.substr(0, eq == std::string_view::npos ? part.size() : eq));
                        auto value =
                          eq == std::string_view::npos ? std::string{} : url_decode_plus(part.substr(eq + 1));
                        query.emplace_back(std::move(key), std::move(value));
                    }
                    if (amp == std::string_view::npos) {
                        break;
                    }
                    pos = amp + 1;
                }
            }

            std::string out = std::move(base);
            out.push_back('?');
            bool first = true;
            for (auto const& [key, value] : query) {
                if (lower_ascii(key) == "sheetid") {
                    continue;
                }
                if (!first) {
                    out.push_back('&');
                }
                first  = false;
                out   += url_encode(key);
                out.push_back('=');
                out += url_encode(value);
            }
            if (!first) {
                out.push_back('&');
            }
            out += "SheetId=";
            out += std::to_string(sheet_id);
            return out;
        }

        [[nodiscard]] std::string normalize_symbol(std::string_view value) {
            auto                    text = trim_copy(value);
            static std::regex const codal_re(R"(codal\.ir/ReportList\.aspx\?[^#]*?\bSymbol=([^&#]+))",
                                             std::regex::icase);
            std::smatch             match;
            if (std::regex_search(text, match, codal_re)) {
                return trim_copy(url_decode_plus(match[1].str()));
            }
            return text;
        }

        // ---------------------------------------------------------------- cache

        [[nodiscard]] std::filesystem::path expand_home(std::string const& text) {
            if (text == "~" || text.starts_with("~/")) {
                auto const home = env_text("HOME");
                if (!home.empty()) {
                    return std::filesystem::path(home) / text.substr(text == "~" ? 1 : 2);
                }
            }
            return std::filesystem::path(text);
        }

        [[nodiscard]] std::filesystem::path cache_root() {
            auto const override = trim_copy(env_text("CODAL_CACHE_DIR"));
            if (!override.empty()) {
                std::error_code ec;
                auto const      absolute = std::filesystem::absolute(expand_home(override), ec);
                return ec ? expand_home(override) : absolute.lexically_normal();
            }
            auto base = env_text("XDG_CACHE_HOME");
            if (base.empty()) {
                base = (expand_home("~/.cache")).string();
            }
            return std::filesystem::path(base) / "tse" / "codal";
        }

        [[nodiscard]] std::filesystem::path cache_path(std::string_view kind, json const& key) {
            std::string name;
            if (kind == cache_kind_search) {
                name = std::format("search-{}-lt{}-cat{}.json",
                                   slug(scalar_text(pick(key, {"symbol"}))),
                                   scalar_text(pick(key, {"letter_type"})),
                                   scalar_text(pick(key, {"category"})));
            } else {
                name = std::format("report-{}.json", slug(scalar_text(pick(key, {"letter"}))));
            }
            return cache_root() / name;
        }

        // nlohmann's json has an implicit conversion to any type, including
        // std::optional<json>, so build the optional explicitly.
        [[nodiscard]] std::optional<json> as_optional(json value) {
            return std::optional<json>(std::in_place, std::move(value));
        }

        [[nodiscard]] std::optional<json> cache_read(std::filesystem::path const& path) {
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                return std::nullopt;
            }
            std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            auto        entry = json::parse(text, nullptr, false);
            if (entry.is_discarded() || !entry.is_object()) {
                return std::nullopt;
            }
            auto const version = pick(entry, {"version"});
            if (!version.is_number_integer() || version.get<long long>() != codal_cache_version) {
                return std::nullopt;
            }
            auto const payload = pick(entry, {"payload"});
            if (!payload.is_object() && !payload.is_array()) {
                return std::nullopt;
            }
            return as_optional(std::move(entry));
        }

        [[nodiscard]] std::string utc_iso_now() {
            auto const        now = std::chrono::system_clock::now();
            std::time_t const seconds =
              std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
            std::tm tm{};
            ::gmtime_r(&seconds, &tm);
            return std::format(
              "{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}+00:00",
              tm.tm_year + 1900,
              tm.tm_mon + 1,
              tm.tm_mday,
              tm.tm_hour,
              tm.tm_min,
              tm.tm_sec);
        }

        void
        cache_write(std::filesystem::path const& path, std::string_view kind, json const& key, json const& payload) {
            json entry              = json::object();
            entry["version"]        = codal_cache_version;
            entry["kind"]           = kind;
            entry["key"]            = key;
            entry["fetched_at"]     = jalali_stamp(tehran_now());
            entry["fetched_at_iso"] = utc_iso_now();
            entry["payload"]        = payload;

            auto const      directory = path.parent_path();
            std::error_code ec;
            std::filesystem::create_directories(directory, ec);
            if (ec) {
                return;
            }
            static std::atomic<unsigned long long> counter{0};
            auto const temp = directory / std::format(".tmp-{}-{}.json", std::random_device{}(), counter.fetch_add(1));
            {
                std::ofstream out(temp, std::ios::binary | std::ios::trunc);
                if (!out) {
                    return;
                }
                out << entry.dump();
                if (!out) {
                    out.close();
                    std::filesystem::remove(temp, ec);
                    return;
                }
            }
            std::filesystem::rename(temp, path, ec);
            if (ec) {
                std::filesystem::remove(temp, ec);
            }
        }

        [[nodiscard]] bool has_zone_suffix(std::string const& text) {
            if (text.ends_with('Z') || text.ends_with('z')) {
                return true;
            }
            if (text.size() < 6) {
                return false;
            }
            auto const tail = text.substr(text.size() - 6);
            return (tail[0] == '+' || tail[0] == '-') && std::isdigit(static_cast<unsigned char>(tail[1])) != 0 &&
                   std::isdigit(static_cast<unsigned char>(tail[2])) != 0 && tail[3] == ':' &&
                   std::isdigit(static_cast<unsigned char>(tail[4])) != 0 &&
                   std::isdigit(static_cast<unsigned char>(tail[5])) != 0;
        }

        [[nodiscard]] std::optional<double> cache_age(json const& entry) {
            auto const raw = scalar_text(pick(entry, {"fetched_at_iso"}));
            if (raw.empty()) {
                return std::nullopt;
            }
            auto moment = parse_moment(raw);
            if (!moment) {
                return std::nullopt;
            }
            if (!has_zone_suffix(raw)) {
                // A naive timestamp is UTC here, not Tehran local time.
                *moment += 3 * 3600 + 30 * 60;
            }
            auto const age = static_cast<double>(tehran_now() - *moment);
            return age > 0.0 ? age : 0.0;
        }

        [[nodiscard]] json cache_note(json const& entry, bool stale, std::optional<std::string> const& live_error) {
            json note          = json::object();
            note["cached"]     = true;
            note["fetched_at"] = pick(entry, {"fetched_at"});
            note["age"]        = human_age(cache_age(entry));
            note["stale"]      = stale;
            if (live_error) {
                note["live_error"] = *live_error;
            }
            return note;
        }

        [[nodiscard]] bool transient_failure(std::string_view text) {
            return std::any_of(transient_markers.begin(), transient_markers.end(), [text](std::string_view marker) {
                return text.find(marker) != std::string_view::npos;
            });
        }

        [[nodiscard]] bool retryable_failure(std::string_view text) {
            return std::any_of(retry_markers.begin(), retry_markers.end(), [text](std::string_view marker) {
                return text.find(marker) != std::string_view::npos;
            });
        }

        struct [[nodiscard]] cache_policy {
            double max_age    = default_max_age;
            bool   refresh    = false;
            bool   cache_only = false;
        };

        struct [[nodiscard]] fetch_result {
            json                payload;
            std::optional<json> note;
        };

        [[nodiscard]] fetch_result cached_fetch(
          cache_policy const&          policy,
          std::string_view             kind,
          json const&                  key,
          std::function<json()> const& fetch) {
            auto const path  = cache_path(kind, key);
            auto       entry = policy.refresh ? std::optional<json>{} : cache_read(path);

            auto label = scalar_text(pick(key, {"symbol"}));
            if (label.empty()) {
                label = scalar_text(pick(key, {"letter"}));
            }
            if (label.empty()) {
                label = "Codal data";
            }

            if (entry) {
                auto const age   = cache_age(*entry);
                bool const fresh = age.has_value() && *age <= policy.max_age;
                if (fresh || policy.cache_only) {
                    return {(*entry)["payload"],
                            as_optional(cache_note(*entry, policy.cache_only && !fresh, std::nullopt))};
                }
            }
            if (policy.cache_only) {
                if (entry) {
                    return {(*entry)["payload"], as_optional(cache_note(*entry, true, std::nullopt))};
                }
                throw error(std::format("No cached Codal data for {} and --cache-only was given", label));
            }
            try {
                auto payload = fetch();
                cache_write(path, kind, key, payload);
                return {std::move(payload), std::nullopt};
            } catch (error const& exc) {
                if (entry && !policy.refresh) {
                    return {(*entry)["payload"], as_optional(cache_note(*entry, true, std::string(exc.what())))};
                }
                throw;
            }
        }

        // ---------------------------------------------------------------- http

        struct [[nodiscard]] pace_state {
            std::mutex                                                             mutex;
            std::unordered_map<std::string, std::chrono::steady_clock::time_point> last;
        };

        [[nodiscard]] pace_state& pace_global() {
            static pace_state state;
            return state;
        }

        void pace(std::string const& host) {
            using clock = std::chrono::steady_clock;
            std::chrono::milliseconds wait{0};
            {
                std::lock_guard const lock(pace_global().mutex);
                auto&                 last = pace_global().last[host];
                auto const            now  = clock::now();
                auto const next = last + std::chrono::milliseconds(static_cast<long long>(host_spacing_seconds * 1000));
                wait            = next > now ? std::chrono::duration_cast<std::chrono::milliseconds>(next - now)
                                             : std::chrono::milliseconds{0};
                last            = now + wait;
            }
            if (wait.count() > 0) {
                std::this_thread::sleep_for(wait);
            }
        }

        void penalize(std::string const& host) {
            using clock = std::chrono::steady_clock;
            std::lock_guard const lock(pace_global().mutex);
            auto&                 last = pace_global().last[host];
            auto const target = clock::now() + std::chrono::milliseconds(static_cast<long long>(host_cooldown * 1000));
            if (last < target) {
                last = target;
            }
        }

        template <typename Operation>
        auto codal_retry(std::string const& host, Operation&& operation) -> std::invoke_result_t<Operation> {
            using clock        = std::chrono::steady_clock;
            auto const started = clock::now();
            auto const deadline =
              started +
              std::chrono::milliseconds(static_cast<long long>(env_number("CODAL_DEADLINE", default_deadline) * 1000));
            auto const limit = env_int("CODAL_RETRIES", static_cast<int>(default_attempts));

            int         attempts = 0;
            std::string last_error;
            while (attempts < limit) {
                ++attempts;
                try {
                    return operation();
                } catch (error const& exc) {
                    last_error = exc.what();
                    if (!retryable_failure(last_error)) {
                        throw;
                    }
                    penalize(host);
                    auto const remaining = deadline - clock::now();
                    if (attempts >= limit || remaining <= clock::duration::zero()) {
                        break;
                    }
                    auto const delay = std::min(retry_backoff_max, retry_backoff * std::pow(2.0, attempts - 1));
                    thread_local std::mt19937              rng{std::random_device{}()};
                    std::uniform_real_distribution<double> jitter(0.0, retry_jitter * 1000.0);
                    auto const sleep_ms = std::min(delay * 1000.0 + jitter(rng),
                                                   std::chrono::duration<double, std::milli>(remaining).count());
                    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long long>(sleep_ms)));
                }
            }
            auto const elapsed = std::chrono::duration<double>(clock::now() - started).count();
            throw error(std::format(
              "Cannot reach {} after {} attempt(s) in {:.0f}s ({}); Codal resets connections intermittently, retry in "
              "a minute",
              host,
              attempts,
              elapsed,
              last_error.empty() ? "unknown error" : last_error));
        }

        [[nodiscard]] std::string codal_get(client& http, std::string const& url, std::string_view accept) {
            auto const host = host_of(url);
            pace(host);
            return codal_retry(host, [&] {
                return http.get(url, accept, body_limit);
            });
        }

        [[nodiscard]] json codal_json(client& http, std::string const& url) {
            auto const host = host_of(url);
            pace(host);
            auto const text = codal_retry(host, [&] {
                return http.get(url, "application/json", body_limit);
            });
            auto       data = json::parse(text, nullptr, false);
            if (data.is_discarded()) {
                throw error(std::format("Non-JSON response from {} (possibly blocked)", host));
            }
            return data;
        }

        // ---------------------------------------------------------------- codal rows

        [[nodiscard]] json codal_meta(json const& row) {
            auto const report_url = [&row](std::initializer_list<std::string_view> names, bool report) -> json {
                if (auto const url = codal_url(pick(row, names), report)) {
                    return *url;
                }
                return nullptr;
            };

            json out              = json::object();
            out["title"]          = pick(row, {"Title"});
            out["letter_code"]    = pick(row, {"LetterCode"});
            out["company_name"]   = pick(row, {"CompanyName"});
            out["published_at"]   = pick(row, {"PublishDateTime", "SentDateTime"});
            out["sent_at"]        = pick(row, {"SentDateTime"});
            out["tracing_no"]     = pick(row, {"TracingNo"});
            out["is_estimate"]    = pick(row, {"IsEstimate"});
            out["url"]            = report_url({"Url"}, true);
            out["pdf_url"]        = report_url({"PdfUrl"}, false);
            out["excel_url"]      = report_url({"ExcelUrl"}, false);
            out["xbrl_url"]       = report_url({"XbrlUrl"}, false);
            out["tedan_url"]      = report_url({"TedanUrl"}, false);
            out["attachment_url"] = report_url({"AttachmentUrl"}, false);
            return out;
        }

        [[nodiscard]] std::string strip_spaces(std::string text) {
            text.erase(std::remove(text.begin(), text.end(), ' '), text.end());
            return text;
        }

        [[nodiscard]] json statement_row(json const& rows, std::initializer_list<std::string_view> labels) {
            if (!rows.is_array() || rows.empty()) {
                return nullptr;
            }
            std::unordered_set<std::string> wanted;
            for (auto const label : labels) {
                wanted.insert(strip_spaces(normal(label)));
            }
            for (auto const& row : rows) {
                if (!row.is_object()) {
                    continue;
                }
                auto const item = strip_spaces(normal(scalar_text(pick(row, {"item"}))));
                if (wanted.contains(item)) {
                    return number(pick(row, {"current_period"}));
                }
            }
            return nullptr;
        }

        [[nodiscard]] std::optional<long long> unit_factor(json const& shares, json const& capital) {
            auto const share_count    = as_double(shares);
            auto const capital_amount = as_double(capital);
            if (!share_count || !capital_amount || *share_count == 0.0 || *capital_amount <= 0.0) {
                return std::nullopt;
            }
            double const                              factor = *share_count * 1000.0 / *capital_amount;
            static constexpr std::array<long long, 5> units{1, 1000, 1'000'000, 1'000'000'000, 1'000'000'000'000};
            for (auto const unit : units) {
                double const ratio = factor / static_cast<double>(unit);
                if (ratio >= 0.9 && ratio <= 1.1) {
                    return unit;
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] std::string unit_name(long long unit) {
            switch (unit) {
                case 1: return "rial";
                case 1000: return "thousand_rial";
                case 1'000'000: return "million_rial";
                case 1'000'000'000: return "billion_rial";
                default: return "trillion_rial";
            }
        }

        void derive_fundamentals(json& fundamentals, json const& filings, json const& closing) {
            if (!fundamentals.is_object() || !filings.is_object()) {
                return;
            }
            json       balance_rows = nullptr;
            json       income_rows  = nullptr;
            json       period       = nullptr;
            auto const statements   = pick(filings, {"statements"});
            if (statements.is_array()) {
                for (auto const& report : statements) {
                    if (!report.is_object()) {
                        continue;
                    }
                    auto const financials   = pick(report, {"financials"});
                    auto const balance      = pick(financials, {"balance_sheet"});
                    auto const income       = pick(financials, {"income_statement"});
                    auto const balance_data = pick(balance, {"rows"});
                    if (balance_data.is_array() && !balance_data.empty()) {
                        balance_rows = balance_data;
                    }
                    auto const income_data = pick(income, {"rows"});
                    if (income_data.is_array() && !income_data.empty()) {
                        income_rows = income_data;
                    }
                    auto const report_period = pick(report, {"period"});
                    if (truthy(report_period)) {
                        period = report_period;
                    }
                    if (truthy(balance_rows) || truthy(income_rows)) {
                        break;
                    }
                }
            }
            if (!truthy(balance_rows) && !truthy(income_rows)) {
                return;
            }

            json derived      = json::object();
            derived["source"] = "Codal extracted statement rows (derived, not as-reported ratios)";

            auto const equity =
              statement_row(balance_rows, {"جمع حقوق مالکانه", "جمع حقوق صاحبان سهام", "جمع کل حقوق مالکانه"});
            auto const liabilities = statement_row(balance_rows, {"جمع بدهی ها", "جمع کل بدهی ها"});
            auto       capital     = statement_row(balance_rows, {"سرمایه"});
            if (capital.is_null()) {
                capital = statement_row(income_rows, {"سرمایه"});
            }
            auto const revenue = statement_row(
              income_rows,
              {"درآمدهای عملیاتی", "جمع درآمدهای عملیاتی", "فروش کالا", "فروش کالا و خدمات", "فروش کالاها و خدمات"});
            auto const net_income =
              statement_row(income_rows, {"سود(زیان) خالص", "سود (زیان) خالص", "سودوزیان خالص", "سود خالص"});
            auto const shares = pick(fundamentals, {"share_count"});

            auto const equity_num      = as_double(equity);
            auto const liabilities_num = as_double(liabilities);
            auto const revenue_num     = as_double(revenue);
            auto const net_income_num  = as_double(net_income);

            if (equity_num && net_income_num && *equity_num != 0.0) {
                derived["roe_pct"] = round_to(*net_income_num / *equity_num * 100.0, 2);
            }
            if (revenue_num && net_income_num && *revenue_num != 0.0) {
                derived["net_margin_pct"] = round_to(*net_income_num / *revenue_num * 100.0, 2);
            }
            if (liabilities_num && equity_num && *equity_num != 0.0) {
                derived["debt_to_equity"] = round_to(*liabilities_num / *equity_num, 2);
            }
            if (auto const factor = unit_factor(shares, capital)) {
                derived["unit_detection"] = unit_name(*factor);
                auto const shares_num     = as_double(shares);
                if (equity_num && shares_num && *shares_num != 0.0) {
                    double const book_value              = *equity_num * static_cast<double>(*factor) / *shares_num;
                    derived["book_value_per_share_rial"] = static_cast<long long>(std::nearbyint(book_value));
                    auto const closing_num               = as_double(closing);
                    if (closing_num && book_value > 0.0) {
                        derived["pb"] = round_to(*closing_num / book_value, 2);
                    }
                }
            } else {
                derived["unit_detection"] = "failed";
            }
            if (truthy(period)) {
                derived["period"] = period;
            }
            fundamentals["derived_ratios"] = derived;
        }

        [[nodiscard]] json sheet_rows(json const& datasource, std::string_view alias) {
            auto const sheets = pick(datasource, {"sheets"});
            if (!sheets.is_array()) {
                return nullptr;
            }
            for (auto const& sheet : sheets) {
                if (!sheet.is_object()) {
                    continue;
                }
                auto const tables = pick(sheet, {"tables"});
                if (!tables.is_array()) {
                    continue;
                }
                for (auto const& table : tables) {
                    if (!table.is_object() || scalar_text(pick(table, {"aliasName"})) != alias) {
                        continue;
                    }
                    std::map<long long, std::map<long long, json>> grid;
                    auto const                                     cells = pick(table, {"cells"});
                    if (cells.is_array()) {
                        for (auto const& cell : cells) {
                            if (!cell.is_object()) {
                                continue;
                            }
                            auto const row_number = number(pick(cell, {"rowSequence"}));
                            auto const col_number = number(pick(cell, {"columnSequence"}));
                            if (!row_number.is_number_integer() || !col_number.is_number_integer()) {
                                continue;
                            }
                            grid[row_number.get<long long>()][col_number.get<long long>()] = pick(cell, {"value"});
                        }
                    }
                    for (auto const& [header, columns] : grid) {
                        std::vector<long long> label_columns;
                        for (auto const& [column, text] : columns) {
                            if (normal(scalar_text(text)) == "شرح") {
                                label_columns.push_back(column);
                            }
                        }
                        if (label_columns.empty()) {
                            continue;
                        }
                        json result = json::array();
                        for (auto const& [row_number, row_cells] : grid) {
                            if (row_number <= header) {
                                continue;
                            }
                            for (auto const column : label_columns) {
                                auto const label_it = row_cells.find(column);
                                auto const label =
                                  normal(label_it != row_cells.end() ? scalar_text(label_it->second) : std::string{});
                                auto const amount_it = row_cells.find(column + 1);
                                auto const amount =
                                  amount_it != row_cells.end() ? number(amount_it->second) : json(nullptr);
                                if (!label.empty() && !amount.is_null()) {
                                    result.push_back(json{
                                      {          "item",  label},
                                      {"current_period", amount}
                                    });
                                }
                            }
                        }
                        if (!result.empty()) {
                            json out             = json::object();
                            out["table_title"]   = pick(table, {"title_Fa"});
                            bool const truncated = result.size() > 60;
                            if (truncated) {
                                json cut = json::array();
                                for (std::size_t i = 0; i < 60; ++i) {
                                    cut.push_back(result[i]);
                                }
                                out["rows"] = std::move(cut);
                            } else {
                                out["rows"] = std::move(result);
                            }
                            out["truncated"] = truncated;
                            return out;
                        }
                    }
                }
            }
            return nullptr;
        }

        [[nodiscard]] bool verification_page(std::string_view html) {
            if (
              html.find("تأیید کاربر") != std::string_view::npos || html.find("تایید کاربر") != std::string_view::npos)
            {
                return true;
            }
            auto const lowered = lower_ascii(html);
            return lowered.find("captcha") != std::string::npos || lowered.find("verify") != std::string::npos;
        }

        // Python's `JSONDecoder().raw_decode` from an offset: scan the balanced
        // object/array and parse just that slice.
        [[nodiscard]] json raw_decode(std::string_view text, std::size_t offset) {
            while (offset < text.size() && is_space(text[offset])) {
                ++offset;
            }
            if (offset >= text.size()) {
                throw error("Report has no embedded financial datasource");
            }
            char const open  = text[offset];
            char       close = '\0';
            if (open == '{') {
                close = '}';
            } else if (open == '[') {
                close = ']';
            }
            if (close == '\0') {
                throw error("Unexpected financial datasource");
            }
            int         depth  = 0;
            bool        quoted = false;
            bool        escape = false;
            std::size_t end    = std::string_view::npos;
            for (std::size_t i = offset; i < text.size(); ++i) {
                char const c = text[i];
                if (quoted) {
                    if (escape) {
                        escape = false;
                    } else if (c == '\\') {
                        escape = true;
                    } else if (c == '"') {
                        quoted = false;
                    }
                    continue;
                }
                if (c == '"') {
                    quoted = true;
                } else if (c == open) {
                    ++depth;
                } else if (c == close) {
                    --depth;
                    if (depth == 0) {
                        end = i + 1;
                        break;
                    }
                }
            }
            if (end == std::string_view::npos) {
                throw error("Unexpected financial datasource");
            }
            auto data = json::parse(text.substr(offset, end - offset), nullptr, false);
            if (data.is_discarded()) {
                throw error("Unexpected financial datasource");
            }
            return data;
        }

        [[nodiscard]] fetch_result statement(client& http, json const& metadata, cache_policy const& policy) {
            auto const        url_value = pick(metadata, {"url"});
            std::string const url       = url_value.is_string() ? url_value.get<std::string>() : std::string{};
            if (url.empty() || lower_ascii(url_path(url)) != "/reports/decision.aspx") {
                json report          = metadata;
                report["financials"] = nullptr;
                report["reason"]     = "No supported machine-readable report URL";
                return {std::move(report), std::nullopt};
            }

            auto const fetch = [&]() -> json {
                json                     financials = json::object();
                json                     details    = json::object();
                std::vector<std::string> failures;

                struct sheet_request {
                    int              sheet_id;
                    std::string_view alias;
                    std::string_view name;
                };
                static constexpr std::array<sheet_request, 2> sheets{
                  {
                   {1, "IncomeStatement", "income_statement"},
                   {0, "BalanceSheet", "balance_sheet"},
                   }
                };

                for (auto const& sheet : sheets) {
                    try {
                        auto const              page = with_sheet_id(url, sheet.sheet_id);
                        auto const              html = codal_get(http, page, "text/html");
                        static std::regex const datasource_re(R"(\bvar\s+datasource\s*=)");
                        std::smatch             match;
                        if (!std::regex_search(html, match, datasource_re)) {
                            if (verification_page(html)) {
                                throw error("Report page requires browser verification; try again later");
                            }
                            throw error("Report has no embedded financial datasource");
                        }
                        auto const data = raw_decode(html, static_cast<std::size_t>(match.position() + match.length()));
                        if (!data.is_object()) {
                            throw error("Unexpected financial datasource");
                        }
                        details                             = json::object();
                        details["period"]                   = pick(data, {"period"});
                        details["periodEndToDate"]          = pick(data, {"periodEndToDate"});
                        details["yearEndToDate"]            = pick(data, {"yearEndToDate"});
                        details["isAudited"]                = pick(data, {"isAudited"});
                        details["isConsolidated"]           = pick(data, {"isConsolidated"});
                        financials[std::string(sheet.name)] = sheet_rows(data, sheet.alias);
                        if (financials[std::string(sheet.name)].is_null()) {
                            failures.push_back(std::format("{}: no recognized table rows", sheet.name));
                        }
                    } catch (std::exception const& exc) {
                        auto const message = std::string(exc.what());
                        failures.push_back(std::format("{}: {}", sheet.name, message));
                        if (message.find("requires browser verification") != std::string::npos ||
                            message.find("Rate limited") != std::string::npos)
                        {
                            break;
                        }
                    }
                }

                std::string reason;
                for (std::size_t i = 0; i < failures.size(); ++i) {
                    if (i != 0) {
                        reason += "; ";
                    }
                    reason += failures[i];
                }
                if (financials.empty() && !failures.empty() &&
                    std::all_of(failures.begin(),
                                failures.end(),
                                [](std::string const& item) {
                                    return transient_failure(item);
                                }))
                {
                    // Never cache (or return) an outage as a parsed report; the
                    // caller falls back to the last good extraction.
                    throw error(reason);
                }

                json report          = metadata;
                report["period"]     = details;
                report["financials"] = financials.empty() ? json(nullptr) : financials;
                report["reason"]     = reason.empty() ? json(nullptr) : json(reason);
                return report;
            };

            auto const tracing = pick(metadata, {"tracing_no"});
            auto const letter  = truthy(tracing) ? scalar_text(tracing) : sha1_hex(url).substr(0, 16);
            json       key     = json::object();
            key["letter"]      = letter;
            key["url"]         = url;
            return cached_fetch(policy, cache_kind_report, key, fetch);
        }

        // ---------------------------------------------------------------- search

        [[nodiscard]] std::string search_url(std::string const& symbol, int letter_type, int category) {
            std::vector<std::pair<std::string, std::string>> params;
            params.emplace_back("Symbol", symbol);
            params.emplace_back("Category", std::to_string(category));
            params.emplace_back("PublisherType", "1");
            params.emplace_back("LetterType", std::to_string(letter_type));
            params.emplace_back("Length", "-1");
            params.emplace_back("Audited", "true");
            params.emplace_back("NotAudited", "true");
            params.emplace_back("IsNotAudited", "false");
            params.emplace_back("Mains", "true");
            params.emplace_back("Childs", "false");
            params.emplace_back("Consolidatable", "true");
            params.emplace_back("NotConsolidatable", "true");
            params.emplace_back("AuditorRef", "-1");
            params.emplace_back("CompanyState", "0");
            params.emplace_back("CompanyType", "-1");
            params.emplace_back("PageNumber", "1");
            params.emplace_back("TracingNo", "-1");
            params.emplace_back("Publisher", "false");
            params.emplace_back("search", "true");

            std::string url(codal_search_base);
            url.push_back('?');
            for (std::size_t i = 0; i < params.size(); ++i) {
                if (i != 0) {
                    url.push_back('&');
                }
                url += url_encode(params[i].first);
                url.push_back('=');
                url += url_encode(params[i].second);
            }
            return url;
        }

        [[nodiscard]] fetch_result
        codal_search(client& http, std::string symbol, int letter_type, int category, cache_policy const& policy) {
            symbol         = normalize_symbol(symbol);
            auto const url = search_url(symbol, letter_type, category);

            auto const query = [&]() -> std::vector<json> {
                auto const payload = codal_json(http, url);
                auto const rows    = pick(payload, {"Letters"});
                if (!rows.is_array()) {
                    throw error("Unexpected Codal search result");
                }
                std::vector<json> out;
                for (auto const& row : rows) {
                    if (!row.is_object()) {
                        continue;
                    }
                    if (normal(scalar_text(pick(row, {"Symbol"}))) == normal(symbol)) {
                        out.push_back(row);
                    }
                }
                return out;
            };

            auto const fetch = [&]() -> json {
                auto rows = query();
                if (rows.empty()) {
                    // Without a live cookie session Codal answers zero rows
                    // instead of a rate limit, so retry once.
                    rows = query();
                }
                json out = json::array();
                for (auto& row : rows) {
                    out.push_back(std::move(row));
                }
                return out;
            };

            json key           = json::object();
            key["symbol"]      = normal(symbol);
            key["letter_type"] = letter_type;
            key["category"]    = category;
            return cached_fetch(policy, cache_kind_search, key, fetch);
        }

        // ---------------------------------------------------------------- instrument metadata

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

        [[nodiscard]] json quote(client& http, std::string const& code) {
            auto const row = http.api(std::format("ClosingPrice/GetClosingPriceInfo/{}", code), "closingPriceInfo");
            if (!row.is_object()) {
                throw error("No quote for this instrument");
            }
            auto const state     = pick(row, {"instrumentState"});
            auto const last      = price(pick(row, {"pDrCotVal", "pl"}));
            auto const yesterday = price(pick(row, {"priceYesterday", "py"}));

            json out          = json::object();
            out["ins_code"]   = code;
            out["trade_date"] = trade_date(pick(row, {"dEven"}));
            out["trade_time"] = trade_time(pick(row, {"hEven"}));
            auto state_title  = pick(state, {"cEtavalTitle"});
            if (!truthy(state_title)) {
                state_title = pick(row, {"cEtavalTitle"});
            }
            out["state"]             = state_title;
            out["last_rial"]         = last;
            out["closing_rial"]      = price(pick(row, {"pClosing", "pc"}));
            auto const last_num      = as_double(last);
            auto const yesterday_num = as_double(yesterday);
            if (last_num && yesterday_num && *last_num != 0.0 && *yesterday_num != 0.0) {
                out["last_change_pct"] = round_to((*last_num / *yesterday_num - 1.0) * 100.0, 2);
            } else {
                out["last_change_pct"] = nullptr;
            }
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

        [[nodiscard]] bool
        detect_fund(json const& isin, json const& industry, json const& sub_industry, json const& units) {
            auto const label     = normal(scalar_text(industry)) + " " + normal(scalar_text(sub_industry));
            bool const isin_text = isin.is_string();
            bool const labelled  = label.find("صندوق") != std::string::npos;
            if (!isin_text && !truthy(units) && !labelled) {
                return false;
            }
            return (isin_text && isin.get<std::string>().starts_with("IRT")) || truthy(units) || labelled;
        }

        // The share count used by the ratio unit detection. Only that field and
        // the error keys are observable in the Codal dataset.
        [[nodiscard]] json
        instrument_info(client& http, std::string const& code, json const& ident, json const& current, json& errors) {
            json       share_count  = pick(ident, {"share_count"});
            json       industry     = pick(ident, {"industry"});
            auto const sub_industry = pick(ident, {"sub_industry"});
            json       market_cap   = nullptr;
            json       fund_units   = nullptr;

            try {
                auto const row = http.api(std::format("Instrument/GetInstrumentInfo/{}", code), "instrumentInfo");
                if (!row.is_object()) {
                    throw error("No instrument fundamentals");
                }
                auto const count = value_of(row, {"shareCount", "zTitad"});
                if (!count.is_null()) {
                    share_count = count;
                }
                auto const sector = pick(pick(row, {"sector"}), {"lSecVal"});
                if (truthy(sector)) {
                    industry = sector;
                }
                auto const units = value_of(row, {"etfIssuedUnit"});
                if (truthy(units)) {
                    json detail     = json::object();
                    detail["units"] = units;
                    detail["date"]  = trade_date(pick(row, {"etfUnitDeven"}));
                    fund_units      = detail;
                }
                market_cap = value_of(row, {"marketValue", "marketCap"});
            } catch (error const& exc) {
                errors["instrument_info"] = std::string(exc.what());
            }

            bool const is_fund = detect_fund(pick(ident, {"isin"}), industry, sub_industry, fund_units);
            if (is_fund) {
                try {
                    auto const fund = http.api(std::format("Fund/GetETFByInsCode/{}", code), "etf");
                    if (!fund.is_object()) {
                        throw error("No fund NAV data");
                    }
                } catch (error const& exc) {
                    errors["fund_nav"] = std::string(exc.what());
                }
            }

            auto const isin    = pick(ident, {"isin"});
            auto const closing = pick(current, {"closing_rial"});
            if (truthy(isin) && (share_count.is_null() || (market_cap.is_null() && closing.is_null()))) {
                try {
                    auto const url =
                      std::string(webgw_base) + "?" + url_encode("InstrumentId") + "=" + url_encode(scalar_text(isin));
                    auto       row  = http.json_get(url);
                    auto const data = pick(row, {"data"});
                    if (truthy(data)) {
                        row = data;
                    }
                    if (!row.is_object()) {
                        throw error("Unexpected official gateway response");
                    }
                    if (share_count.is_null()) {
                        share_count = value_of(row, {"sharecount"});
                    }
                    if (market_cap.is_null()) {
                        market_cap = value_of(row, {"marketvalue"});
                    }
                } catch (error const& exc) {
                    errors["market_cap"] = std::string(exc.what());
                }
            }

            json out           = json::object();
            out["share_count"] = share_count;
            return out;
        }

        // ---------------------------------------------------------------- dataset

        [[nodiscard]] json
        codal(client& http, std::string symbol, json& errors, bool fund, cache_policy const& policy) {
            symbol = normalize_symbol(symbol);

            json found        = json::object();
            json found_errors = json::object();
            json found_cache  = json::object();

            try {
                auto result             = codal_search(http, symbol, 6, 1, policy);
                found["statement_rows"] = std::move(result.payload);
                if (result.note) {
                    found_cache["financials"] = *result.note;
                }
            } catch (error const& exc) {
                found_errors["codal_financials"] = std::string(exc.what());
            }

            try {
                auto        result = codal_search(http, symbol, -1, -1, policy);
                json        list   = json::array();
                auto const& rows   = result.payload;
                if (rows.is_array()) {
                    for (std::size_t i = 0; i < rows.size() && i < max_announcements; ++i) {
                        list.push_back(codal_meta(rows[i]));
                    }
                }
                found["announcements"] = std::move(list);
                if (result.note) {
                    found_cache["announcements"] = *result.note;
                }
            } catch (error const& exc) {
                found_errors["codal_announcements"] = std::string(exc.what());
            }

            if (fund) {
                try {
                    auto        result = codal_search(http, symbol, -1, 3, policy);
                    json        list   = json::array();
                    auto const& rows   = result.payload;
                    if (rows.is_array()) {
                        for (std::size_t i = 0; i < rows.size() && i < 3; ++i) {
                            list.push_back(codal_meta(rows[i]));
                        }
                    }
                    found["portfolio_reports"] = std::move(list);
                    if (result.note) {
                        found_cache["portfolio_reports"] = *result.note;
                    }
                } catch (error const& exc) {
                    found_errors["codal_portfolio"] = std::string(exc.what());
                }
            }

            json       statements  = json::array();
            json       page_errors = json::object();
            json       page_cache  = json::object();
            auto const rows_value  = found.contains("statement_rows") ? found["statement_rows"] : json(nullptr);
            if (rows_value.is_array() && !rows_value.empty()) {
                cache_policy                    report_policy = policy;
                std::unordered_set<std::string> seen_reports;
                std::unordered_set<std::string> seen_periods;
                for (std::size_t i = 0; i < rows_value.size() && i < 6; ++i) {
                    auto const& row = rows_value[i];
                    if (!row.is_object()) {
                        continue;
                    }
                    auto const metadata = codal_meta(row);
                    if (!truthy(pick(metadata, {"url"})) || !truthy(pick(metadata, {"title"}))) {
                        continue;
                    }
                    auto const key = scalar_text(pick(row, {"TracingNo"}));
                    if (!seen_reports.insert(key).second) {
                        continue;
                    }
                    try {
                        auto result = statement(http, metadata, report_policy);
                        if (result.note) {
                            page_cache["financials"] = *result.note;
                            if (truthy(pick(*result.note, {"live_error"}))) {
                                // The network is down; serve the remaining
                                // letters from the cache instead of retrying.
                                report_policy.cache_only = true;
                            }
                        }
                        auto period = pick(result.payload, {"period"});
                        if (!period.is_object()) {
                            period = json::object();
                        }
                        auto const period_end   = pick(period, {"periodEndToDate"});
                        auto const consolidated = pick(period, {"isConsolidated"});
                        auto const period_key   = compact_dump(json::array({period_end, consolidated}));
                        if (truthy(period_end) && seen_periods.contains(period_key)) {
                            continue;
                        }
                        seen_periods.insert(period_key);
                        statements.push_back(result.payload);
                        auto const reason = scalar_text(pick(result.payload, {"reason"}));
                        if (!reason.empty() && (reason.find("requires browser verification") != std::string::npos ||
                                                reason.find("Rate limited") != std::string::npos ||
                                                reason.find("Cannot reach") != std::string::npos))
                        {
                            page_errors["codal_financials"] = reason;
                            break;
                        }
                        if (statements.size() >= 2) {
                            break;
                        }
                    } catch (error const& exc) {
                        if (report_policy.cache_only) {
                            continue;
                        }
                        json empty          = metadata;
                        empty["period"]     = json::object();
                        empty["financials"] = nullptr;
                        empty["reason"]     = std::string(exc.what());
                        statements.push_back(std::move(empty));
                        page_errors["codal_financials"] = std::string(exc.what());
                        break;
                    }
                }
            }

            json result             = json::object();
            result["announcements"] = found.contains("announcements") ? found["announcements"] : json::array();
            result["statements"]    = std::move(statements);
            result["portfolio_reports"] =
              found.contains("portfolio_reports") ? found["portfolio_reports"] : json::array();

            json cache = json::object();
            for (auto const* source : {&found_cache, &page_cache}) {
                for (auto it = source->begin(); it != source->end(); ++it) {
                    auto const existing = cache.find(it.key());
                    if (existing == cache.end() ||
                        (truthy(pick(it.value(), {"stale"})) && !truthy(pick(existing.value(), {"stale"}))))
                    {
                        // A stale artifact makes the whole answer stale.
                        cache[it.key()] = it.value();
                    }
                }
            }
            if (!cache.empty()) {
                result["cache"] = cache;
            }

            for (auto const key : {"codal_announcements", "codal_portfolio", "codal_financials"}) {
                if (found_errors.contains(key)) {
                    errors[key] = found_errors[key];
                }
                if (page_errors.contains(key)) {
                    errors[key] = page_errors[key];
                }
            }

            // A stale cache is only used because the live fetch failed; say both.
            static constexpr std::array<std::pair<std::string_view, std::string_view>, 3> cache_errors{
              {
               {"announcements", "codal_announcements"},
               {"portfolio_reports", "codal_portfolio"},
               {"financials", "codal_financials"},
               }
            };
            for (auto const& [name, error_key] : cache_errors) {
                auto const note       = cache.contains(std::string(name)) ? cache[std::string(name)] : json::object();
                auto const live_error = pick(note, {"live_error"});
                if (truthy(live_error) && !errors.contains(std::string(error_key))) {
                    auto const fetched_at          = pick(note, {"fetched_at"});
                    errors[std::string(error_key)] = std::format(
                      "live fetch failed ({}); using cached data from {}",
                      scalar_text(live_error),
                      truthy(fetched_at) ? scalar_text(fetched_at) : "an unknown time");
                }
            }

            if (result["statements"].empty() && !errors.contains("codal_financials")) {
                errors["codal_financials"] = "No matching Codal financial statements found";
            } else if (!result["statements"].empty()) {
                bool any_rows = false;
                for (auto const& report : result["statements"]) {
                    auto const financials = pick(report, {"financials"});
                    if (!financials.is_object()) {
                        continue;
                    }
                    for (auto it = financials.begin(); it != financials.end(); ++it) {
                        if (truthy(it.value())) {
                            any_rows = true;
                            break;
                        }
                    }
                    if (any_rows) {
                        break;
                    }
                }
                if (!any_rows && !errors.contains("codal_financials")) {
                    auto const reason = pick(result["statements"][0], {"reason"});
                    errors["codal_financials"] =
                      truthy(reason) ? scalar_text(reason) : "No financial statement rows extracted";
                }
            }
            return result;
        }

        [[nodiscard]] json codal_snapshot_data(
          client&             http,
          std::string const&  symbol,
          int                 top,
          json&               errors,
          bool                fund,
          cache_policy const& policy) {
            json result = codal(http, symbol, errors, fund, policy);

            if (result.contains("announcements") && result["announcements"].is_array()) {
                json        trimmed       = json::array();
                auto const& announcements = result["announcements"];
                auto const  limit         = static_cast<std::size_t>(std::max(0, top));
                for (std::size_t i = 0; i < announcements.size() && i < limit; ++i) {
                    trimmed.push_back(announcements[i]);
                }
                result["announcements"] = std::move(trimmed);
            }
            if (!result.contains("portfolio_reports")) {
                result["portfolio_reports"] = json::array();
            }
            result["retrieved_at"] = jalali_stamp(tehran_now());
            return result;
        }

        [[nodiscard]] std::string join_unique_errors(json const& errors) {
            std::vector<std::string> seen;
            std::string              out;
            for (auto it = errors.begin(); it != errors.end(); ++it) {
                auto text = scalar_text(it.value());
                if (std::find(seen.begin(), seen.end(), text) != seen.end()) {
                    continue;
                }
                seen.push_back(text);
                if (!out.empty()) {
                    out += "; ";
                }
                out += text;
            }
            return out;
        }

        [[nodiscard]] std::pair<json, json>
        snapshot_dataset(std::string const& symbol, int top, json& errors, bool fund, cache_policy const& policy) {
            auto const  fallback = normalize_symbol(symbol);
            std::string canonical;
            json        instrument = json::object();
            json        ident      = json::object();
            bool        resolved   = false;
            client      http;

            auto const normalized = normal(normalize_input(symbol));
            bool const digits_only =
              !normalized.empty() && std::all_of(normalized.begin(), normalized.end(), [](char c) {
                  return c >= '0' && c <= '9';
              });
            if (digits_only) {
                instrument["ins_code"] = normalized;
                instrument["symbol"]   = "";
                instrument["name"]     = "";
                instrument["isin"]     = nullptr;
                instrument["flow"]     = nullptr;
                instrument["market"]   = nullptr;
                resolved               = true;
            } else {
                try {
                    auto const record      = resolve_one(http, symbol);
                    auto const code        = pick(record, {"ins_code"});
                    instrument["ins_code"] = truthy(code) ? code : json(normalized);
                    auto const name        = pick(record, {"name"});
                    auto const sym         = pick(record, {"symbol"});
                    instrument["symbol"]   = truthy(sym) ? sym : json("");
                    instrument["name"]     = truthy(name) ? name : json("");
                    instrument["isin"]     = pick(record, {"isin"});
                    instrument["flow"]     = pick(record, {"flow"});
                    instrument["market"]   = pick(record, {"market"});
                    resolved               = true;
                } catch (error const&) {
                    instrument = json{
                      {"symbol", fallback}
                    };
                }
            }

            if (resolved) {
                auto const code = pick(instrument, {"ins_code"});
                if (truthy(code)) {
                    try {
                        ident = identity(http, scalar_text(code));
                    } catch (error const&) {
                        ident = json::object();
                    }
                }
            }

            canonical = scalar_text(pick(ident, {"symbol"}));
            if (canonical.empty()) {
                canonical = scalar_text(pick(instrument, {"symbol"}));
            }
            if (canonical.empty()) {
                canonical = fallback;
            }
            if (canonical.empty()) {
                throw error("No symbol found for Codal search");
            }

            auto isin = pick(ident, {"isin"});
            if (!truthy(isin)) {
                isin = pick(instrument, {"isin"});
            }
            bool const is_fund =
              fund || detect_fund(isin, pick(ident, {"industry"}), pick(ident, {"sub_industry"}), json(nullptr));

            json result = codal_snapshot_data(http, canonical, top, errors, is_fund, policy);

            if (!truthy(pick(result, {"announcements"})) && !truthy(pick(result, {"statements"})) &&
                !truthy(pick(result, {"portfolio_reports"})) && !errors.empty())
            {
                throw error("No Codal data available: " + join_unique_errors(errors));
            }

            json       closing     = nullptr;
            json       share_count = pick(ident, {"share_count"});
            auto const code        = pick(instrument, {"ins_code"});
            if (resolved && truthy(code)) {
                try {
                    auto const current = quote(http, scalar_text(code));
                    closing            = pick(current, {"closing_rial"});
                    if (!truthy(share_count)) {
                        // GetInstrumentIdentity can report 0; the instrument
                        // info carries the real share count.
                        auto const info = instrument_info(http, scalar_text(code), ident, current, errors);
                        share_count     = pick(info, {"share_count"});
                    }
                } catch (std::exception const& exc) {
                    errors["quote"] = std::string(exc.what());
                }
            }

            json fundamentals           = json::object();
            fundamentals["share_count"] = share_count;
            derive_fundamentals(fundamentals, result, closing);
            if (truthy(pick(fundamentals, {"derived_ratios"}))) {
                result["derived_ratios"] = fundamentals["derived_ratios"];
            }

            json out_instrument      = instrument.is_object() ? instrument : json::object();
            out_instrument["symbol"] = canonical;
            out_instrument["isin"]   = isin;
            auto name                = pick(ident, {"name"});
            if (!truthy(name)) {
                name = pick(instrument, {"name"});
            }
            out_instrument["name"] = name;
            return {std::move(result), std::move(out_instrument)};
        }

        // ---------------------------------------------------------------- markdown

        [[nodiscard]] std::vector<std::string> codal_cache_lines(json const& cache) {
            std::vector<std::string> items;
            if (!cache.is_object()) {
                return items;
            }
            for (auto it = cache.begin(); it != cache.end(); ++it) {
                if (!it.value().is_object()) {
                    continue;
                }
                auto label = std::string(it.key());
                if (label == "financials") {
                    label = "financial statements";
                } else if (label == "portfolio_reports") {
                    label = "portfolio reports";
                }
                std::string text        = "- " + label + ": cached, fetched at ";
                auto const  fetched_at  = scalar_text(pick(it.value(), {"fetched_at"}));
                text                   += fetched_at.empty() ? "an unknown time" : fetched_at;
                auto const age          = scalar_text(pick(it.value(), {"age"}));
                if (!age.empty()) {
                    text += " (" + age + " ago)";
                }
                if (truthy(pick(it.value(), {"stale"}))) {
                    text += ", stale";
                }
                auto const live_error = scalar_text(pick(it.value(), {"live_error"}));
                if (!live_error.empty()) {
                    text += "; live fetch failed: " + live_error;
                }
                items.push_back(std::move(text));
            }
            if (items.empty()) {
                return items;
            }
            std::vector<std::string> out;
            out.reserve(items.size() + 2);
            out.emplace_back("");
            out.emplace_back("Cached Codal data (`tse.codal snapshot --refresh` refetches):");
            for (auto& item : items) {
                out.push_back(std::move(item));
            }
            return out;
        }

        [[nodiscard]] std::string render_markdown(json const& result, json const& instrument, json const& errors) {
            auto const now              = tehran_now();
            json       data             = json::object();
            data["fetched_at"]          = jalali_stamp(now);
            data["market_session_open"] = market_session_open(now);
            data["instrument"]          = instrument;
            data["codal"]               = result;
            data["errors"]              = errors;

            std::vector<std::string> lines;
            auto const               fetched_raw  = scalar_text(pick(data, {"fetched_at"}));
            auto const               when         = parse_moment(fetched_raw);
            std::string              fetched_text = "unknown";
            if (when) {
                fetched_text = jalali_stamp(*when);
            } else if (!fetched_raw.empty()) {
                fetched_text = fetched_raw;
            }
            lines.push_back("Fetched at: " + fetched_text);
            lines.push_back(
              std::format("Market session open by schedule: {} (does not guarantee an instrument is trading)",
                          truthy(data["market_session_open"]) ? "True" : "False"));
            lines.push_back("Prices and traded value are in rial, not toman. Missing values are unknown, not zero.");

            static constexpr std::array<std::pair<std::string_view, std::string_view>, 2> sections{
              {
               {"Symbol / نماد", "instrument"},
               {"Codal / اطلاعیه‌ها و صورت‌های مالی", "codal"},
               }
            };
            for (auto const& [heading, key] : sections) {
                auto const value = pick(data, {key});
                if (value.is_null()) {
                    continue;
                }
                lines.emplace_back("");
                lines.push_back("## " + std::string(heading));
                if (key == "codal" && value.is_object()) {
                    auto const cache_lines = codal_cache_lines(pick(value, {"cache"}));
                    lines.insert(lines.end(), cache_lines.begin(), cache_lines.end());
                }
                lines.emplace_back("```json");
                lines.push_back(compact_dump(value));
                lines.emplace_back("```");
            }

            if (truthy(pick(data, {"errors"}))) {
                lines.emplace_back("");
                lines.emplace_back("## Missing or failed sources / داده‌های ناموجود");
                lines.emplace_back("```json");
                lines.push_back(compact_dump(data["errors"]));
                lines.emplace_back("```");
            }

            std::string out;
            for (std::size_t i = 0; i < lines.size(); ++i) {
                if (i != 0) {
                    out.push_back('\n');
                }
                out += lines[i];
            }
            out.push_back('\n');
            return out;
        }

    } // namespace

    std::string codal_snapshot(std::string const& symbol, codal_options const& options) {
        int const top = std::clamp(options.top <= 0 ? 5 : options.top, 1, 20);

        cache_policy policy;
        policy.max_age    = env_max_age();
        policy.refresh    = options.refresh;
        policy.cache_only = false;

        json errors               = json::object();
        auto [result, instrument] = snapshot_dataset(symbol, top, errors, options.fund, policy);
        return render_markdown(result, instrument, errors);
    }

} // namespace dana::tools::tse
