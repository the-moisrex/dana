#include "subtitle.hpp"

#include "../core/process.hpp"
#include "yt_links.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace dana::tools {

    namespace {

        [[nodiscard]] std::string yt_lang(std::string_view lang) {
            if (lang == "eng" || lang == "en" || lang == "en-orig") {
                return "en";
            }
            if (lang == "spa" || lang == "es") {
                return "es";
            }
            if (lang == "fas" || lang == "fa") {
                return "fa";
            }
            if (lang == "fra" || lang == "fr") {
                return "fr";
            }
            if (lang == "ger" || lang == "de") {
                return "de";
            }
            if (lang == "zh-Hans" || lang == "zh") {
                return "zh-CN";
            }
            if (lang == "zh-Hant") {
                return "zh-TW";
            }
            return std::string(lang);
        }

        [[nodiscard]] std::filesystem::path make_temp_dir() {
            static unsigned counter = 0;
            auto const      stamp   = std::chrono::steady_clock::now().time_since_epoch().count();
            auto const path = std::filesystem::temp_directory_path() /
                              std::format("prompt-subtitles-{}-{}-{}", static_cast<long>(::getpid()), stamp, ++counter);
            std::error_code ec;
            std::filesystem::create_directories(path, ec);
            return path;
        }

        struct temp_dir_guard {
            std::filesystem::path path;

            ~temp_dir_guard() {
                std::error_code ec;
                std::filesystem::remove_all(path, ec);
            }
        };

        [[nodiscard]] std::optional<std::string> read_file(std::filesystem::path const& file) {
            std::ifstream in(file, std::ios::binary);
            if (!in) {
                return std::nullopt;
            }
            std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            return data;
        }

        [[nodiscard]] std::optional<std::filesystem::path> find_subtitle(
          std::filesystem::path const& dir,
          std::string_view const       format) {
            std::error_code ec;
            for (auto const& entry : std::filesystem::directory_iterator(dir, ec)) {
                if (ec) {
                    break;
                }
                if (entry.is_regular_file(ec) && entry.path().extension() == "." + std::string(format)) {
                    return entry.path();
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] bool
        run_yt_dlp(std::vector<std::string> const& base, std::string_view const browser, bool const verbose) {
            std::vector<std::string> argv = base;
            if (!browser.empty()) {
                argv.emplace_back("--cookies-from-browser");
                argv.emplace_back(browser);
            }
            process::command command{argv};
            command.discard_stdout();
            if (!verbose) {
                command.discard_stderr();
            }
            auto const result = command.run();
            return result.exit_code == 0;
        }

        [[nodiscard]] std::optional<std::string>
        from_transcript_api(std::string_view const url, std::string const& lang, std::string const& format) {
            if (!process::tool_exists("youtube_transcript_api")) {
                return std::nullopt;
            }
            auto const id = youtube_id_from_url(url);
            if (!id) {
                return std::nullopt;
            }

            std::string api_format = "srt";
            if (format == "vtt" || format == "webvtt") {
                api_format = "webvtt";
            } else if (format == "txt") {
                api_format = "text";
            } else if (format == "json" || format == "pretty") {
                api_format = format;
            }

            std::vector<std::string> const
                       argv{"youtube_transcript_api", "--languages", lang, "--format", api_format, *id};
            auto const result = process::command{argv}.capture_stderr().run();
            if (result.exit_code != 0 || result.stdout_data.empty()) {
                return std::nullopt;
            }
            if (result.stdout_data.contains("Could not retrieve a transcript") ||
                result.stdout_data.contains("This is most likely caused by:"))
            {
                return std::nullopt;
            }
            return result.stdout_data;
        }

    } // namespace

    std::optional<std::string> fetch_subtitles(std::string_view url, subtitle_options const& options) {
        temp_dir_guard const guard{.path = make_temp_dir()};

        std::string const lang     = yt_lang(options.lang);
        std::string const outtmpl  = (guard.path / "subtitles.%(ext)s").string();
        std::string const sub_lang = lang + "-orig," + lang;

        std::vector<std::string> const base{
          "yt-dlp",
          "--skip-download",
          "--sub-lang",
          sub_lang,
          "--write-auto-sub",
          "--convert-subs",
          options.format,
          "--sub-format",
          options.format,
          "-o",
          outtmpl,
          std::string(url),
        };

        std::array<std::string_view, 3> const browsers{std::string_view{}, "firefox", "vivaldi"};
        for (auto const browser : browsers) {
            if (!run_yt_dlp(base, browser, options.verbose)) {
                continue;
            }
            if (auto const file = find_subtitle(guard.path, options.format)) {
                if (auto text = read_file(*file)) {
                    return text;
                }
            }
        }

        if (auto text = from_transcript_api(url, lang, options.format)) {
            return text;
        }
        return std::nullopt;
    }

} // namespace dana::tools
