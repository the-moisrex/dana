#include "paths_extract.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dana::tools {

    namespace {

        enum struct [[nodiscard]] resolve_mode : unsigned char {
            automatic,
            strip,
            keep
        };

        [[nodiscard]] bool is_space(char const c) noexcept {
            return std::isspace(static_cast<unsigned char>(c)) != 0;
        }

        [[nodiscard]] bool is_digit(char const c) noexcept {
            return c >= '0' && c <= '9';
        }

        [[nodiscard]] bool is_punctuation(char const c) noexcept {
            return c == '.' || c == ',' || c == ':' || c == ';';
        }

        [[nodiscard]] bool is_bare_char(char const c) noexcept {
            switch (c) {
                case ']':
                case '[':
                case '"':
                case '\'':
                case '`':
                case '<':
                case '>':
                case '|':
                case '(':
                case ')':
                case '{':
                case '}':
                case ',':
                case ';':
                case '=': return false;
                default: return !is_space(c);
            }
        }

        [[nodiscard]] std::string home_dir() {
            if (char const* home = std::getenv("HOME")) { // NOLINT(concurrency-mt-unsafe): read once at startup; no
                                                          // concurrent env mutation.
                return home;
            }
            return {};
        }

        [[nodiscard]] std::string collapse_path(std::string path) {
            std::size_t pos = 0;
            while ((pos = path.find("/./", pos)) != std::string::npos) {
                path.replace(pos, 3, "/");
            }
            pos = 0;
            while ((pos = path.find("//", pos)) != std::string::npos) {
                path.replace(pos, 2, "/");
            }
            return path;
        }

        // The greedy `^(.*)(:[0-9]+:[0-9]+)$` / `^(.*)(:[0-9]+)$` split.
        [[nodiscard]] bool split_location(std::string& candidate, std::string& location) {
            auto const last = candidate.rfind(':');
            if (last == std::string::npos || last + 1 == candidate.size()) {
                return false;
            }
            for (std::size_t i = last + 1; i < candidate.size(); ++i) {
                if (!is_digit(candidate[i])) {
                    return false;
                }
            }
            auto const previous = last > 0 ? candidate.rfind(':', last - 1) : std::string::npos;
            if (previous != std::string::npos && previous + 1 < last) {
                bool const digits = std::all_of(
                  candidate.begin() + static_cast<std::ptrdiff_t>(previous + 1),
                  candidate.begin() + static_cast<std::ptrdiff_t>(last),
                  is_digit);
                if (digits) {
                    location = candidate.substr(previous);
                    candidate.erase(previous);
                    return true;
                }
            }
            location = candidate.substr(last);
            candidate.erase(last);
            return true;
        }

        [[nodiscard]] std::optional<std::pair<int, int>> parse_hunk(std::string_view line) {
            if (!line.starts_with("@@ -")) {
                return std::nullopt;
            }
            std::size_t i      = 4;
            auto        digits = [&](int& value) {
                std::size_t const start = i;
                value                   = 0;
                while (i < line.size() && is_digit(line[i])) {
                    value = value * 10 + (line[i] - '0');
                    ++i;
                }
                return i > start;
            };

            int old_count = 1;
            if (!digits(old_count)) {
                return std::nullopt;
            }
            if (i < line.size() && line[i] == ',') {
                ++i;
                if (!digits(old_count)) {
                    return std::nullopt;
                }
            }
            if (i >= line.size() || line[i] != ' ') {
                return std::nullopt;
            }
            ++i;
            if (i >= line.size() || line[i] != '+') {
                return std::nullopt;
            }
            ++i;
            int new_count = 1;
            if (!digits(new_count)) {
                return std::nullopt;
            }
            if (i < line.size() && line[i] == ',') {
                ++i;
                if (!digits(new_count)) {
                    return std::nullopt;
                }
            }
            if (!line.substr(i).starts_with(" @@")) {
                return std::nullopt;
            }
            return std::pair{old_count, new_count};
        }

        [[nodiscard]] std::string strip_ansi(std::string_view text) {
            std::string out;
            out.reserve(text.size());
            for (std::size_t i = 0; i < text.size(); ++i) {
                if (text[i] == '\033' && i + 1 < text.size() && text[i + 1] == '[') {
                    std::size_t j = i + 2;
                    while (j < text.size() && text[j] >= '0' && text[j] <= '?') {
                        ++j;
                    }
                    while (j < text.size() && text[j] >= ' ' && text[j] <= '/') {
                        ++j;
                    }
                    if (j < text.size() && text[j] >= '@' && text[j] <= '~') {
                        i = j;
                        continue;
                    }
                }
                out.push_back(text[i]);
            }
            return out;
        }

        [[nodiscard]] std::string c_unescape(std::string_view text) {
            std::string out;
            out.reserve(text.size());
            for (std::size_t i = 0; i < text.size(); ++i) {
                if (text[i] != '\\' || i + 1 >= text.size()) {
                    out.push_back(text[i]);
                    continue;
                }
                char const next = text[++i];
                switch (next) {
                    case 'a': out.push_back('\a'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'v': out.push_back('\v'); break;
                    case '\\': out.push_back('\\'); break;
                    case '"': out.push_back('"'); break;
                    default:
                        if (next >= '0' && next <= '7') {
                            int value = next - '0';
                            int count = 1;
                            while (count < 3 && i + 1 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '7') {
                                value = value * 8 + (text[++i] - '0');
                                ++count;
                            }
                            out.push_back(static_cast<char>(value));
                        } else {
                            out.push_back('\\');
                            out.push_back(next);
                        }
                        break;
                }
            }
            return out;
        }

        class extractor {
          public:
            explicit extractor(paths_options options) : m_options(std::move(options)) {}

            [[nodiscard]] std::vector<std::string> run(std::string_view input) {
                std::size_t pos = 0;
                while (pos <= input.size()) {
                    auto const       nl   = input.find('\n', pos);
                    std::string_view line = input.substr(pos, nl == std::string_view::npos ? nl : nl - pos);
                    process_line(line);
                    if (nl == std::string_view::npos) {
                        break;
                    }
                    pos = nl + 1;
                }
                return std::move(m_out);
            }

          private:
            struct resolved {
                std::string resolved;
                std::string identity;
            };

            [[nodiscard]] std::optional<resolved> lookup_file(std::string_view path) const {
                if (path.empty() || path == "/dev/null") {
                    return std::nullopt;
                }
                std::string expanded(path);
                if (path.starts_with("~/")) {
                    expanded = home_dir() + std::string(path.substr(1));
                }
                std::error_code ec;
                if (std::filesystem::is_regular_file(expanded, ec)) {
                    return resolved{std::string(path), expanded};
                }
                if (m_options.repo_root && !std::filesystem::path(std::string(path)).is_absolute() &&
                    !path.starts_with("~/"))
                {
                    auto const candidate = *m_options.repo_root / std::string(path);
                    if (std::filesystem::is_regular_file(candidate, ec)) {
                        return resolved{candidate.string(), candidate.string()};
                    }
                }
                return std::nullopt;
            }

            [[nodiscard]] std::optional<resolved> resolve_file(std::string_view path, resolve_mode mode) const {
                if (path.starts_with("a/") || path.starts_with("b/")) {
                    auto const short_path = path.substr(2);
                    if (mode == resolve_mode::strip) {
                        if (auto found = lookup_file(short_path)) {
                            return found;
                        }
                    }
                    if (auto found = lookup_file(path)) {
                        return found;
                    }
                    if (mode == resolve_mode::automatic) {
                        if (auto found = lookup_file(short_path)) {
                            return found;
                        }
                    }
                    return std::nullopt;
                }
                return lookup_file(path);
            }

            [[nodiscard]] static bool looks_like_path(std::string_view path, std::string_view location) {
                for (char const c : path) {
                    if (!is_bare_char(c)) {
                        return false;
                    }
                }
                if (path == "." || path == ".." || path.ends_with('/')) {
                    return false;
                }
                return path.find('/') != std::string_view::npos || path.find('.') != std::string_view::npos ||
                       !location.empty();
            }

            [[nodiscard]] bool emit(std::string candidate, resolve_mode mode, bool explicit_token) {
                std::string location;
                std::string resolved_path;
                std::string identity;

                if (!m_options.check_exists) {
                    if (!split_location(candidate, location)) {
                        location.clear();
                    }
                    if (candidate.empty() || candidate == "/dev/null" || candidate.contains("://")) {
                        return false;
                    }
                    if (!explicit_token && !looks_like_path(candidate, location)) {
                        return false;
                    }
                    if (mode == resolve_mode::strip && (candidate.starts_with("a/") || candidate.starts_with("b/"))) {
                        candidate.erase(0, 2);
                    }
                    if (candidate.empty() || candidate == "/dev/null") {
                        return false;
                    }
                    resolved_path = candidate;
                    identity      = candidate;
                    if (identity.starts_with("~/")) {
                        identity = home_dir() + identity.substr(1);
                    }
                } else {
                    auto found = resolve_file(candidate, mode);
                    if (!found) {
                        std::string prefix = candidate;
                        if (!split_location(prefix, location)) {
                            return false;
                        }
                        found = resolve_file(prefix, mode);
                        if (!found) {
                            return false;
                        }
                        candidate = std::move(prefix);
                    }
                    resolved_path = found->resolved;
                    identity      = found->identity;
                }

                if (m_options.strip_location) {
                    location.clear();
                }
                identity        = collapse_path((m_options.base / identity).string());
                std::string key = std::to_string(identity.size()) + ":" + identity + location;
                if (m_seen.insert(std::move(key)).second) {
                    m_out.push_back(resolved_path + location);
                }
                return true;
            }

            void emit_token(std::string token, resolve_mode mode, bool explicit_token) {
                if (!m_options.check_exists && !explicit_token) {
                    while (!token.empty() && is_punctuation(token.back())) {
                        token.pop_back();
                    }
                }
                if (emit(token, mode, explicit_token)) {
                    return;
                }
                while (!token.empty() && is_punctuation(token.back())) {
                    token.pop_back();
                    if (emit(token, mode, explicit_token)) {
                        return;
                    }
                }
            }

            [[nodiscard]] static std::string_view location_suffix(std::string_view text, std::size_t& pos) {
                std::size_t const start = pos;
                if (pos >= text.size() || text[pos] != ':') {
                    return {};
                }
                std::size_t       i            = pos + 1;
                std::size_t const digits_start = i;
                while (i < text.size() && is_digit(text[i])) {
                    ++i;
                }
                if (i == digits_start) {
                    return {};
                }
                if (i < text.size() && text[i] == ':') {
                    std::size_t       j            = i + 1;
                    std::size_t const second_start = j;
                    while (j < text.size() && is_digit(text[j])) {
                        ++j;
                    }
                    if (j > second_start) {
                        i = j;
                    }
                }
                pos = i;
                return text.substr(start, i - start);
            }

            void scan(std::string_view text, resolve_mode mode, bool git_quotes) {
                std::size_t pos = 0;
                while (pos < text.size()) {
                    char const c = text[pos];
                    if (c == '"') {
                        std::size_t i = pos + 1;
                        while (i < text.size()) {
                            if (text[i] == '\\' && i + 1 < text.size()) {
                                i += 2;
                                continue;
                            }
                            if (text[i] == '"') {
                                break;
                            }
                            ++i;
                        }
                        std::string token(text.substr(pos + 1, i - pos - 1));
                        if (git_quotes) {
                            token = c_unescape(token);
                        }
                        pos               = i < text.size() ? i + 1 : i;
                        auto const suffix = location_suffix(text, pos);
                        emit_token(token + std::string(suffix), mode, true);
                    } else if (c == '\'' || c == '`') {
                        auto const  close = text.find(c, pos + 1);
                        std::string token(
                          text.substr(pos + 1, close == std::string_view::npos ? close : close - pos - 1));
                        pos               = close == std::string_view::npos ? text.size() : close + 1;
                        auto const suffix = location_suffix(text, pos);
                        emit_token(token + std::string(suffix), mode, true);
                    } else if (is_bare_char(c)) {
                        std::size_t const start = pos;
                        while (pos < text.size() && is_bare_char(text[pos])) {
                            ++pos;
                        }
                        emit_token(std::string(text.substr(start, pos - start)), mode, false);
                    } else {
                        ++pos;
                    }
                }
            }

            void header_path(std::string_view path, resolve_mode mode) {
                if (path.starts_with('"')) {
                    scan(path, mode, true);
                    return;
                }
                auto const tab = path.find('\t');
                if (tab != std::string_view::npos) {
                    path = path.substr(0, tab);
                }
                static std::regex const timestamp(R"(^(.*)[[:space:]]+[0-9]{4}-[0-9]{2}-[0-9]{2}[[:space:]].*$)",
                                                  std::regex::ECMAScript);
                std::string             line(path);
                std::smatch             match;
                if (std::regex_match(line, match, timestamp)) {
                    line = match[1];
                }
                emit_token(line, mode, true);
            }

            void process_line(std::string_view raw) {
                if (!raw.empty() && raw.back() == '\r') {
                    raw.remove_suffix(1);
                }
                std::string const stripped = strip_ansi(raw);
                std::string_view  line     = stripped;

                if (m_old_left > 0 || m_new_left > 0) {
                    if (!m_hunk_indent.empty() && line.starts_with(m_hunk_indent)) {
                        line.remove_prefix(m_hunk_indent.size());
                    }
                    if (line.starts_with(' ')) {
                        if (m_old_left > 0) {
                            --m_old_left;
                        }
                        if (m_new_left > 0) {
                            --m_new_left;
                        }
                        return;
                    }
                    if (line.starts_with('-')) {
                        if (m_old_left > 0) {
                            --m_old_left;
                        }
                        return;
                    }
                    if (line.starts_with('+')) {
                        if (m_new_left > 0) {
                            --m_new_left;
                        }
                        return;
                    }
                    if (line == "\\ No newline at end of file") {
                        return;
                    }
                    m_old_left = 0;
                    m_new_left = 0;
                }

                std::size_t indent = 0;
                while (indent < line.size() && is_space(line[indent])) {
                    ++indent;
                }
                std::string_view const indentation = line.substr(0, indent);
                line.remove_prefix(indent);

                if (auto const hunk = parse_hunk(line)) {
                    m_old_left    = hunk->first;
                    m_new_left    = hunk->second;
                    m_hunk_indent = std::string(indentation);
                    return;
                }

                if (line.starts_with("```") || line.starts_with("~~~")) {
                    return;
                }

                if (line.starts_with("diff --git ")) {
                    std::string_view const payload = line.substr(11);
                    if (payload.starts_with("a/")) {
                        if (auto const sep = payload.find(" b/"); sep != std::string_view::npos) {
                            emit_token(std::string(payload.substr(0, sep)), resolve_mode::strip, true);
                            emit_token("b/" + std::string(payload.substr(sep + 3)), resolve_mode::strip, true);
                            return;
                        }
                    }
                    scan(payload, resolve_mode::strip, true);
                    return;
                }

                if (line.starts_with("--- ") || line.starts_with("+++ ")) {
                    header_path(line.substr(4), resolve_mode::strip);
                    return;
                }

                for (std::string_view const prefix : {"rename from ", "rename to ", "copy from ", "copy to "}) {
                    if (line.starts_with(prefix)) {
                        auto const rest = line.substr(prefix.size());
                        header_path(rest, resolve_mode::keep);
                        return;
                    }
                }

                for (std::string_view const prefix :
                     {"index ",
                      "similarity index ",
                      "dissimilarity index ",
                      "new file mode ",
                      "deleted file mode ",
                      "old mode ",
                      "new mode ",
                      "\\ No newline at end of file"})
                {
                    if (line.starts_with(prefix)) {
                        return;
                    }
                }

                bool const should_scan =
                  !m_options.check_exists || !emit(std::string(line), resolve_mode::automatic, false);
                if (should_scan) {
                    scan(line, resolve_mode::automatic, false);
                }
            }

            paths_options                   m_options;
            std::unordered_set<std::string> m_seen;
            std::vector<std::string>        m_out;
            int                             m_old_left = 0;
            int                             m_new_left = 0;
            std::string                     m_hunk_indent;
        };

    } // namespace

    std::vector<std::string> extract_paths(std::string_view input, paths_options const& options) {
        extractor worker(options);
        return worker.run(input);
    }

} // namespace dana::tools
