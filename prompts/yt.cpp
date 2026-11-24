#include "../core/fs.hpp"
#include "../runtime/prompt_registry.hpp"
#include "../tools/srt2text.hpp"
#include "../tools/subtitle.hpp"
#include "../tools/yt_links.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <string>
#include <string_view>

namespace dana {

    namespace {

        // `trimmed="$(printf '%s' "$input" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"`:
        // every line is stripped of leading/trailing whitespace, then command
        // substitution drops the result's trailing newlines.
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

        // `grep -Eq '^(https?|file)://[^[:space:]]+$'`.
        bool is_url(std::string_view trimmed) {
            if (trimmed.contains('\n')) {
                return false; // multi-line input
            }
            std::string_view rest;
            if (trimmed.starts_with("http://") || trimmed.starts_with("file://")) {
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

        struct fetch_result {
            bool        ok = false;
            std::string text;    // srt2text output, trailing newlines dropped
            std::string failure; // why the pipeline failed, empty on success
        };

        // `yt.links "$url" | xargs subtitle | srt2text`, stopping at the first
        // failing stage and reporting which one it was.
        fetch_result transform_url(std::string_view url) {
            auto const link = tools::extract_link(url);
            if (link.url.empty() || link.failed) {
                return {.ok = false, .text = {}, .failure = "yt.links exited 1"};
            }

            auto const subtitle = tools::fetch_subtitles(link.url);
            if (!subtitle) {
                return {.ok = false, .text = {}, .failure = "subtitle exited 1"};
            }

            return {.ok = true, .text = drop_trailing_newlines(tools::srt_to_text(*subtitle)), .failure = {}};
        }

    } // namespace

    static void execute_yt(view const& v, result& r) {
        bool fallback = false;
        for (auto const arg : v.args) {
            if (arg == "--fallback") {
                fallback = true;
            } else {
                throw usage_error{std::format("prompt yt: unexpected argument '{}'\n", arg)};
            }
        }

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
        std::string trimmed = trim_lines(input);
        if (is_url(trimmed)) {
            auto fetched = transform_url(trimmed);
            if (fetched.ok) {
                // Ask yt.links a second time for the readable video URL.
                auto const link = tools::extract_link(trimmed);
                if (link.url.empty()) {
                    output +=
                      "Turn this into an article and remove the ads, repetition, meaningless stuff and add all "
                      "key takeaways and summary notes at the end as well: \n\n\n";
                } else {
                    output += "Turn this [youtube video](" + link.url +
                              ") into an article and remove the ads, repetition, meaningless stuff and add all key "
                              "takeaways and summary notes at the end as well: \n\n\n";
                }
                output += fetched.text;
                r.out   = std::move(output);
                return;
            }
            // A failed fetch is a failure, not a reason to hand the AI a bare URL:
            // --fallback is the explicit opt-in to the old behaviour.
            if (!fallback) {
                throw soft_error{std::format(
                  "prompt yt: could not fetch subtitles for {} ({})\n"
                  "  pass --fallback to build the prompt from the URL itself\n",
                  trimmed,
                  fetched.failure)};
            }
            r.err = std::format("prompt yt: {}; falling back to the raw URL\n", fetched.failure);
        }

        output +=
          "Turn this into an article and remove the ads, repetition, meaningless stuff and add all key "
          "takeaways and summary notes at the end as well: \n\n\n";
        output += input;
        r.out   = std::move(output);
    }

    static void render_help_yt(std::ostream& os) {
        os << R"EOF(Usage: prompt yt [--fallback]

Builds an article-writing prompt from stdin.
If stdin is a single YouTube URL, it fetches the subtitles/text first and fails
when that fetch fails (--fallback keeps the URL instead).

Options:
  --fallback   Emit the prompt with the raw URL instead of failing when the fetch fails
)EOF";
    }

    // This file is the prompt: it implements it and registers it at static-init time
    // (no central list — see sdk/prompt_registry.hpp).
    [[maybe_unused]] bool const registered = dana::register_prompt(
      "yt",
      "Builds an article-writing prompt from stdin, fetching YouTube subtitles when stdin is a URL.",
      &execute_yt,
      &render_help_yt);

} // namespace dana
