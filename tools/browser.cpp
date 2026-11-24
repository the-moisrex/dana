#include "browser.hpp"

#include "../core/process.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <openssl/evp.h>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <sqlite3.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <zlib.h>

namespace dana::tools {

    namespace {

        namespace fs = std::filesystem;

        [[nodiscard]] constexpr unsigned byte_of(char value) noexcept {
            return static_cast<unsigned>(static_cast<unsigned char>(value));
        }

        // ---------------------------------------------------------------- strings

        [[nodiscard]] bool is_space(char const c) noexcept {
            return std::isspace(static_cast<unsigned char>(c)) != 0;
        }

        [[nodiscard]] std::string lower(std::string_view text) {
            std::string out(text);
            for (char& c : out) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return out;
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

        [[nodiscard]] std::vector<std::string_view> split_lines(std::string_view text) {
            std::vector<std::string_view> lines;
            std::size_t                   start = 0;
            while (start <= text.size()) {
                auto const nl = text.find('\n', start);
                lines.push_back(text.substr(start, nl == std::string_view::npos ? nl : nl - start));
                if (nl == std::string_view::npos) {
                    break;
                }
                start = nl + 1;
            }
            return lines;
        }

        [[nodiscard]] std::string read_text(fs::path const& path) {
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                return {};
            }
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }

        [[nodiscard]] std::string home_dir() {
            if (char const* home = std::getenv("HOME")) { // NOLINT(concurrency-mt-unsafe): read once at startup; no
                                                          // concurrent env mutation.
                return home;
            }
            return {};
        }

        [[nodiscard]] fs::path xdg_config() {
            char const* xdg = std::getenv("XDG_CONFIG_HOME"); // NOLINT(concurrency-mt-unsafe): read once at startup.
            if (xdg != nullptr && *xdg != '\0') {
                return xdg;
            }
            return fs::path(home_dir()) / ".config";
        }

        [[nodiscard]] std::string first_which(std::vector<std::string> const& names) {
            char const* path_env = std::getenv("PATH"); // NOLINT(concurrency-mt-unsafe): read once at startup; no
                                                        // concurrent env mutation.
            if (path_env == nullptr) {
                return {};
            }
            std::vector<std::string_view> dirs;
            std::string_view              path(path_env);
            std::size_t                   start = 0;
            while (start <= path.size()) {
                auto const colon = path.find(':', start);
                dirs.push_back(path.substr(start, colon == std::string_view::npos ? colon : colon - start));
                if (colon == std::string_view::npos) {
                    break;
                }
                start = colon + 1;
            }
            for (auto const& name : names) {
                for (auto const dir : dirs) {
                    if (dir.empty()) {
                        continue;
                    }
                    fs::path        candidate = fs::path(dir) / name;
                    std::error_code ec;
                    if (fs::is_regular_file(candidate, ec)) {
                        return candidate.string();
                    }
                }
            }
            return {};
        }

        [[nodiscard]] std::string first_dir(std::vector<std::string> const& dirs) {
            for (auto const& dir : dirs) {
                std::error_code ec;
                if (!dir.empty() && fs::is_directory(dir, ec)) {
                    return dir;
                }
            }
            return {};
        }

        [[nodiscard]] std::map<std::string, std::map<std::string, std::string>> parse_ini(fs::path const& path) {
            std::map<std::string, std::map<std::string, std::string>> sections;
            auto const                                                text = read_text(path);
            std::string                                               current;
            for (auto const line_view : split_lines(text)) {
                auto const line = trim(line_view);
                if (line.empty() || line.starts_with('#') || line.starts_with(';')) {
                    continue;
                }
                if (line.starts_with('[') && line.ends_with(']')) {
                    current = std::string(line.substr(1, line.size() - 2));
                    sections[current];
                    continue;
                }
                auto const eq = line.find('=');
                if (eq != std::string_view::npos && !current.empty()) {
                    sections[current][std::string(trim(line.substr(0, eq)))] = std::string(trim(line.substr(eq + 1)));
                }
            }
            return sections;
        }

        // ---------------------------------------------------------------- registry

        struct registry_entry {
            std::string              id;
            std::string              name;
            std::string              family;
            std::vector<std::string> exe_names;
            std::vector<std::string> config_dirs;
            std::string              app_id;
            std::vector<std::string> desktop_names;
            std::vector<std::string> proc_hints;
        };

        [[nodiscard]] std::vector<std::string>
        dirs_chromium(std::string_view sub, std::string_view flatpak = {}, std::string_view snap = {}) {
            std::vector<std::string> dirs{(xdg_config() / sub).string()};
            if (!flatpak.empty()) {
                dirs.push_back((fs::path(home_dir()) / ".var/app" / flatpak / "config" / sub).string());
            }
            if (!snap.empty()) {
                dirs.push_back((fs::path(home_dir()) / "snap" / snap / "common" / sub).string());
            }
            return dirs;
        }

        [[nodiscard]] std::vector<std::string>
        dirs_firefox(std::string_view app, std::string_view flatpak = {}, std::string_view snap = {}) {
            std::vector<std::string> dirs{(fs::path(home_dir()) / ".mozilla" / app).string(),
                                          (xdg_config() / "mozilla" / app).string()};
            if (!flatpak.empty()) {
                dirs.push_back((fs::path(home_dir()) / ".var/app" / flatpak / ".mozilla" / app).string());
            }
            if (!snap.empty()) {
                dirs.push_back((fs::path(home_dir()) / "snap" / snap / "common" / ".mozilla" / app).string());
            }
            return dirs;
        }

        [[nodiscard]] std::vector<registry_entry> registry() {
            std::vector<registry_entry> r;

            r.push_back({
              "chromium",
              "Chromium",
              "chromium",
              {"chromium", "chromium-browser"},
              dirs_chromium("chromium", "org.chromium.Chromium"),
              "chromium",
              {"chromium"},
              {"chromium"}
            });
            r.push_back(
              {"chromium-headless",
               "Chromium (headless)",
               "chromium",
               {},
               dirs_chromium("chromium-headless"),
               "chromium",
               {},
               {"chromium-headless"}});
            r.push_back({
              "chrome",
              "Google Chrome",
              "chromium",
              {"google-chrome", "google-chrome-stable"},
              dirs_chromium("google-chrome", "com.google.Chrome"),
              "chrome",
              {"google-chrome"},
              {"google-chrome-stable", "google-chrome"}
            });
            r.push_back({
              "brave",
              "Brave",
              "chromium",
              {"brave", "brave-browser"},
              dirs_chromium("BraveSoftware/Brave-Browser", "com.brave.Browser", "brave"),
              "brave",
              {"brave-browser"},
              {"brave"}
            });
            r.push_back({
              "edge",
              "Microsoft Edge",
              "chromium",
              {"microsoft-edge", "microsoft-edge-stable"},
              dirs_chromium("microsoft-edge", "com.microsoft.Edge"),
              "microsoft-edge",
              {"microsoft-edge"},
              {"microsoft-edge-stable", "microsoft-edge"}
            });
            r.push_back({
              "vivaldi",
              "Vivaldi",
              "chromium",
              {"vivaldi", "vivaldi-stable"},
              dirs_chromium("vivaldi", "com.vivaldi.Vivaldi"),
              "vivaldi",
              {"vivaldi-stable", "vivaldi"},
              {"vivaldi"}
            });
            r.push_back(
              {"opera",
               "Opera",
               "chromium",
               {"opera"},
               dirs_chromium("opera", "com.opera.Opera"),
               "opera",
               {"opera"},
               {"opera"}});
            r.push_back(
              {"ungoogled-chromium",
               "Ungoogled Chromium",
               "chromium",
               {"ungoogled-chromium"},
               dirs_chromium("ungoogled-chromium", "io.github.ungoogled_software.ungoogled_chromium"),
               "chromium",
               {},
               {"ungoogled-chromium"}});
            r.push_back(
              {"iridium", "Iridium", "chromium", {"iridium"}, dirs_chromium("iridium"), "iridium", {}, {"iridium"}});
            r.push_back(
              {"thorium", "Thorium", "chromium", {"thorium"}, dirs_chromium("thorium"), "thorium", {}, {"thorium"}});

            r.push_back(
              {"firefox",
               "Firefox",
               "firefox",
               {"firefox"},
               dirs_firefox("firefox", "org.mozilla.firefox", "firefox"),
               {},
               {"firefox"},
               {"firefox"}});
            r.push_back({
              "firefox-developer-edition",
              "Firefox Developer Edition",
              "firefox",
              {"firefox-developer-edition"},
              dirs_firefox("firefox", "org.mozilla.firefox", "firefox"),
              {},
              {"firefox-developer-edition"},
              {"firefox-developer-edition", "dev-edition"}
            });
            r.push_back(
              {"firefox-nightly",
               "Firefox Nightly",
               "firefox",
               {"firefox-nightly"},
               dirs_firefox("firefox", "org.mozilla.firefox", "firefox"),
               {},
               {"firefox-nightly"},
               {"firefox-nightly"}});
            r.push_back(
              {"librewolf",
               "LibreWolf",
               "firefox",
               {"librewolf"},
               dirs_firefox("librewolf", "io.gitlab.librewolf-community"),
               {},
               {"librewolf"},
               {"librewolf"}});
            r.push_back(
              {"waterfox",
               "Waterfox",
               "firefox",
               {"waterfox"},
               dirs_firefox("waterfox", "net.waterfox.waterfox"),
               {},
               {"waterfox"},
               {"waterfox"}});
            r.push_back({
              "zen",
              "Zen Browser",
              "firefox",
              {"zen-browser", "zen"},
              dirs_firefox("zen", "app.zen_browser.zen"),
              {},
              {"zen-browser", "zen"},
              {"zen-browser"}
            });
            r.push_back(
              {"floorp",
               "Floorp",
               "firefox",
               {"floorp"},
               dirs_firefox("floorp", "one.ablaze.floorp"),
               {},
               {"floorp"},
               {"floorp"}});
            return r;
        }

        // ---------------------------------------------------------------- version

        [[nodiscard]] std::optional<std::string> exec_target(fs::path const& path) {
            auto const text = read_text(path);
            if (!text.starts_with("#!")) {
                return std::nullopt;
            }
            static std::regex const re(R"(^\s*exec\s+(\S+))", std::regex::ECMAScript | std::regex::multiline);
            std::smatch             match;
            if (std::regex_search(text, match, re)) {
                return match[1].str();
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<fs::path> firefox_install_dir(std::string const& exe) {
            if (exe.empty()) {
                return std::nullopt;
            }
            std::error_code ec;
            fs::path        p = fs::canonical(exe, ec);
            if (ec) {
                p = exe;
            }
            std::vector<fs::path> cands{p.parent_path(), p.parent_path().parent_path()};
            if (auto const target = exec_target(p)) {
                fs::path const tp(*target);
                cands = {tp.parent_path(), tp, p.parent_path(), p.parent_path().parent_path()};
            }
            for (auto const& cand : cands) {
                std::error_code fec;
                if (fs::is_regular_file(cand / "application.ini", fec)) {
                    return cand;
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] std::string exe_version(std::string const& exe) {
            if (exe.empty()) {
                return {};
            }
            auto const result =
              process::command{
                {exe, "--version"}
            }
                .capture_stderr()
                .run();
            auto const text  = result.stdout_data.empty() ? result.stderr_data : result.stdout_data;
            auto const lines = split_lines(text);
            if (lines.empty()) {
                return {};
            }
            std::string line(lines.front());
            std::size_t pos = 0;
            while ((pos = line.find("Mozilla", pos)) != std::string::npos) {
                line.replace(pos, 7, " ");
            }
            std::vector<std::string> tokens;
            std::string              current;
            for (char const c : line) {
                if (is_space(c)) {
                    if (!current.empty()) {
                        tokens.push_back(std::move(current));
                        current.clear();
                    }
                } else {
                    current.push_back(c);
                }
            }
            if (!current.empty()) {
                tokens.push_back(std::move(current));
            }
            for (auto it = tokens.rbegin(); it != tokens.rend(); ++it) {
                if (!it->empty() && std::isdigit(static_cast<unsigned char>(it->front())) != 0) {
                    std::string out = *it;
                    while (!out.empty() && (out.front() == '(' || out.back() == ')')) {
                        if (out.front() == '(') {
                            out.erase(out.begin());
                        }
                        if (!out.empty() && out.back() == ')') {
                            out.pop_back();
                        }
                    }
                    return out;
                }
            }
            return {};
        }

        [[nodiscard]] std::string
        browser_version(registry_entry const& entry, std::string const& exe, std::string const& config_dir) {
            if (entry.family == "firefox") {
                if (auto const dir = firefox_install_dir(exe)) {
                    auto const ini = parse_ini(*dir / "application.ini");
                    if (auto const app = ini.find("App"); app != ini.end()) {
                        if (auto const version = app->second.find("Version"); version != app->second.end()) {
                            return version->second;
                        }
                    }
                }
                return exe_version(exe);
            }
            if (!config_dir.empty()) {
                auto const version = read_text(fs::path(config_dir) / "Last Version");
                if (!trim(version).empty()) {
                    return std::string(trim(version));
                }
            }
            return exe_version(exe);
        }

        [[nodiscard]] std::string platform_token() {
            return "X11; Linux x86_64";
        }

        [[nodiscard]] std::string
        build_user_agent(std::string const& browser_id, std::string const& family, std::string const& version) {
            std::string const plat = platform_token();
            if (family == "firefox") {
                std::string const rv = version.empty() ? "0" : version;
                return std::format("Mozilla/5.0 ({}; rv:{}) Gecko/20100101 Firefox/{}", plat, rv, rv);
            }
            std::string const major = version.empty() ? "0" : version.substr(0, version.find('.'));
            std::string       base  = std::format(
              "Mozilla/5.0 ({}) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/{}.0.0.0 Safari/537.36",
              plat,
              major);
            if (!version.empty()) {
                if (browser_id == "edge") {
                    base += std::format(" Edg/{}", version);
                } else if (browser_id == "opera") {
                    base += std::format(" OPR/{}", version);
                } else if (browser_id == "vivaldi") {
                    base += std::format(" Vivaldi/{}.0", major);
                }
            }
            return base;
        }

        // ---------------------------------------------------------------- profiles

        [[nodiscard]] std::vector<browser_profile> chromium_profiles(std::string const& config_dir) {
            fs::path const        config(config_dir);
            std::set<std::string> names;
            std::string           last_used;

            std::error_code ec;
            if (fs::is_regular_file(config / "Local State", ec)) {
                auto const data = nlohmann::json::parse(read_text(config / "Local State"), nullptr, false);
                if (!data.is_discarded() && data.contains("profile")) {
                    auto const& profile = data["profile"];
                    if (profile.contains("info_cache") && profile["info_cache"].is_object()) {
                        for (auto const& [key, _] : profile["info_cache"].items()) {
                            names.insert(key);
                        }
                    }
                    if (profile.contains("last_used") && profile["last_used"].is_string()) {
                        last_used = profile["last_used"].get<std::string>();
                    }
                }
            }

            for (auto const& entry : fs::directory_iterator(config, ec)) {
                if (ec) {
                    break;
                }
                std::error_code fec;
                if (entry.is_directory(fec) && fs::is_regular_file(entry.path() / "Preferences", fec)) {
                    names.insert(entry.path().filename().string());
                }
            }

            std::vector<browser_profile> profiles;
            for (auto const& name : names) {
                fs::path const  pdir    = config / name;
                fs::path        cookies = pdir / "Network" / "Cookies";
                std::error_code fec;
                if (!fs::is_regular_file(cookies, fec)) {
                    cookies = pdir / "Cookies";
                }
                browser_profile profile;
                profile.name       = name;
                profile.path       = pdir.string();
                profile.is_default = name == (last_used.empty() ? "Default" : last_used);
                if (fs::is_regular_file(cookies, fec)) {
                    profile.cookies_db = cookies.string();
                }
                profiles.push_back(std::move(profile));
            }
            return profiles;
        }

        [[nodiscard]] std::vector<browser_profile> firefox_profiles(std::string const& config_dir) {
            fs::path const config(config_dir);
            auto const     ini = parse_ini(config / "profiles.ini");

            std::set<std::string> install_defaults;
            for (auto const& [section, values] : ini) {
                if (section.starts_with("Install")) {
                    if (auto const it = values.find("Default"); it != values.end()) {
                        install_defaults.insert(it->second);
                    }
                }
            }

            std::vector<browser_profile> profiles;
            for (auto const& [section, values] : ini) {
                if (!section.starts_with("Profile")) {
                    continue;
                }
                auto const path_it = values.find("Path");
                if (path_it == values.end()) {
                    continue;
                }
                std::string const raw = path_it->second;
                bool const        relative =
                  !(values.contains("IsRelative") && values.at("IsRelative") == "0") && !fs::path(raw).is_absolute();
                fs::path const path         = relative ? config / raw : fs::path(raw);
                bool const     from_install = install_defaults.contains(raw);
                bool const     is_default = (values.contains("Default") && values.at("Default") == "1") || from_install;

                browser_profile profile;
                profile.name       = values.contains("Name") ? values.at("Name") : path.filename().string();
                profile.path       = path.string();
                profile.is_default = is_default;
                std::error_code ec;
                if (fs::is_regular_file(path / "cookies.sqlite", ec)) {
                    profile.cookies_db = (path / "cookies.sqlite").string();
                }
                profiles.push_back(std::move(profile));
            }

            if (profiles.empty()) {
                std::error_code ec;
                for (auto const& entry : fs::directory_iterator(config, ec)) {
                    if (ec) {
                        break;
                    }
                    std::error_code fec;
                    if (!entry.is_directory(fec)) {
                        continue;
                    }
                    if (!fs::is_regular_file(entry.path() / "cookies.sqlite", fec) &&
                        !fs::is_regular_file(entry.path() / "prefs.js", fec))
                    {
                        continue;
                    }
                    browser_profile profile;
                    profile.name       = entry.path().filename().string();
                    profile.path       = entry.path().string();
                    profile.is_default = profile.name.find("default") != std::string::npos;
                    if (fs::is_regular_file(entry.path() / "cookies.sqlite", fec)) {
                        profile.cookies_db = (entry.path() / "cookies.sqlite").string();
                    }
                    profiles.push_back(std::move(profile));
                }
            }
            return profiles;
        }

        // ---------------------------------------------------------------- processes

        struct proc_info {
            int                      pid = 0;
            std::string              exe;
            std::vector<std::string> args;
        };

        [[nodiscard]] std::vector<proc_info> iter_procs() {
            std::vector<proc_info> procs;
            std::error_code        ec;
            for (auto const& entry : fs::directory_iterator("/proc", ec)) {
                if (ec) {
                    break;
                }
                auto const name = entry.path().filename().string();
                if (name.empty() || !std::all_of(name.begin(),
                                                 name.end(),
                                                 [](char c) {
                                                     return std::isdigit(static_cast<unsigned char>(c)) != 0;
                                                 }))
                {
                    continue;
                }
                proc_info info;
                int       pid_value             = 0;
                auto const [pid_end, pid_error] = std::from_chars(name.data(), name.data() + name.size(), pid_value);
                if (pid_error != std::errc{} || pid_end != name.data() + name.size()) {
                    continue;
                }
                info.pid = pid_value;
                std::error_code lec;
                auto const      exe = fs::read_symlink(entry.path() / "exe", lec);
                if (!lec) {
                    info.exe = exe.string();
                }
                auto const  raw   = read_text(entry.path() / "cmdline");
                std::size_t start = 0;
                while (start < raw.size()) {
                    auto const nul = raw.find('\0', start);
                    auto const end = nul == std::string::npos ? raw.size() : nul;
                    if (end > start) {
                        info.args.emplace_back(raw.substr(start, end - start));
                    }
                    start = end == std::string::npos ? raw.size() : end + 1;
                }
                if (info.exe.empty() && info.args.empty()) {
                    continue;
                }
                procs.push_back(std::move(info));
            }
            return procs;
        }

        [[nodiscard]] bool
        is_main_process(std::string const& family, std::string const& exe, std::vector<std::string> const& args) {
            if (family == "chromium") {
                return std::none_of(args.begin(), args.end(), [](std::string const& a) {
                    return a.starts_with("--type=");
                });
            }
            bool const content = std::any_of(args.begin(), args.end(), [](std::string const& a) {
                return a == "-contentproc";
            });
            return !content && fs::path(exe).filename().string().find("crashhelper") == std::string::npos;
        }

        [[nodiscard]] registry_entry const* match_entry(
          std::vector<registry_entry> const& entries,
          std::string const&                 exe,
          std::vector<std::string> const&    args) {
            std::string const     hay      = lower(exe + " " + (args.empty() ? std::string{} : args.front()));
            registry_entry const* best     = nullptr;
            std::size_t           best_len = 0;
            for (auto const& entry : entries) {
                for (auto const& hint : entry.proc_hints) {
                    if (hay.find(hint) != std::string::npos && hint.size() > best_len) {
                        best     = &entry;
                        best_len = hint.size();
                    }
                }
            }
            return best;
        }

        [[nodiscard]] std::optional<std::string> arg_value(std::vector<std::string> const&         args,
                                                           std::initializer_list<std::string_view> names) {
            for (std::size_t i = 0; i < args.size(); ++i) {
                for (auto const name : names) {
                    if (args[i] == name && i + 1 < args.size()) {
                        return args[i + 1];
                    }
                    if (args[i].starts_with(std::string(name) + "=")) {
                        return args[i].substr(name.size() + 1);
                    }
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<std::string> chromium_proc_profile(
          registry_entry const&           entry,
          std::vector<std::string> const& args) {
            auto const  udd  = arg_value(args, {"--user-data-dir"});
            auto const  pdir = arg_value(args, {"--profile-directory"});
            std::string base;
            if (udd) {
                base = *udd;
            } else {
                base = first_dir(entry.config_dirs);
            }
            if (base.empty()) {
                return std::nullopt;
            }
            return (fs::path(base) / (pdir ? *pdir : std::string("Default"))).string();
        }

        [[nodiscard]] std::optional<std::string> firefox_profile_from_fds(int pid) {
            fs::path const  base = fs::path("/proc") / std::to_string(pid) / "fd";
            std::error_code ec;
            for (auto const& entry : fs::directory_iterator(base, ec)) {
                if (ec) {
                    break;
                }
                std::error_code lec;
                auto const      target = fs::read_symlink(entry.path(), lec);
                if (lec) {
                    continue;
                }
                auto const name = target.filename().string();
                if (name == "cookies.sqlite" || name == "places.sqlite" || name == "cookies.sqlite-wal") {
                    return target.parent_path().string();
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<std::string>
        firefox_proc_profile(registry_entry const& entry, int pid, std::vector<std::string> const& args) {
            (void) entry;
            if (auto const p = arg_value(args, {"-profile", "--profile"})) {
                return p;
            }
            return firefox_profile_from_fds(pid);
        }

        [[nodiscard]] std::string canonical_path(std::string const& path) {
            std::error_code ec;
            auto const      canonical = fs::canonical(path, ec);
            return ec ? path : canonical.string();
        }

        // ---------------------------------------------------------------- default

        [[nodiscard]] std::string normalize_name(std::string_view text) {
            std::string out;
            for (char const c : text) {
                if (c == ' ' || c == '_') {
                    out.push_back('-');
                } else {
                    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
                }
            }
            return out;
        }

        [[nodiscard]] std::string system_default_browser_id(std::vector<browser_info> const&   browsers,
                                                            std::vector<registry_entry> const& entries) {
            std::string                                   stem;
            std::array<std::vector<std::string>, 3> const commands{
              {
               {"xdg-settings", "get", "default-web-browser"},
               {"xdg-mime", "query", "default", "x-scheme-handler/https"},
               {"xdg-mime", "query", "default", "x-scheme-handler/http"},
               }
            };
            for (auto const& cmd : commands) {
                if (!process::tool_exists(cmd.front())) {
                    continue;
                }
                auto const result = process::command{cmd}.capture_stderr().run();
                if (result.exit_code == 0 && !trim(result.stdout_data).empty()) {
                    stem = std::string(trim(result.stdout_data));
                    break;
                }
            }
            if (stem.ends_with(".desktop")) {
                stem.resize(stem.size() - 8);
            }
            if (stem.empty()) {
                return {};
            }
            auto const  norm = normalize_name(stem);
            std::string best;
            std::size_t best_len = 0;
            for (auto const& browser : browsers) {
                auto const it = std::find_if(entries.begin(), entries.end(), [&](registry_entry const& e) {
                    return e.id == browser.id;
                });
                if (it == entries.end()) {
                    continue;
                }
                auto const& names = it->desktop_names.empty() ? std::vector<std::string>{it->id} : it->desktop_names;
                for (auto const& name : names) {
                    auto const n = normalize_name(name);
                    if (n.empty()) {
                        continue;
                    }
                    if (n == norm || norm.find(n) != std::string::npos || n.find(norm) != std::string::npos) {
                        if (n.size() > best_len) {
                            best     = browser.id;
                            best_len = n.size();
                        }
                    }
                }
            }
            return best;
        }

        // ---------------------------------------------------------------- sqlite

        struct db_value {
            int         type = SQLITE_NULL;
            std::string text;
            long long   integer = 0;
        };

        using db_row = std::unordered_map<std::string, db_value>;

        struct sqlite_db {
            sqlite3* handle = nullptr;
            fs::path dir;

            sqlite_db()                            = default;
            sqlite_db(sqlite_db const&)            = delete;
            sqlite_db& operator=(sqlite_db const&) = delete;

            ~sqlite_db() {
                if (handle != nullptr) {
                    sqlite3_close(handle);
                }
                if (!dir.empty()) {
                    std::error_code ec;
                    fs::remove_all(dir, ec);
                }
            }

            [[nodiscard]] bool open(fs::path const& path) {
                std::error_code ec;
                if (!fs::is_regular_file(path, ec)) {
                    return false;
                }
                static unsigned counter = 0;
                dir                     = fs::temp_directory_path() /
                                          std::format("prompt-browser-{}-{}", static_cast<long>(::getpid()), ++counter);
                fs::create_directories(dir, ec);
                auto const base = path.filename().string();
                for (std::string const suffix : {"", "-wal", "-shm"}) {
                    fs::path const src = path.string() + suffix;
                    if (fs::is_regular_file(src, ec)) {
                        fs::copy_file(src, dir / (base + suffix), fs::copy_options::overwrite_existing, ec);
                    }
                }
                return sqlite3_open_v2((dir / base).string().c_str(), &handle, SQLITE_OPEN_READONLY, nullptr) ==
                       SQLITE_OK;
            }

            [[nodiscard]] std::vector<db_row> query(std::string const& sql) {
                std::vector<db_row> rows;
                if (handle == nullptr) {
                    return rows;
                }
                sqlite3_stmt* stmt = nullptr;
                if (sqlite3_prepare_v2(handle, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
                    return rows;
                }
                int const columns = sqlite3_column_count(stmt);
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    db_row row;
                    for (int i = 0; i < columns; ++i) {
                        auto const name = sqlite3_column_name(stmt, i);
                        db_value   value;
                        value.type = sqlite3_column_type(stmt, i);
                        if (value.type == SQLITE_TEXT) {
                            auto const* text = sqlite3_column_text(stmt, i);
                            auto const  len  = sqlite3_column_bytes(stmt, i);
                            value.text.assign(reinterpret_cast<char const*>(text), static_cast<std::size_t>(len));
                        } else if (value.type == SQLITE_BLOB) {
                            auto const* blob = sqlite3_column_blob(stmt, i);
                            auto const  len  = sqlite3_column_bytes(stmt, i);
                            value.text.assign(reinterpret_cast<char const*>(blob), static_cast<std::size_t>(len));
                        } else if (value.type == SQLITE_INTEGER) {
                            value.integer = sqlite3_column_int64(stmt, i);
                        }
                        row[name] = std::move(value);
                    }
                    rows.push_back(std::move(row));
                }
                sqlite3_finalize(stmt);
                return rows;
            }
        };

        [[nodiscard]] db_value const* pick(db_row const& row, std::initializer_list<std::string_view> names) {
            for (auto const name : names) {
                auto const it = row.find(std::string(name));
                if (it != row.end()) {
                    return &it->second;
                }
            }
            return nullptr;
        }

        [[nodiscard]] std::string
        pick_text(db_row const& row, std::initializer_list<std::string_view> names, std::string_view fallback = {}) {
            if (auto const* value = pick(row, names)) {
                return value->text;
            }
            return std::string(fallback);
        }

        [[nodiscard]] long long
        pick_int(db_row const& row, std::initializer_list<std::string_view> names, long long fallback = 0) {
            if (auto const* value = pick(row, names)) {
                if (value->type == SQLITE_INTEGER) {
                    return value->integer;
                }
                if (!value->text.empty()) {
                    long long parsed = 0;
                    auto const [text_end, text_error] =
                      std::from_chars(value->text.data(), value->text.data() + value->text.size(), parsed);
                    if (text_error == std::errc{} && text_end == value->text.data() + value->text.size()) {
                        return parsed;
                    }
                }
            }
            return fallback;
        }

        // ---------------------------------------------------------------- crypto

        [[nodiscard]] std::string aes_cbc_decrypt(
          std::span<unsigned char const> key,
          std::span<unsigned char const> iv,
          std::string const&             ciphertext) {
            if (ciphertext.empty()) {
                return {};
            }
            EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
            if (ctx == nullptr) {
                return {};
            }
            std::string plain(ciphertext.size() + 16, '\0');
            int         out_len   = 0;
            int         final_len = 0;
            bool const  ok =
              EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr, key.data(), iv.data()) == 1 &&
              EVP_DecryptUpdate(
                ctx,
                reinterpret_cast<unsigned char*>(plain.data()),
                &out_len,
                reinterpret_cast<unsigned char const*>(ciphertext.data()),
                static_cast<int>(ciphertext.size())) == 1 &&
              EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(plain.data()) + out_len, &final_len) == 1;
            EVP_CIPHER_CTX_free(ctx);
            if (!ok) {
                return {};
            }
            plain.resize(static_cast<std::size_t>(out_len) + static_cast<std::size_t>(final_len));
            return plain;
        }

        [[nodiscard]] std::string unpad(std::string data) {
            if (data.empty()) {
                return data;
            }
            auto const n = byte_of(data.back());
            if (n >= 1 && n <= 16 && data.size() >= n) {
                bool const valid = std::all_of(data.end() - static_cast<std::ptrdiff_t>(n), data.end(), [n](char c) {
                    return byte_of(c) == n;
                });
                if (valid) {
                    data.resize(data.size() - n);
                }
            }
            return data;
        }

        [[nodiscard]] std::array<unsigned char, 16> chromium_key(std::string const& app_id) {
            std::string secret;
            if (process::tool_exists("secret-tool")) {
                auto const result =
                  process::command{
                    {"secret-tool", "lookup", "application", app_id}
                }
                    .capture_stderr()
                    .run();
                if (result.exit_code == 0 && !trim(result.stdout_data).empty()) {
                    secret = std::string(trim(result.stdout_data));
                }
            }
            if (secret.empty()) {
                secret = "peanuts";
            }
            std::array<unsigned char, 16> key{};
            PKCS5_PBKDF2_HMAC(
              secret.c_str(),
              static_cast<int>(secret.size()),
              reinterpret_cast<unsigned char const*>("saltysalt"),
              9,
              1,
              EVP_sha1(),
              static_cast<int>(key.size()),
              key.data());
            return key;
        }

        [[nodiscard]] std::string decode_chromium_plain(std::string const& plain) {
            auto const valid_utf8 = [](std::string const& text) {
                std::size_t i = 0;
                while (i < text.size()) {
                    auto const  c     = byte_of(text[i]);
                    std::size_t extra = 0;
                    if (c < 0x80) {
                        ++i;
                        continue;
                    }
                    if ((c & 0xE0U) == 0xC0U) {
                        extra = 1;
                    } else if ((c & 0xF0U) == 0xE0U) {
                        extra = 2;
                    } else if ((c & 0xF8U) == 0xF0U) {
                        extra = 3;
                    } else {
                        return false;
                    }
                    if (i + extra >= text.size()) {
                        return false;
                    }
                    for (std::size_t j = 1; j <= extra; ++j) {
                        if ((byte_of(text[i + j]) & 0xC0U) != 0x80U) {
                            return false;
                        }
                    }
                    i += extra + 1;
                }
                return true;
            };
            if (valid_utf8(plain)) {
                return plain;
            }
            if (plain.size() > 32) {
                auto const tail = plain.substr(32);
                if (valid_utf8(tail)) {
                    return tail;
                }
            }
            return plain;
        }

        [[nodiscard]] std::string decrypt_chromium(std::string const& blob, std::array<unsigned char, 16> const& key) {
            if (blob.empty()) {
                return {};
            }
            auto const prefix = std::string_view(blob).substr(0, 3);
            if (prefix == "v10" || prefix == "v11") {
                std::string ct = blob.substr(3);
                ct.resize(ct.size() / 16 * 16);
                if (ct.empty()) {
                    return {};
                }
                std::array<unsigned char, 16> const
                  iv{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
                return decode_chromium_plain(unpad(aes_cbc_decrypt(key, iv, ct)));
            }
            if (prefix == "v20") {
                return "<v20: app-bound, unsupported>";
            }
            return blob;
        }

        // ---------------------------------------------------------------- cookies

        [[nodiscard]] std::string domain_of(std::string_view target) {
            std::string t(trim(target));
            if (t.find("://") != std::string::npos) {
                t                = t.substr(t.find("://") + 3);
                auto const slash = t.find('/');
                if (slash != std::string::npos) {
                    t.resize(slash);
                }
                auto const query = t.find('?');
                if (query != std::string::npos) {
                    t.resize(query);
                }
                auto const at = t.rfind('@');
                if (at != std::string::npos) {
                    t = t.substr(at + 1);
                }
                auto const colon = t.find(':');
                if (colon != std::string::npos) {
                    t.resize(colon);
                }
                return lower(t);
            }
            auto const slash = t.find('/');
            if (slash != std::string::npos) {
                t.resize(slash);
            }
            auto const query = t.find('?');
            if (query != std::string::npos) {
                t.resize(query);
            }
            while (!t.empty() && t.back() == '.') {
                t.pop_back();
            }
            return lower(t);
        }

        [[nodiscard]] bool host_matches(std::string_view host, std::string_view domain, bool exact) {
            if (host.empty() || domain.empty()) {
                return false;
            }
            while (host.starts_with('.')) {
                host.remove_prefix(1);
            }
            auto const h = lower(host);
            auto const d = lower(domain);
            if (exact) {
                return h == d;
            }
            return h == d || h.ends_with("." + d) || d.ends_with("." + h);
        }

        [[nodiscard]] long long chromium_expiry(long long expires_utc) {
            if (expires_utc <= 0) {
                return 0;
            }
            auto const seconds = static_cast<double>(expires_utc) / 1'000'000.0 - 11'644'473'600.0;
            return seconds > 0 ? static_cast<long long>(seconds) : 0;
        }

        [[nodiscard]] std::vector<browser_cookie> read_chromium_cookies(
          browser_info const&    browser,
          browser_profile const& profile,
          std::string_view       domain,
          bool                   exact) {
            std::vector<browser_cookie> out;
            if (profile.cookies_db.empty()) {
                return out;
            }
            sqlite_db db;
            if (!db.open(profile.cookies_db)) {
                return out;
            }
            auto const key = chromium_key(browser.app_id.empty() ? "chromium" : browser.app_id);
            for (auto const& row : db.query("select * from cookies")) {
                auto const host = pick_text(row, {"host_key"});
                if (!host_matches(host, domain, exact)) {
                    continue;
                }
                browser_cookie cookie;
                cookie.name  = pick_text(row, {"name"});
                cookie.value = pick_text(row, {"value"});
                if (cookie.value.empty()) {
                    cookie.value = decrypt_chromium(pick_text(row, {"encrypted_value"}), key);
                }
                cookie.host      = host;
                cookie.path      = pick_text(row, {"path"}, "/");
                cookie.expires   = chromium_expiry(pick_int(row, {"expires_utc"}));
                cookie.secure    = pick_int(row, {"is_secure", "secure"}) != 0;
                cookie.http_only = pick_int(row, {"is_httponly", "httponly"}) != 0;
                cookie.same_site = static_cast<int>(pick_int(row, {"samesite", "same_site"}));
                out.push_back(std::move(cookie));
            }
            return out;
        }

        [[nodiscard]] std::vector<browser_cookie>
        read_firefox_cookies(browser_profile const& profile, std::string_view domain, bool exact) {
            std::vector<browser_cookie> out;
            if (profile.cookies_db.empty()) {
                return out;
            }
            sqlite_db db;
            if (!db.open(profile.cookies_db)) {
                return out;
            }
            for (auto const& row : db.query("select * from moz_cookies")) {
                auto const host = pick_text(row, {"host"});
                if (!host_matches(host, domain, exact)) {
                    continue;
                }
                browser_cookie cookie;
                cookie.name    = pick_text(row, {"name"});
                cookie.value   = pick_text(row, {"value"});
                cookie.host    = host;
                cookie.path    = pick_text(row, {"path"}, "/");
                cookie.expires = pick_int(row, {"expiry"});
                if (cookie.expires > 100'000'000'000LL) {
                    cookie.expires /= 1000;
                }
                cookie.secure    = pick_int(row, {"isSecure"}) != 0;
                cookie.http_only = pick_int(row, {"isHttpOnly"}) != 0;
                cookie.same_site = static_cast<int>(pick_int(row, {"sameSite"}));
                out.push_back(std::move(cookie));
            }
            return out;
        }

        // ---------------------------------------------------------------- storage

        [[nodiscard]] unsigned long long read_varint(std::string const& data, std::size_t& pos) {
            unsigned long long result = 0;
            unsigned           shift  = 0;
            while (pos < data.size()) {
                auto const b  = byte_of(data[pos++]);
                result       |= static_cast<unsigned long long>(b & 0x7FU) << shift;
                if ((b & 0x80U) == 0) {
                    break;
                }
                shift += 7;
            }
            return result;
        }

        [[nodiscard]] std::string snappy_decompress(std::string const& data) {
            std::size_t pos    = 0;
            auto const  length = read_varint(data, pos);
            std::string out;
            out.reserve(static_cast<std::size_t>(length));
            while (pos < data.size()) {
                auto const tag  = byte_of(data[pos++]);
                auto const kind = tag & 0x03U;
                if (kind == 0) {
                    std::size_t len = tag >> 2U;
                    if (len < 60) {
                        ++len;
                    } else {
                        auto const extra = len - 59;
                        len              = 0;
                        for (std::size_t i = 0; i < extra && pos < data.size(); ++i) {
                            len |= static_cast<std::size_t>(byte_of(data[pos++])) << (8U * i);
                        }
                        ++len;
                    }
                    if (pos + len > data.size()) {
                        break;
                    }
                    out.append(data, pos, len);
                    pos += len;
                } else {
                    std::size_t len    = 0;
                    std::size_t offset = 0;
                    if (kind == 1) {
                        len    = ((tag >> 2U) & 0x07U) + 4U;
                        offset = (static_cast<std::size_t>(tag >> 5U) << 8U) | byte_of(data[pos++]);
                    } else if (kind == 2) {
                        len     = (tag >> 2U) + 1U;
                        offset  = byte_of(data[pos]) | (static_cast<std::size_t>(byte_of(data[pos + 1])) << 8U);
                        pos    += 2;
                    } else {
                        len     = (tag >> 2U) + 1U;
                        offset  = byte_of(data[pos]) | (static_cast<std::size_t>(byte_of(data[pos + 1])) << 8U) |
                                  (static_cast<std::size_t>(byte_of(data[pos + 2])) << 16U) |
                                  (static_cast<std::size_t>(byte_of(data[pos + 3])) << 24U);
                        pos    += 4;
                    }
                    if (offset == 0 || offset > out.size()) {
                        break;
                    }
                    auto const start = out.size() - offset;
                    for (std::size_t i = 0; i < len; ++i) {
                        out.push_back(out[start + (i % offset)]);
                    }
                }
            }
            return out;
        }

        constexpr unsigned long long leveldb_magic = 0xDB47'7524'8B80'FB57ULL;

        [[nodiscard]] std::string read_block(std::string const& data, std::size_t offset, std::size_t size) {
            if (offset + size > data.size()) {
                return {};
            }
            std::string raw  = data.substr(offset, size);
            auto const  type = offset + size < data.size() ? byte_of(data[offset + size]) : static_cast<unsigned>(0);
            if (type == 1) {
                return snappy_decompress(raw);
            }
            if (type == 2) {
                uLongf      out_len = static_cast<uLongf>(raw.size() * 4 + 64);
                std::string out(out_len, '\0');
                auto const  rc = uncompress(
                  reinterpret_cast<Bytef*>(out.data()),
                  &out_len,
                  reinterpret_cast<Bytef const*>(raw.data()),
                  static_cast<uLong>(raw.size()));
                if (rc == Z_OK) {
                    out.resize(out_len);
                    return out;
                }
                return {};
            }
            return raw;
        }

        template <typename Fn>
        void iter_block(std::string const& block, Fn&& fn) {
            if (block.size() < 4) {
                return;
            }
            auto const num_restarts = static_cast<std::size_t>(
              byte_of(block[block.size() - 4]) | (static_cast<std::size_t>(byte_of(block[block.size() - 3])) << 8U) |
              (static_cast<std::size_t>(byte_of(block[block.size() - 2])) << 16U) |
              (static_cast<std::size_t>(byte_of(block[block.size() - 1])) << 24U));
            std::size_t restart_off = block.size() - 4 - num_restarts * 4;
            if (num_restarts == 0 || restart_off > block.size()) {
                restart_off = block.size();
            }
            std::size_t pos = 0;
            std::string last;
            while (pos < restart_off) {
                auto const shared     = static_cast<std::size_t>(read_varint(block, pos));
                auto const non_shared = static_cast<std::size_t>(read_varint(block, pos));
                auto const value_len  = static_cast<std::size_t>(read_varint(block, pos));
                if (pos + non_shared > block.size()) {
                    break;
                }
                std::string key = last.substr(0, std::min(shared, last.size()));
                key.append(block, pos, non_shared);
                pos += non_shared;
                if (pos + value_len > block.size()) {
                    break;
                }
                std::string value  = block.substr(pos, value_len);
                pos               += value_len;
                last               = key;
                fn(key, value);
            }
        }

        [[nodiscard]] std::unordered_map<std::string, std::string> read_leveldb(fs::path const& dirpath) {
            std::unordered_map<std::string, std::string> entries;
            std::vector<fs::path>                        tables;
            std::vector<fs::path>                        logs;
            std::error_code                              ec;
            for (auto const& entry : fs::directory_iterator(dirpath, ec)) {
                if (ec) {
                    break;
                }
                auto const ext = entry.path().extension().string();
                if (ext == ".ldb" || ext == ".sst") {
                    tables.push_back(entry.path());
                } else if (ext == ".log") {
                    logs.push_back(entry.path());
                }
            }
            std::ranges::sort(tables);
            std::ranges::sort(logs);

            for (auto const& path : tables) {
                auto const data = read_text(path);
                if (data.size() < 48) {
                    continue;
                }
                auto const         footer = data.substr(data.size() - 48);
                unsigned long long magic  = 0;
                for (std::size_t i = 0; i < 8; ++i) {
                    magic |= static_cast<unsigned long long>(byte_of(footer[40 + i])) << (8U * i);
                }
                if (magic != leveldb_magic) {
                    continue;
                }
                std::size_t pos = 0;
                (void) read_varint(footer, pos);
                (void) read_varint(footer, pos);
                auto const index_off  = static_cast<std::size_t>(read_varint(footer, pos));
                auto const index_size = static_cast<std::size_t>(read_varint(footer, pos));
                auto const index      = read_block(data, index_off, index_size);
                iter_block(index, [&](std::string const&, std::string const& handle) {
                    std::size_t hpos   = 0;
                    auto const  h_off  = static_cast<std::size_t>(read_varint(handle, hpos));
                    auto const  h_size = static_cast<std::size_t>(read_varint(handle, hpos));
                    auto const  body   = read_block(data, h_off, h_size);
                    iter_block(body, [&](std::string const& key, std::string const& value) {
                        entries[key.size() >= 8 ? key.substr(0, key.size() - 8) : key] = value;
                    });
                });
            }

            for (auto const& path : logs) {
                auto const               data       = read_text(path);
                constexpr std::size_t    block_size = 32'768;
                std::size_t              pos        = 0;
                std::vector<std::string> pending;
                while (pos < data.size()) {
                    std::size_t const block_end = std::min(pos + block_size, data.size());
                    std::size_t       p         = pos;
                    while (p + 7 <= block_end) {
                        auto const length = static_cast<std::size_t>(
                          byte_of(data[p + 4]) | (static_cast<std::size_t>(byte_of(data[p + 5])) << 8U));
                        auto const rtype = byte_of(data[p + 6]);
                        if (length == 0 && rtype == 0) {
                            break;
                        }
                        p += 7;
                        if (p + length > block_end) {
                            break;
                        }
                        std::string chunk  = data.substr(p, length);
                        p                 += length;
                        if (rtype == 1) {
                            // full
                            auto const& record = chunk;
                            std::size_t rpos   = 8;
                            if (record.size() < 12) {
                                continue;
                            }
                            auto const count = static_cast<std::size_t>(
                              byte_of(record[8]) | (static_cast<std::size_t>(byte_of(record[9])) << 8U) |
                              (static_cast<std::size_t>(byte_of(record[10])) << 16U) |
                              (static_cast<std::size_t>(byte_of(record[11])) << 24U));
                            rpos = 12;
                            for (std::size_t i = 0; i < count && rpos < record.size(); ++i) {
                                auto const item = byte_of(record[rpos++]);
                                auto const klen = static_cast<std::size_t>(read_varint(record, rpos));
                                if (rpos + klen > record.size()) {
                                    break;
                                }
                                std::string key  = record.substr(rpos, klen);
                                rpos            += klen;
                                if (item == 1) {
                                    auto const vlen = static_cast<std::size_t>(read_varint(record, rpos));
                                    if (rpos + vlen > record.size()) {
                                        break;
                                    }
                                    entries[key]  = record.substr(rpos, vlen);
                                    rpos         += vlen;
                                } else {
                                    entries.erase(key);
                                }
                            }
                        } else if (rtype == 2) {
                            pending = {std::move(chunk)};
                        } else if (rtype == 3) {
                            pending.push_back(std::move(chunk));
                        } else if (rtype == 4) {
                            pending.push_back(std::move(chunk));
                            std::string record;
                            for (auto const& part : pending) {
                                record += part;
                            }
                            pending.clear();
                            if (record.size() < 12) {
                                continue;
                            }
                            auto const count = static_cast<std::size_t>(
                              byte_of(record[8]) | (static_cast<std::size_t>(byte_of(record[9])) << 8U) |
                              (static_cast<std::size_t>(byte_of(record[10])) << 16U) |
                              (static_cast<std::size_t>(byte_of(record[11])) << 24U));
                            std::size_t rpos = 12;
                            for (std::size_t i = 0; i < count && rpos < record.size(); ++i) {
                                auto const item = byte_of(record[rpos++]);
                                auto const klen = static_cast<std::size_t>(read_varint(record, rpos));
                                if (rpos + klen > record.size()) {
                                    break;
                                }
                                std::string key  = record.substr(rpos, klen);
                                rpos            += klen;
                                if (item == 1) {
                                    auto const vlen = static_cast<std::size_t>(read_varint(record, rpos));
                                    if (rpos + vlen > record.size()) {
                                        break;
                                    }
                                    entries[key]  = record.substr(rpos, vlen);
                                    rpos         += vlen;
                                } else {
                                    entries.erase(key);
                                }
                            }
                        }
                    }
                    pos += block_size;
                }
            }
            return entries;
        }

        [[nodiscard]] std::string decode_storage_string(unsigned char marker, std::string const& raw) {
            if (marker == 0) {
                std::string out;
                out.reserve(raw.size() / 2);
                for (std::size_t i = 0; i + 1 < raw.size(); i += 2) {
                    auto const lo = byte_of(raw[i]);
                    auto const hi = byte_of(raw[i + 1]);
                    auto const cp = lo | (hi << 8U);
                    if (cp < 0x80U) {
                        out.push_back(static_cast<char>(cp));
                    } else if (cp < 0x800U) {
                        out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
                        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
                    } else {
                        out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
                        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
                        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
                    }
                }
                return out;
            }
            return raw;
        }

        struct parsed_origin {
            std::string        scheme;
            std::string        host;
            std::optional<int> port;
        };

        [[nodiscard]] parsed_origin parse_origin(std::string_view origin) {
            parsed_origin out;
            auto const    scheme_end = origin.find("://");
            std::string   rest;
            if (scheme_end != std::string_view::npos) {
                out.scheme = lower(origin.substr(0, scheme_end));
                rest       = std::string(origin.substr(scheme_end + 3));
            } else {
                rest = std::string(origin);
            }
            auto const slash = rest.find('/');
            if (slash != std::string::npos) {
                rest.resize(slash);
            }
            auto const colon = rest.rfind(':');
            if (colon != std::string::npos && rest.find(']') == std::string::npos) {
                auto const port_text = rest.substr(colon + 1);
                if (!port_text.empty() &&
                    std::all_of(port_text.begin(),
                                port_text.end(),
                                [](char c) {
                                    return std::isdigit(static_cast<unsigned char>(c)) != 0;
                                }))
                {
                    int port_value = 0;
                    auto const [port_end, port_error] =
                      std::from_chars(port_text.data(), port_text.data() + port_text.size(), port_value);
                    if (port_error == std::errc{} && port_end == port_text.data() + port_text.size()) {
                        out.port = port_value;
                        rest.resize(colon);
                    }
                }
            }
            out.host = lower(rest);
            return out;
        }

        [[nodiscard]] bool origin_matches(std::string const& origin, std::string_view domain, bool exact) {
            auto const parsed = parse_origin(origin);
            if (!host_matches(parsed.host, domain, exact)) {
                return false;
            }
            return true;
        }

        [[nodiscard]] std::vector<browser_storage_entry> read_chromium_storage(
          browser_profile const& profile,
          std::string_view       kind,
          std::string_view       domain,
          bool                   exact) {
            std::vector<browser_storage_entry> out;
            fs::path const                     base =
              fs::path(profile.path) / (kind == "localstorage" ? "Local Storage" : "Session Storage");
            fs::path        leveldb = base / "leveldb";
            std::error_code ec;
            if (!fs::is_directory(leveldb, ec)) {
                if (fs::is_directory(base, ec)) {
                    leveldb = base;
                } else {
                    return out;
                }
            }
            auto const entries = read_leveldb(leveldb);
            for (auto const& [key, value] : entries) {
                if (key == "VERSION" || key.starts_with("META:") || !key.starts_with('_')) {
                    continue;
                }
                auto const nul = key.find('\0', 1);
                if (nul == std::string::npos || nul <= 1) {
                    continue;
                }
                auto const origin = key.substr(1, nul - 1);
                if (!origin_matches(origin, domain, exact)) {
                    continue;
                }
                auto const tail = key.substr(nul + 1);
                if (tail.empty()) {
                    continue;
                }
                browser_storage_entry entry;
                entry.origin = origin;
                entry.key    = decode_storage_string(static_cast<unsigned char>(byte_of(tail[0])), tail.substr(1));
                entry.value =
                  value.empty() ? std::string{}
                                : decode_storage_string(static_cast<unsigned char>(byte_of(value[0])), value.substr(1));
                entry.kind = std::string(kind);
                out.push_back(std::move(entry));
            }
            return out;
        }

        [[nodiscard]] std::string origin_from_dir(std::string name) {
            auto const caret = name.find('^');
            if (caret != std::string::npos) {
                name.resize(caret);
            }
            auto const plus = name.find("+++");
            if (plus == std::string::npos) {
                return {};
            }
            auto const scheme = name.substr(0, plus);
            auto       rest   = name.substr(plus + 3);
            if (scheme == "file") {
                std::string path;
                for (char const c : rest) {
                    path.push_back(c == '+' ? '/' : c);
                }
                return "file://" + path;
            }
            auto const colon = rest.rfind('+');
            if (colon != std::string::npos) {
                auto const port = rest.substr(colon + 1);
                if (!port.empty() && std::all_of(port.begin(),
                                                 port.end(),
                                                 [](char c) {
                                                     return std::isdigit(static_cast<unsigned char>(c)) != 0;
                                                 }))
                {
                    return scheme + "://" + rest.substr(0, colon) + ":" + port;
                }
            }
            return scheme + "://" + rest;
        }

        [[nodiscard]] std::string decode_firefox_value(
          db_value const& value,
          long long       utf16_length,
          long long       conversion_type,
          long long       compression_type) {
            if (value.type == SQLITE_TEXT) {
                return value.text;
            }
            std::string raw = value.text;
            if (compression_type != 0) {
                raw = snappy_decompress(raw);
            }
            if (conversion_type == 1) {
                return raw;
            }
            if (utf16_length > 0 && raw.size() == static_cast<std::size_t>(utf16_length) * 2) {
                std::string out;
                out.reserve(raw.size() / 2);
                for (std::size_t i = 0; i + 1 < raw.size(); i += 2) {
                    auto const cp = byte_of(raw[i]) | (byte_of(raw[i + 1]) << 8U);
                    if (cp < 0x80U) {
                        out.push_back(static_cast<char>(cp));
                    } else if (cp < 0x800U) {
                        out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
                        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
                    } else {
                        out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
                        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
                        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
                    }
                }
                return out;
            }
            return raw;
        }

        [[nodiscard]] std::vector<browser_storage_entry> read_firefox_storage(
          browser_profile const& profile,
          std::string_view       kind,
          std::string_view       domain,
          bool                   exact) {
            std::vector<browser_storage_entry> out;
            std::vector<std::string>           roots;
            if (kind == "localstorage") {
                roots = {(fs::path(profile.path) / "storage" / "default").string(),
                         (fs::path(profile.path) / "storage" / "permanent").string()};
            } else {
                roots = {(fs::path(profile.path) / "storage" / "temporary").string()};
            }
            for (auto const& root : roots) {
                std::error_code       ec;
                std::vector<fs::path> origin_dirs;
                for (auto const& entry : fs::directory_iterator(root, ec)) {
                    if (ec) {
                        break;
                    }
                    if (entry.is_directory(ec)) {
                        origin_dirs.push_back(entry.path());
                    }
                }
                std::ranges::sort(origin_dirs);
                for (auto const& dir : origin_dirs) {
                    auto const db_path = dir / "ls" / "data.sqlite";
                    if (!fs::is_regular_file(db_path, ec)) {
                        continue;
                    }
                    auto const origin = origin_from_dir(dir.filename().string());
                    if (!origin_matches(origin, domain, exact)) {
                        continue;
                    }
                    sqlite_db db;
                    if (!db.open(db_path)) {
                        continue;
                    }
                    for (auto const& row :
                         db.query("select key, value, utf16_length, conversion_type, compression_type from data"))
                    {
                        browser_storage_entry entry;
                        entry.origin = origin;
                        entry.key    = pick_text(row, {"key"});
                        if (auto const* value = pick(row, {"value"})) {
                            entry.value = decode_firefox_value(
                              *value,
                              pick_int(row, {"utf16_length"}),
                              pick_int(row, {"conversion_type"}),
                              pick_int(row, {"compression_type"}));
                        }
                        entry.kind = std::string(kind);
                        out.push_back(std::move(entry));
                    }
                }
            }
            return out;
        }

    } // namespace

    std::vector<browser_info> collect_browsers() {
        auto const                entries = registry();
        std::vector<browser_info> browsers;

        for (auto const& entry : entries) {
            auto const exe        = first_which(entry.exe_names);
            auto const config_dir = first_dir(entry.config_dirs);
            if (exe.empty() && config_dir.empty()) {
                continue;
            }
            browser_info info;
            info.id         = entry.id;
            info.name       = entry.name;
            info.family     = entry.family;
            info.app_id     = entry.app_id;
            info.executable = exe;
            info.config_dir = config_dir;
            info.version    = browser_version(entry, exe, config_dir);
            info.user_agent = build_user_agent(entry.id, entry.family, info.version);
            if (!config_dir.empty()) {
                info.profiles =
                  entry.family == "chromium" ? chromium_profiles(config_dir) : firefox_profiles(config_dir);
            }
            browsers.push_back(std::move(info));
        }

        std::unordered_set<std::string> installed_dirs;
        for (auto const& browser : browsers) {
            if (!browser.executable.empty()) {
                installed_dirs.insert(browser.config_dir);
            }
        }
        std::erase_if(browsers, [&](browser_info const& browser) {
            return browser.executable.empty() && installed_dirs.contains(browser.config_dir);
        });

        // Running instances.
        std::unordered_map<std::string, int> open_paths;
        for (auto const& proc : iter_procs()) {
            auto const* entry = match_entry(entries, proc.exe, proc.args);
            if (entry == nullptr || !is_main_process(entry->family, proc.exe, proc.args)) {
                continue;
            }
            auto const found = std::find_if(browsers.begin(), browsers.end(), [&](browser_info const& b) {
                return b.id == entry->id;
            });
            if (found == browsers.end()) {
                continue;
            }
            std::optional<std::string> path;
            if (entry->family == "chromium") {
                path = chromium_proc_profile(*entry, proc.args);
            } else {
                path = firefox_proc_profile(*entry, proc.pid, proc.args);
            }
            if (path) {
                open_paths[canonical_path(*path)] = proc.pid;
            }
        }
        for (auto& browser : browsers) {
            for (auto& profile : browser.profiles) {
                auto const it = open_paths.find(canonical_path(profile.path));
                if (it != open_paths.end()) {
                    profile.is_open = true;
                    profile.pid     = it->second;
                }
            }
        }

        auto const default_id = system_default_browser_id(browsers, entries);
        if (!default_id.empty()) {
            for (auto& browser : browsers) {
                if (browser.id == default_id) {
                    browser.is_system_default = true;
                }
            }
        }
        return browsers;
    }

    std::vector<browser_cookie>
    read_cookies(browser_info const& browser, browser_profile const& profile, std::string_view domain, bool exact) {
        if (browser.family == "chromium") {
            return read_chromium_cookies(browser, profile, domain, exact);
        }
        return read_firefox_cookies(profile, domain, exact);
    }

    std::vector<browser_storage_entry> read_storage(
      browser_info const&    browser,
      browser_profile const& profile,
      std::string_view       kind,
      std::string_view       target,
      bool                   exact) {
        auto const domain = domain_of(target);
        if (browser.family == "chromium") {
            return read_chromium_storage(profile, kind, domain, exact);
        }
        return read_firefox_storage(profile, kind, domain, exact);
    }

    std::optional<session_match> find_session(std::string_view target, session_options const& options) {
        auto const domain = domain_of(target);
        if (domain.empty()) {
            return std::nullopt;
        }

        auto browsers = collect_browsers();

        struct candidate {
            browser_info const*    browser = nullptr;
            browser_profile const* profile = nullptr;
        };

        std::vector<candidate>          candidates;
        std::unordered_set<std::string> seen;
        for (auto const& browser : browsers) {
            if (!options.browser.empty()) {
                bool const wanted =
                  std::any_of(options.browser.begin(), options.browser.end(), [&](std::string const& name) {
                      return lower(name) == lower(browser.id) || lower(name) == lower(browser.name);
                  });
                if (!wanted) {
                    continue;
                }
            }
            if (!options.family.empty() && options.family != browser.family) {
                continue;
            }
            for (auto const& profile : browser.profiles) {
                if (options.open_only && !profile.is_open) {
                    continue;
                }
                if (!options.profile.empty()) {
                    auto const wanted = lower(options.profile);
                    if (wanted != lower(profile.name) && wanted != lower(fs::path(profile.path).filename().string()) &&
                        wanted != lower(profile.path))
                    {
                        continue;
                    }
                }
                auto const real = canonical_path(profile.path);
                if (!seen.insert(real).second) {
                    continue;
                }
                candidates.push_back({&browser, &profile});
            }
        }

        std::ranges::stable_sort(candidates, [](candidate const& a, candidate const& b) {
            if (a.profile->is_open != b.profile->is_open) {
                return a.profile->is_open;
            }
            if (a.profile->is_default != b.profile->is_default) {
                return a.profile->is_default;
            }
            return a.profile->name < b.profile->name;
        });

        std::vector<session_match> matches;
        for (auto const& candidate : candidates) {
            session_match match;
            match.browser      = candidate.browser->id;
            match.browser_name = candidate.browser->name;
            match.family       = candidate.browser->family;
            match.profile      = candidate.profile->name;
            match.path         = candidate.profile->path;
            match.is_open      = candidate.profile->is_open;
            match.user_agent   = options.ua.empty() ? candidate.browser->user_agent : options.ua;
            match.cookies      = read_cookies(*candidate.browser, *candidate.profile, domain, options.exact);
            auto local = read_storage(*candidate.browser, *candidate.profile, "localstorage", target, options.exact);
            auto session =
              read_storage(*candidate.browser, *candidate.profile, "sessionstorage", target, options.exact);
            match.storage.insert(match.storage.end(), local.begin(), local.end());
            match.storage.insert(match.storage.end(), session.begin(), session.end());
            if (match.cookies.empty() && match.storage.empty()) {
                continue;
            }
            for (auto const& cookie : match.cookies) {
                if (!match.cookie_header.empty()) {
                    match.cookie_header += "; ";
                }
                match.cookie_header += cookie.name + "=" + cookie.value;
            }
            matches.push_back(std::move(match));
        }

        if (matches.empty()) {
            return std::nullopt;
        }
        std::ranges::stable_sort(matches, [](session_match const& a, session_match const& b) {
            if (a.is_open != b.is_open) {
                return a.is_open;
            }
            return !a.cookies.empty() && b.cookies.empty();
        });
        return matches.front();
    }

} // namespace dana::tools
