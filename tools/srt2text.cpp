#include "srt2text.hpp"

#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>

namespace dana::tools {

    namespace {

        [[nodiscard]] bool is_digit(char c) noexcept {
            return c >= '0' && c <= '9';
        }

        [[nodiscard]] double to_double(std::string_view text) noexcept {
            double value         = 0.0;
            auto const [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
            (void) ptr;
            return ec == std::errc{} ? value : 0.0;
        }

        // awk's hms_to_sec after the comma-to-dot substitution: the milliseconds
        // field is empty (the dot is not a split separator), so only the whole
        // seconds survive. Keep the same split set and arithmetic.
        [[nodiscard]] double parse_time(std::string_view text) noexcept {
            double      total = 0.0;
            std::size_t start = 0;
            int         part  = 0;
            while (start <= text.size()) {
                auto const       sep   = text.find_first_of(":,", start);
                std::string_view field = text.substr(start, sep == std::string_view::npos ? sep : sep - start);
                switch (part) {
                    case 0: total += to_double(field) * 3600.0; break;
                    case 1: total += to_double(field) * 60.0; break;
                    case 2: total += to_double(field); break;
                    default: total += to_double(field) / 1000.0; break;
                }
                if (sep == std::string_view::npos) {
                    break;
                }
                start = sep + 1;
                ++part;
            }
            return total;
        }

        // `^[0-9]+:[0-9][0-9]:[0-9][0-9],[0-9]+ -->`
        [[nodiscard]] bool is_timing_line(std::string_view line) noexcept {
            std::size_t i = 0;
            while (i < line.size() && is_digit(line[i])) {
                ++i;
            }
            if (i == 0 || i >= line.size() || line[i] != ':') {
                return false;
            }
            ++i;
            for (int n = 0; n < 2; ++n) {
                if (i >= line.size() || !is_digit(line[i])) {
                    return false;
                }
                ++i;
            }
            if (i >= line.size() || line[i] != ':') {
                return false;
            }
            ++i;
            for (int n = 0; n < 2; ++n) {
                if (i >= line.size() || !is_digit(line[i])) {
                    return false;
                }
                ++i;
            }
            if (i >= line.size() || line[i] != ',') {
                return false;
            }
            ++i;
            std::size_t const ms_start = i;
            while (i < line.size() && is_digit(line[i])) {
                ++i;
            }
            if (i == ms_start || i >= line.size() || line[i] != ' ') {
                return false;
            }
            ++i;
            return line.substr(i).starts_with("-->");
        }

        // The nth whitespace-separated field (awk's $n), or an empty view.
        [[nodiscard]] std::string_view field(std::string_view line, std::size_t n) noexcept {
            std::size_t i = 0;
            for (std::size_t index = 1; index <= n; ++index) {
                while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
                    ++i;
                }
                std::size_t const start = i;
                while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
                    ++i;
                }
                if (index == n) {
                    return line.substr(start, i - start);
                }
                if (i >= line.size()) {
                    return {};
                }
            }
            return {};
        }

        // Strip every `</?[^>]+>` occurrence (the awk `gsub(/<\/?[^>]+>/, "")`).
        void append_without_tags(std::string& out, std::string_view line) {
            std::size_t pos = 0;
            while (pos < line.size()) {
                auto const open = line.find('<', pos);
                if (open == std::string_view::npos) {
                    out.append(line.substr(pos));
                    return;
                }
                out.append(line.substr(pos, open - pos));
                std::size_t cursor = open + 1;
                if (cursor < line.size() && line[cursor] == '/') {
                    ++cursor;
                }
                if (cursor >= line.size() || line[cursor] == '>') {
                    // Not a tag: keep the '<' and continue after it.
                    out.push_back('<');
                    pos = open + 1;
                    continue;
                }
                auto const close = line.find('>', cursor);
                if (close == std::string_view::npos) {
                    out.append(line.substr(open));
                    return;
                }
                pos = close + 1;
            }
        }

        [[nodiscard]] bool is_index_line(std::string_view line) noexcept {
            if (line.empty()) {
                return false;
            }
            for (char const c : line) {
                if (!is_digit(c)) {
                    return false;
                }
            }
            return true;
        }

    } // namespace

    std::string srt_to_text(std::string_view input) {
        std::string out;
        out.reserve(input.size());

        bool   have_last_end = false;
        double last_end      = 0.0;
        bool   first_text    = true;

        std::size_t pos = 0;
        while (pos <= input.size()) {
            auto const       nl   = input.find('\n', pos);
            std::string_view line = input.substr(pos, nl == std::string_view::npos ? nl : nl - pos);

            if (is_timing_line(line)) {
                double const start = parse_time(field(line, 1));
                double const end   = parse_time(field(line, 3));
                if (have_last_end && start - last_end > 2.0) {
                    out += "\n\n";
                } else if (have_last_end) {
                    out.push_back(' ');
                }
                last_end      = end;
                have_last_end = true;
                first_text    = true;
            } else if (!line.empty() && !is_index_line(line)) {
                if (!first_text) {
                    out.push_back(' ');
                }
                first_text = false;
                append_without_tags(out, line);
            }

            if (nl == std::string_view::npos) {
                break;
            }
            pos = nl + 1;
        }

        out.push_back('\n');
        return out;
    }

} // namespace dana::tools
