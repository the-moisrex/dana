#include "../core/fs.hpp"
#include "../core/process.hpp"
#include "../sdk/prompt_registry.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

        // Port of grep -Eq '^(https?|file)://[^[:space:]]+$' (yt.sh's URL test).
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

        // yt.links and srt2text are local and quick; subtitle may download with
        // yt-dlp, so it gets a longer budget.
        constexpr auto kLinksTimeout    = std::chrono::seconds(30);
        constexpr auto kSubtitleTimeout = std::chrono::minutes(5);
        constexpr auto kSrt2TextTimeout = std::chrono::seconds(30);

        struct command_output {
            std::string stdout_data;
            int         status    = 0;
            bool        timed_out = false;
        };

        command_output run_tool(
          std::filesystem::path const&    tool,
          std::vector<std::string> const& args,
          std::string_view                stdin_data,
          std::chrono::milliseconds       timeout) {
            std::vector<std::string> cmd = {tool.string()};
            cmd.insert(cmd.end(), args.begin(), args.end());
            auto result = dana::process::run_command(cmd, stdin_data, {}, timeout);
            return {.stdout_data = std::move(result.stdout_data),
                    .status      = result.exit_code,
                    .timed_out   = result.timed_out};
        }

        // xargs: every whitespace-separated token becomes one subtitle argument (an
        // empty token list still runs the command once, like GNU xargs without -r).
        std::vector<std::string> xargs_tokens(std::string_view text) {
            std::vector<std::string> tokens;
            std::size_t              pos = 0;
            while (pos < text.size()) {
                std::size_t start = text.find_first_not_of(" \t\r\n", pos);
                if (start == std::string_view::npos) {
                    break;
                }
                std::size_t end = text.find_first_of(" \t\r\n", start);
                if (end == std::string_view::npos) {
                    end = text.size();
                }
                tokens.emplace_back(text.substr(start, end - start));
                pos = end;
            }
            return tokens;
        }

        struct fetch_result {
            bool        ok = false;
            std::string text;    // srt2text output, trailing newlines dropped
            std::string failure; // why the pipeline failed, empty on success
        };

        // `"$curdir/yt.links" "$url" | xargs "$curdir/subtitle" | "$curdir/srt2text"`
        // (under `set -o pipefail`, so any failing stage fails the whole pipeline). The
        // pipeline stops at its first failing stage and reports which one it was.
        fetch_result transform_url(std::string_view url, std::span<std::string const> tools) {
            auto reason = [](std::string_view stage, std::chrono::milliseconds timeout, command_output const& out) {
                if (out.timed_out) {
                    return std::format("{} timed out after {}s", stage, timeout.count() / 1000);
                }
                return std::format("{} exited {}", stage, out.status);
            };

            auto links = run_tool(tools[0], {std::string(url)}, {}, kLinksTimeout);
            if (links.status != 0 || links.timed_out) {
                return {.ok = false, .text = {}, .failure = reason("yt.links", kLinksTimeout, links)};
            }

            auto subtitle = run_tool(tools[1], xargs_tokens(links.stdout_data), {}, kSubtitleTimeout);
            if (subtitle.status != 0 || subtitle.timed_out) {
                return {.ok = false, .text = {}, .failure = reason("subtitle", kSubtitleTimeout, subtitle)};
            }

            auto text = run_tool(tools[2], {}, subtitle.stdout_data, kSrt2TextTimeout);
            if (text.status != 0 || text.timed_out) {
                return {.ok = false, .text = {}, .failure = reason("srt2text", kSrt2TextTimeout, text)};
            }

            return {.ok = true, .text = drop_trailing_newlines(text.stdout_data), .failure = {}};
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

        std::vector<std::string> tools;
        tools.reserve(3);
        for (char const* name : {"yt.links", "subtitle", "srt2text"}) {
            tools.push_back(dana::fs::bin_tool(name, v.env.exe_path, v.env.git_root).string());
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
            auto fetched = transform_url(trimmed, tools);
            if (fetched.ok) {
                // yt.sh asks yt.links a second time for the readable video URL.
                std::string url = drop_trailing_newlines(run_tool(tools[0], {trimmed}, {}, kLinksTimeout).stdout_data);
                if (url.empty()) {
                    output +=
                      "Turn this into an article and remove the ads, repetition, meaningless stuff and add all "
                      "key takeaways and summary notes at the end as well: \n\n\n";
                } else {
                    output += "Turn this [youtube video](" + url +
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
    bool const registered = dana::register_prompt(
      "yt",
      "Builds an article-writing prompt from stdin. If stdin is a single YouTube URL, it "
      "fetches the subtitles/text first and fails when that fetch fails (--fallback keeps "
      "the URL instead).",
      &execute_yt,
      &render_help_yt);

} // namespace dana
