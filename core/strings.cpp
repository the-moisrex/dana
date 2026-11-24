#include "strings.hpp"

#include "../runtime/embed.hpp"

#include <algorithm>
#include <charconv>

namespace dana::text {

    std::string rstrip_newlines(std::string_view s) {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
            s.remove_suffix(1);
        }
        return std::string(s);
    }

    std::string_view strip_newlines(std::string_view s) {
        while (!s.empty() && s.front() == '\n') {
            s.remove_prefix(1);
        }
        while (!s.empty() && s.back() == '\n') {
            s.remove_suffix(1);
        }
        return s;
    }

    bool append_block(std::string& text, std::string_view block) {
        block = strip_newlines(block);
        if (block.empty()) {
            return false;
        }
        if (!text.empty()) {
            while (!text.empty() && text.back() == '\n') {
                text.pop_back();
            }
            text += "\n\n";
        }
        text.append(block);
        text += '\n';
        return true;
    }

    std::string rstrip(std::string_view s, char c) {
        while (!s.empty() && s.back() == c) {
            s.remove_suffix(1);
        }
        return std::string(s);
    }

    std::string strip(std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
            s.remove_prefix(1);
        }
        return rstrip(s);
    }

    std::vector<std::string> split_lines(std::string_view s, bool keep_blank) {
        std::vector<std::string> out;
        std::size_t              start = 0;
        while (start <= s.size()) {
            std::size_t      end = s.find('\n', start);
            std::string_view line =
              s.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
            if (keep_blank || !line.empty()) {
                out.emplace_back(line);
            }
            if (end == std::string_view::npos) {
                break;
            }
            start = end + 1;
        }
        return out;
    }

    std::vector<std::string> words(std::string_view s) {
        std::vector<std::string> out;
        std::size_t              i = 0;
        while (i < s.size()) {
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
                ++i;
            }
            std::size_t begin = i;
            while (i < s.size() && s[i] != ' ' && s[i] != '\t') {
                ++i;
            }
            if (i > begin) {
                out.emplace_back(s.substr(begin, i - begin));
            }
        }
        return out;
    }

    std::string first_n_lines(std::string_view s, std::size_t n) {
        if (n == 0) {
            return std::string(s);
        }
        std::string out;
        std::size_t start = 0;
        std::size_t seen  = 0;
        while (start < s.size() && seen < n) {
            auto const end  = s.find('\n', start);
            auto const stop = end == std::string_view::npos ? s.size() : end + 1;
            out.append(s.substr(start, stop - start));
            ++seen;
            start = stop;
        }
        if (!out.empty() && out.back() == '\n') {
            out.pop_back();
        }
        return out;
    }

    std::size_t to_size(std::string_view s, std::size_t fallback) {
        if (s.empty() || s.find_first_not_of("0123456789") != std::string_view::npos) {
            return fallback;
        }
        std::size_t value = 0;
        auto        res   = std::from_chars(s.data(), s.data() + s.size(), value);
        if (res.ec != std::errc{} || res.ptr != s.data() + s.size()) {
            return fallback;
        }
        return value;
    }

    std::int64_t to_ll(std::string_view s, std::int64_t fallback) {
        if (s.empty()) {
            return fallback;
        }
        bool             negative = s.front() == '-';
        std::string_view digits   = negative ? s.substr(1) : s;
        if (digits.find_first_not_of("0123456789") != std::string_view::npos) {
            return fallback;
        }
        std::int64_t value = 0;
        auto         res   = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (res.ec != std::errc{} || res.ptr != digits.data() + digits.size()) {
            return fallback;
        }
        return negative ? -value : value;
    }

    std::string regex_escape(std::string_view s) {
        static constexpr char const* meta = "\\^$.|?*+()[]{}";
        std::string                  out;
        out.reserve(s.size() + 8);
        for (char c : s) {
            for (char const* m = meta; *m != '\0'; ++m) {
                if (c == *m) {
                    out += '\\';
                    break;
                }
            }
            out += c;
        }
        return out;
    }

    std::string strip_trailing_newlines(std::string_view text) {
        while (!text.empty() && text.back() == '\n') {
            text.remove_suffix(1);
        }
        return std::string(text);
    }

    // Trim a --head-capped context the way `sed -n 1,Np` does: a range whose end
    // precedes its start counts as line 1, so a --head of 0 keeps one line while an
    // absent --head adds a single trailing newline.
    std::string trim_head(std::string_view const text, std::optional<std::size_t> const head_lines) {
        return trim_context_nl(text, head_lines);
    }

    bool is_all_digits(std::string_view text) {
        if (text.empty()) {
            return false;
        }
        return std::ranges::all_of(text, [](char c) {
            return c >= '0' && c <= '9';
        });
    }

    // Leading-zero tokens would be read in base 8 by an arithmetic test, where
    // 08-style tokens error out and count as false, so only all-zero values fail.
    bool is_positive_int_token(std::string_view const text) {
        if (!is_all_digits(text)) {
            return false;
        }
        return std::ranges::any_of(text, [](char c) {
            return c != '0';
        });
    }

    std::size_t parse_head_value(std::string_view const text) {
        std::string const copy(text);
        std::uint64_t     parsed = std::strtoull(copy.c_str(), nullptr, 10);
        parsed                   = std::min<std::size_t>(parsed, 0x7fff'ffff'ffff'ffffULL);
        return parsed;
    }

    bool ends_with(std::string_view text, std::string_view suffix) {
        return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
    }

    // Generalized helper for appending fenced code/text blocks efficiently
    void append_fenced(std::string&               out,
                       std::string_view           payload,
                       std::optional<std::size_t> head_lines,
                       std::string_view const     language) {
        std::string const trimmed = head_lines.has_value() ? trim_head(payload, *head_lines) : std::string(payload);

        // out.reserve(out.size() + 3 + language.size() + 1 + trimmed.size() + 5);
        out.append("\n```");
        out.append(language);
        out.append("\n");
        out.append(trimmed);
        out.append("\n```\n");
    }

} // namespace dana::text
