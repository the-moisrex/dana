#pragma once

// Load a site's signed-in session (cookies + web storage) via
// bin/browser.session.  That tool stays external (like bin/tse): the native
// engine only consumes its JSON bundle.

#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prompt::ai::session {

    class session_error : public std::runtime_error {
      public:
        using std::runtime_error::runtime_error;
    };

    struct match {
        std::string    user_agent;
        std::string    cookie_header;
        std::string    browser;
        std::string    profile;
        std::string    path;
        bool           is_open = false;
        nlohmann::json cookies = nlohmann::json::array();
        nlohmann::json storage = nlohmann::json::object();
        nlohmann::json raw     = nlohmann::json::object();
    };

    struct options {
        std::vector<std::string> browser;
        std::string              profile;
        std::string              family;
        bool                     open_only       = false;
        bool                     exact           = false;
        int                      timeout_seconds = 120;
    };

    // Return the best browser.session match for `target` (or nullopt).
    std::optional<match> load(std::string_view target, options const& opts = {});

    // Request headers seeded from the session (UA + Cookie), plus `extra`.
    std::vector<std::pair<std::string, std::string>> build_headers(
      match const&                                     value,
      std::vector<std::pair<std::string, std::string>> extra = {});

    nlohmann::json const& localstorage(match const& value);
    nlohmann::json const& sessionstorage(match const& value);

    // Look up the first present key in localStorage then sessionStorage.  Stored
    // values that are a JSON object with a `value` member are unwrapped.
    std::optional<std::string> storage_value(match const& value, std::vector<std::string_view> const& keys);
    std::optional<std::string> cookie_value(match const& value, std::string_view name);

} // namespace prompt::ai::session
