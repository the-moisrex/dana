#include "session.hpp"

#include "../core/fs.hpp"
#include "../core/process.hpp"
#include "../tools/browser.hpp"

#include <chrono>
#include <nlohmann/json.hpp>
#include <string>

namespace dana::ai::session {

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
                return std::to_string(value.get<std::int64_t>());
            }
            if (value.is_number_unsigned()) {
                return std::to_string(value.get<std::uint64_t>());
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
        tools::session_options session_options;
        session_options.browser   = opts.browser;
        session_options.profile   = opts.profile;
        session_options.family    = opts.family;
        session_options.open_only = opts.open_only;
        session_options.exact     = opts.exact;

        auto const found = tools::find_session(target, session_options);
        if (!found) {
            return std::nullopt;
        }

        match out;
        out.user_agent    = found->user_agent;
        out.cookie_header = found->cookie_header;
        out.browser       = found->browser;
        out.profile       = found->profile;
        out.path          = found->path;
        out.is_open       = found->is_open;

        out.cookies = json::array();
        for (auto const& cookie : found->cookies) {
            out.cookies.push_back({
              {     "name",      cookie.name},
              {    "value",     cookie.value},
              {     "host",      cookie.host},
              {     "path",      cookie.path},
              {  "expires",   cookie.expires},
              {   "secure",    cookie.secure},
              {"http_only", cookie.http_only},
              {"same_site", cookie.same_site}
            });
        }

        out.storage = {
          {  "localstorage", json::object()},
          {"sessionstorage", json::object()}
        };
        for (auto const& entry : found->storage) {
            out.storage[entry.kind][entry.key] = entry.value;
        }

        out.raw = {
          {      "browser",       found->browser},
          { "browser_name",  found->browser_name},
          {       "family",        found->family},
          {      "profile",       found->profile},
          {         "path",          found->path},
          {      "is_open",       found->is_open},
          {   "user_agent",    found->user_agent},
          {"cookie_header", found->cookie_header},
          {      "cookies",          out.cookies},
          {      "storage",          out.storage}
        };
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

} // namespace dana::ai::session
