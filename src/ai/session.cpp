#include "session.hpp"

#include "../core/fs.hpp"
#include "../core/process.hpp"

#include <chrono>
#include <nlohmann/json.hpp>
#include <string>

namespace prompt::ai::session {

    namespace fs = std::filesystem;
    using nlohmann::json;

    namespace {

        std::string unwrap(json const& value) {
            if (value.is_string()) {
                std::string text = value.get<std::string>();
                if (!text.empty() && (text.front() == '{' || text.front() == '[')) {
                    json parsed = json::parse(text, nullptr, false);
                    if (!parsed.is_discarded() && parsed.is_object() && parsed.contains("value") &&
                        parsed["value"].is_string())
                    {
                        return parsed["value"].get<std::string>();
                    }
                }
                return text;
            }
            if (value.is_number_integer()) {
                return std::to_string(value.get<long long>());
            }
            if (value.is_number_unsigned()) {
                return std::to_string(value.get<unsigned long long>());
            }
            if (value.is_number_float()) {
                return std::to_string(value.get<double>());
            }
            if (value.is_boolean()) {
                return value.get<bool>() ? "true" : "false";
            }
            return {};
        }

    } // namespace

    std::optional<match> load(std::string_view target, options const& opts) {
        auto const exe = prompt::fs::bin_tool(
          "browser.session",
          prompt::fs::self_path().value_or(fs::path{}),
          prompt::fs::find_git_root());
        if (exe.empty()) {
            throw session_error("browser.session not found next to ai or on PATH");
        }

        std::vector<std::string> argv{exe.string(), std::string(target), "--format", "json"};
        for (auto const& browser : opts.browser) {
            argv.emplace_back("--browser");
            argv.push_back(browser);
        }
        if (!opts.profile.empty()) {
            argv.emplace_back("--profile");
            argv.push_back(opts.profile);
        }
        if (!opts.family.empty()) {
            argv.emplace_back("--family");
            argv.push_back(opts.family);
        }
        if (opts.open_only) {
            argv.emplace_back("--open-only");
        }
        if (opts.exact) {
            argv.emplace_back("--exact");
        }

        auto const result =
          process::run_command(argv, {}, {}, std::chrono::milliseconds(std::max(1, opts.timeout_seconds) * 1000));
        json data = json::parse(result.stdout_data, nullptr, false);
        if (data.is_discarded()) {
            throw session_error("browser.session returned no JSON");
        }
        if (!data.contains("matches") || !data["matches"].is_array() || data["matches"].empty()) {
            return std::nullopt;
        }
        auto const& first = data["matches"][0];
        if (!first.is_object()) {
            return std::nullopt;
        }

        match out;
        out.user_agent    = first.value("user_agent", "");
        out.cookie_header = first.value("cookie_header", "");
        out.browser       = first.value("browser", "");
        out.profile       = first.value("profile", "");
        out.path          = first.value("path", "");
        out.is_open       = first.value("is_open", false);
        out.cookies       = first.contains("cookies") ? first["cookies"] : json::array();
        out.storage       = first.contains("storage") ? first["storage"] : json::object();
        out.raw           = first;
        return out;
    }

    std::vector<std::pair<std::string, std::string>> build_headers(
      match const&                                     value,
      std::vector<std::pair<std::string, std::string>> extra) {
        std::vector<std::pair<std::string, std::string>> headers;
        if (!value.user_agent.empty()) {
            headers.emplace_back("User-Agent", value.user_agent);
        }
        if (!value.cookie_header.empty()) {
            headers.emplace_back("Cookie", value.cookie_header);
        }
        for (auto& header : extra) {
            headers.push_back(std::move(header));
        }
        return headers;
    }

    json const& localstorage(match const& value) {
        static json const empty = json::object();
        if (!value.storage.is_object() || !value.storage.contains("localstorage")) {
            return empty;
        }
        auto const& store = value.storage["localstorage"];
        return store.is_object() ? store : empty;
    }

    json const& sessionstorage(match const& value) {
        static json const empty = json::object();
        if (!value.storage.is_object() || !value.storage.contains("sessionstorage")) {
            return empty;
        }
        auto const& store = value.storage["sessionstorage"];
        return store.is_object() ? store : empty;
    }

    std::optional<std::string> storage_value(match const& value, std::vector<std::string_view> const& keys) {
        for (auto const key : keys) {
            for (auto const* store : {&localstorage(value), &sessionstorage(value)}) {
                auto const it = store->find(std::string(key));
                if (it != store->end()) {
                    return unwrap(*it);
                }
            }
        }
        return std::nullopt;
    }

    std::optional<std::string> cookie_value(match const& value, std::string_view name) {
        if (!value.cookies.is_array()) {
            return std::nullopt;
        }
        for (auto const& cookie : value.cookies) {
            if (cookie.is_object() && cookie.value("name", "") == name) {
                if (cookie.contains("value")) {
                    return unwrap(cookie["value"]);
                }
            }
        }
        return std::nullopt;
    }

} // namespace prompt::ai::session
