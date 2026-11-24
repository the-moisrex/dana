#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dana::tools {

    struct [[nodiscard]] browser_profile {
        std::string name;
        std::string path;
        bool        is_default = false;
        bool        is_open    = false;
        int         pid        = 0;
        std::string cookies_db;
    };

    struct [[nodiscard]] browser_info {
        std::string                  id;
        std::string                  name;
        std::string                  family; // "chromium" or "firefox"
        std::string                  app_id;
        std::string                  executable;
        std::string                  config_dir;
        std::string                  version;
        std::string                  user_agent;
        bool                         is_system_default = false;
        std::vector<browser_profile> profiles;
    };

    struct [[nodiscard]] browser_cookie {
        std::string name;
        std::string value;
        std::string host;
        std::string path;
        long long   expires   = 0;
        bool        secure    = false;
        bool        http_only = false;
        int         same_site = 0;
    };

    struct [[nodiscard]] browser_storage_entry {
        std::string origin;
        std::string key;
        std::string value;
        std::string kind; // "localstorage" or "sessionstorage"
    };

    struct [[nodiscard]] session_match {
        std::string                        browser;
        std::string                        browser_name;
        std::string                        family;
        std::string                        profile;
        std::string                        path;
        bool                               is_open = false;
        std::string                        user_agent;
        std::string                        cookie_header;
        std::vector<browser_cookie>        cookies;
        std::vector<browser_storage_entry> storage;
    };

    struct [[nodiscard]] session_options {
        std::vector<std::string> browser;
        std::string              profile;
        std::string              family;
        bool                     open_only = false;
        bool                     exact     = false;
        std::string              ua;
    };

    // Discover installed browsers, their profiles and running instances.
    [[nodiscard]] std::vector<browser_info> collect_browsers();

    // Compose an authenticated session (cookies + web storage + User-Agent) for
    // a domain or URL. Returns the best match, or nullopt when nothing matches.
    [[nodiscard]] std::optional<session_match> find_session(std::string_view       target,
                                                            session_options const& options = {});

    // Read cookies and web storage for a single profile (used by the session
    // composer and exposed for focused callers).
    [[nodiscard]] std::vector<browser_cookie>
    read_cookies(browser_info const& browser, browser_profile const& profile, std::string_view domain, bool exact);
    [[nodiscard]] std::vector<browser_storage_entry> read_storage(
      browser_info const&    browser,
      browser_profile const& profile,
      std::string_view       kind,
      std::string_view       target,
      bool                   exact);

} // namespace dana::tools
