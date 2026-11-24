#include "yt_links.hpp"

#include "../ai/http.hpp"
#include "../core/process.hpp"

#include <cctype>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <format>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace dana::tools {

    namespace {

        [[nodiscard]] bool is_id_char(char const c) noexcept {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        }

        [[nodiscard]] bool is_letter(char const c) noexcept {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        }

        [[nodiscard]] bool is_space(char const c) noexcept {
            return std::isspace(static_cast<unsigned char>(c)) != 0;
        }

        [[nodiscard]] std::string_view trim(std::string_view text) noexcept {
            while (!text.empty() && is_space(text.front())) {
                text.remove_prefix(1);
            }
            while (!text.empty() && is_space(text.back())) {
                text.remove_suffix(1);
            }
            return text;
        }

        [[nodiscard]] bool id_at(std::string_view text, std::size_t pos, std::string& out) {
            if (pos + 11 > text.size()) {
                return false;
            }
            std::string_view const candidate = text.substr(pos, 11);
            if (!is_valid_youtube_id(candidate)) {
                return false;
            }
            out.assign(candidate);
            return true;
        }

        [[nodiscard]] std::optional<std::string> from_filename(std::string_view base) {
            std::string id;

            for (std::size_t i = 0; i < base.size(); ++i) {
                if (base[i] != '[') {
                    continue;
                }
                if (id_at(base, i + 1, id) && i + 12 < base.size() && base[i + 12] == ']') {
                    return id;
                }
            }
            for (std::size_t i = 0; i < base.size(); ++i) {
                if (base[i] != '(') {
                    continue;
                }
                if (id_at(base, i + 1, id) && i + 12 < base.size() && base[i + 12] == ')') {
                    return id;
                }
            }
            for (std::size_t i = 0; i < base.size(); ++i) {
                if (base[i] != '-') {
                    continue;
                }
                if (!id_at(base, i + 1, id)) {
                    continue;
                }
                std::size_t const dot = i + 12;
                if (dot >= base.size() || base[dot] != '.') {
                    continue;
                }
                std::string_view const ext = base.substr(dot + 1);
                if (!ext.empty() && ext.find('.') == std::string_view::npos) {
                    return id;
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] bool is_youtube_url(std::string_view url) noexcept {
            std::string_view rest;
            if (url.starts_with("https://")) {
                rest = url.substr(8);
            } else if (url.starts_with("http://")) {
                rest = url.substr(7);
            } else {
                return false;
            }
            if (rest.starts_with("www.")) {
                rest.remove_prefix(4);
            }
            return rest.starts_with("youtube.com/") || rest.starts_with("youtu.be/");
        }

        void collect_urls(std::string_view text, std::vector<std::string>& urls) {
            std::size_t pos = 0;
            while (pos < text.size()) {
                auto const  http  = text.find("http://", pos);
                auto const  https = text.find("https://", pos);
                std::size_t start = std::string::npos;
                if (http == std::string_view::npos) {
                    start = https;
                } else if (https == std::string_view::npos) {
                    start = http;
                } else {
                    start = http < https ? http : https;
                }
                if (start == std::string_view::npos) {
                    return;
                }
                std::size_t end = start;
                while (end < text.size() && text[end] != ' ') {
                    ++end;
                }
                urls.emplace_back(text.substr(start, end - start));
                pos = end;
            }
        }

        void collect_tag_urls(nlohmann::ordered_json const& tags, std::vector<std::string>& urls) {
            if (!tags.is_object()) {
                return;
            }
            for (auto const& [key, value] : tags.items()) {
                (void) key;
                if (value.is_string()) {
                    collect_urls(value.get_ref<std::string const&>(), urls);
                }
            }
        }

        [[nodiscard]] std::optional<std::string> from_ffprobe(std::filesystem::path const& file) {
            std::vector<std::string> const
              argv{"ffprobe", "-v", "error", "-print_format", "json", "-show_format", "-show_streams", file.string()};
            auto const result = process::command{argv}.discard_stderr().run();
            if (result.exit_code != 0 || result.stdout_data.empty()) {
                return std::nullopt;
            }

            auto const json = nlohmann::ordered_json::parse(result.stdout_data, nullptr, false);
            if (json.is_discarded() || !json.is_object()) {
                return std::nullopt;
            }

            std::vector<std::string> urls;
            if (auto const format = json.find("format"); format != json.end() && format->is_object()) {
                if (auto const tags = format->find("tags"); tags != format->end()) {
                    collect_tag_urls(*tags, urls);
                }
            }
            if (auto const streams = json.find("streams"); streams != json.end() && streams->is_array()) {
                for (auto const& stream : *streams) {
                    if (!stream.is_object()) {
                        continue;
                    }
                    if (auto const tags = stream.find("tags"); tags != stream.end()) {
                        collect_tag_urls(*tags, urls);
                    }
                }
            }

            for (auto const& url : urls) {
                if (is_youtube_url(url)) {
                    return url;
                }
            }
            if (!urls.empty()) {
                return urls.front();
            }
            return std::nullopt;
        }

        [[nodiscard]] std::string percent_decode(std::string_view text) {
            auto hex = [](char const c) -> int {
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

            std::string out;
            out.reserve(text.size());
            for (std::size_t i = 0; i < text.size(); ++i) {
                if (text[i] == '%' && i + 2 < text.size()) {
                    int const hi = hex(text[i + 1]);
                    int const lo = hex(text[i + 2]);
                    if (hi >= 0 && lo >= 0) {
                        out.push_back(static_cast<char>((static_cast<unsigned>(hi) << 4U) | static_cast<unsigned>(lo)));
                        i += 2;
                        continue;
                    }
                }
                out.push_back(text[i]);
            }
            return out;
        }

        enum struct [[nodiscard]] check_state : unsigned char {
            exists,
            not_found,
            unchecked
        };

        [[nodiscard]] check_state check_video(std::string_view url, int timeout) {
            auto const id = youtube_id_from_url(url);
            if (!id) {
                return check_state::unchecked;
            }
            try {
                ai::http_request request;
                request.url = std::format("https://www.youtube.com/oembed?url=https://www.youtube.com/watch?v={}", *id);
                request.method            = "GET";
                request.timeout_seconds   = timeout;
                request.retries           = 0;
                auto const [status, body] = ai::request_raw(request);
                (void) body;
                return status >= 200 && status < 400 ? check_state::exists : check_state::not_found;
            } catch (...) {
                return check_state::not_found;
            }
        }

        [[nodiscard]] std::string_view state_label(check_state state) noexcept {
            switch (state) {
                case check_state::exists: return "[exists]";
                case check_state::not_found: return "[not found]";
                case check_state::unchecked: return "[unchecked]";
            }
            return "[unchecked]";
        }

    } // namespace

    bool is_valid_youtube_id(std::string_view id) noexcept {
        if (id.size() != 11) {
            return false;
        }
        bool has_letter = false;
        for (char const c : id) {
            if (!is_id_char(c)) {
                return false;
            }
            has_letter = has_letter || is_letter(c);
        }
        return has_letter;
    }

    std::optional<std::string> youtube_id_from_url(std::string_view url) noexcept {
        std::string id;
        for (std::size_t i = 0; i + 3 <= url.size(); ++i) {
            if ((url[i] == '?' || url[i] == '&') && url[i + 1] == 'v' && url[i + 2] == '=' && id_at(url, i + 3, id)) {
                return id;
            }
        }
        if (auto const pos = url.find("youtu.be/"); pos != std::string_view::npos && id_at(url, pos + 9, id)) {
            return id;
        }
        return std::nullopt;
    }

    std::string clean_youtube_url(std::string_view url) {
        if (auto const id = youtube_id_from_url(url)) {
            return "https://youtu.be/" + *id;
        }
        return std::string(url);
    }

    yt_link_result extract_link(std::string_view input, yt_link_options const& options) {
        std::string_view text = trim(input);

        if (text.starts_with("file://")) {
            std::string const decoded = percent_decode(text.substr(7));
            text                      = decoded;
        }

        std::string url;
        if (text.starts_with("http://") || text.starts_with("https://")) {
            if (auto const id = youtube_id_from_url(text)) {
                url = "https://youtu.be/" + *id;
            } else {
                url = std::string(text);
            }
        } else {
            std::filesystem::path const file{std::string(text)};
            std::error_code             ec;
            if (!std::filesystem::is_regular_file(file, ec)) {
                if (options.verbose) {
                    (void) std::fprintf(stderr, "Skipping non-file: %s\n", std::string(text).c_str());
                }
                return {};
            }

            auto const from_name = [&] {
                return from_filename(file.filename().string());
            };
            auto const from_meta = [&] {
                return from_ffprobe(file);
            };

            std::optional<std::string> found;
            switch (options.method) {
                case yt_method::file: found = from_name(); break;
                case yt_method::meta: found = from_meta(); break;
                case yt_method::both:
                    found = from_name();
                    if (!found) {
                        found = from_meta();
                    }
                    break;
                case yt_method::meta_first:
                    found = from_meta();
                    if (!found) {
                        found = from_name();
                    }
                    break;
            }

            if (!found) {
                if (options.verbose) {
                    (void) std::fprintf(stderr, "No URL found for: %s\n", std::string(text).c_str());
                }
                return {.url = {}, .failed = true};
            }
            url = "https://youtu.be/" + *found;
        }

        std::string clean = clean_youtube_url(url);
        if (options.check) {
            auto const state  = check_video(clean, options.timeout);
            clean            += "  ";
            clean            += state_label(state);
        }
        return {.url = std::move(clean), .failed = false};
    }

} // namespace dana::tools
