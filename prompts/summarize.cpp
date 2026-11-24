#include "../core/fs.hpp"
#include "../runtime/prompt_registry.hpp"
#include "../tools/srt2text.hpp"
#include "../tools/subtitle.hpp"
#include "../tools/yt_links.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace dana {

    namespace {

        // Port of `trimmed="$(printf '%s' "$input" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"`:
        // every line is stripped of leading/trailing whitespace (sed works line by
        // line), then command substitution drops the result's trailing newlines.
        std::string trim_lines(std::string_view input) {
            std::string out;
            std::size_t pos = 0;
            while (pos <= input.size()) {
                std::size_t      nl    = input.find('\n', pos);
                bool             last  = nl == std::string_view::npos;
                std::string_view line  = last ? input.substr(pos) : input.substr(pos, nl - pos);
                std::size_t      begin = 0;
                std::size_t      end   = line.size();
                while (begin < end && std::isspace(static_cast<unsigned char>(line[begin])) != 0) {
                    ++begin;
                }
                while (end > begin && std::isspace(static_cast<unsigned char>(line[end - 1])) != 0) {
                    --end;
                }
                out.append(line.substr(begin, end - begin));
                if (last) {
                    break;
                }
                out += '\n';
                pos  = nl + 1;
            }
            while (!out.empty() && out.back() == '\n') {
                out.pop_back();
            }
            return out;
        }

        std::string drop_trailing_newlines(std::string_view text) {
            while (!text.empty() && text.back() == '\n') {
                text.remove_suffix(1);
            }
            return std::string(text);
        }

        // Port of grep -Eq '^https?://[^[:space:]]+$' (summarize.sh's URL test).
        bool is_url(std::string_view trimmed) {
            if (trimmed.contains('\n')) {
                return false; // multi-line input
            }
            std::string_view rest;
            if (trimmed.starts_with("http://")) {
                rest = trimmed.substr(7);
            } else if (trimmed.starts_with("https://")) {
                rest = trimmed.substr(8);
            } else {
                return false;
            }
            if (rest.empty()) {
                return false; // the + needs at least one character
            }
            return std::ranges::all_of(rest, [](char c) {
                return std::isspace(static_cast<unsigned char>(c)) == 0;
            });
        }

        // `yt.links "$url" | xargs subtitle | srt2text`, stopping at the first
        // failing stage.
        std::string transform_url(std::string_view const url, bool& ok) {
            auto const link = tools::extract_link(url);
            if (link.url.empty() || link.failed) {
                ok = false;
                return {};
            }
            auto const subtitle = tools::fetch_subtitles(link.url);
            if (!subtitle) {
                ok = false;
                return {};
            }
            ok = true;
            return drop_trailing_newlines(tools::srt_to_text(*subtitle));
        }

    } // namespace

    static void execute_summarize(view const& v, result& r) {
        // bash checks `[ -t 0 ]`: a piped stdin wins over the clipboard.
        std::string input;
        if (v.in.piped()) {
            input = drop_trailing_newlines(v.in.text());
        } else {
            input = drop_trailing_newlines(clipboard_content(v.env));
            if (input.empty()) {
                throw soft_error{"No input. Pipe a URL or clipboard content.\n"};
            }
        }

        std::string output;
        output += "Summarize this and remove the ads, repetition, meaningless stuff: \n\n";

        std::string trimmed = trim_lines(input);
        if (is_url(trimmed)) {
            bool        ok     = false;
            std::string result = transform_url(trimmed, ok);
            if (ok) {
                output += result;
                r.out   = std::move(output);
                return;
            }
            // transform_url failed: fall back to the raw input.
        }

        output += input;
        r.out   = std::move(output);
    }

    static void render_help_summarize(std::ostream& os) {
        os << R"EOF(Usage: prompt summarize

Builds a prompt that summarizes stdin.
If stdin is a single URL, it tries to fetch subtitles/text first.
)EOF";
    }

    // This file is the prompt: it implements it and registers it at static-init time
    // (no central list — see sdk/prompt_registry.hpp).
    [[maybe_unused]] bool const registered = dana::register_prompt(
      "summarize",
      "Builds a prompt that summarizes stdin. If stdin is a single URL, it tries to "
      "fetch subtitles/text first.",
      &execute_summarize,
      &render_help_summarize);

} // namespace dana
