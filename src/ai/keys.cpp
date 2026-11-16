#include "keys.hpp"

#include "../core/fs.hpp"
#include "../core/process.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace prompt::ai::keys {

    namespace {

        using nlohmann::json;
        namespace fs = std::filesystem;

        std::string env_value(std::string_view name) {
            std::string key(name);
            char const* raw = std::getenv(key.c_str());
            return raw == nullptr ? std::string{} : std::string(raw);
        }

        bool truthy(std::string_view value) {
            return value == "1" || value == "true" || value == "yes" || value == "on" || value == "TRUE" ||
                   value == "YES" || value == "ON";
        }

        fs::path home_dir() {
            std::string const home = env_value("HOME");
            return home.empty() ? fs::path("/") : fs::path(home);
        }

        fs::path xdg(std::string_view name, std::vector<std::string> const& fallback) {
            std::string value = env_value(name);
            if (!value.empty()) {
                fs::path resolved(std::move(value));
                return resolved;
            }
            fs::path out = home_dir();
            for (auto const& part : fallback) {
                out /= part;
            }
            return out;
        }

        fs::path opencode_data_dir() {
            return xdg("XDG_DATA_HOME", {".local", "share"}) / "opencode";
        }

        fs::path opencode_config_dir() {
            return xdg("XDG_CONFIG_HOME", {".config"}) / "opencode";
        }

        std::map<std::string, std::string> parse_env_file(fs::path const& path) {
            std::map<std::string, std::string> values;
            std::ifstream                      stream(path);
            if (!stream) {
                return values;
            }
            std::string line;
            while (std::getline(stream, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
                    line.pop_back();
                }
                std::size_t start = line.find_first_not_of(" \t");
                if (start == std::string::npos) {
                    continue;
                }
                line = line.substr(start);
                if (line.empty() || line.front() == '#') {
                    continue;
                }
                if (line.starts_with("export ")) {
                    line  = line.substr(7);
                    start = line.find_first_not_of(" \t");
                    if (start != std::string::npos) {
                        line = line.substr(start);
                    }
                }
                auto const eq = line.find('=');
                if (eq == std::string::npos) {
                    continue;
                }
                std::string name = line.substr(0, eq);
                while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
                    name.pop_back();
                }
                if (name.empty()) {
                    continue;
                }
                bool valid = true;
                for (char c : name) {
                    if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '_') {
                        valid = false;
                    }
                }
                if (!valid) {
                    continue;
                }
                std::string value = line.substr(eq + 1);
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                    value.erase(value.begin());
                }
                if (
                  value.size() >= 2 && value.front() == value.back() && (value.front() == '\'' || value.front() == '"'))
                {
                    value = value.substr(1, value.size() - 2);
                } else {
                    auto const comment = value.find(" #");
                    if (comment != std::string::npos) {
                        value = value.substr(0, comment);
                    }
                    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                        value.pop_back();
                    }
                }
                values[name] = value;
            }
            return values;
        }

        std::vector<fs::path> dotenv_paths() {
            std::vector<fs::path> paths;
            std::set<std::string> seen;
            auto                  add = [&](fs::path path) {
                std::error_code   ec;
                auto const        canonical = fs::weakly_canonical(path, ec);
                std::string const key       = ec ? path.string() : canonical.string();
                if (seen.insert(key).second) {
                    paths.push_back(std::move(path));
                }
            };

            std::string const explicit_path = env_value("AI_ENV_FILE");
            if (!explicit_path.empty()) {
                add(fs::path(explicit_path));
                return paths;
            }

            fs::path stop = home_dir();
            if (auto const root = prompt::fs::find_git_root()) {
                stop = *root;
            }

            fs::path here = fs::current_path();
            for (;;) {
                if (here.parent_path() == here && here != stop) {
                    break;
                }
                add(here / ".env");
                add(here / ".env.local");
                if (here == stop) {
                    break;
                }
                if (here.parent_path() == here) {
                    break;
                }
                here = here.parent_path();
            }
            add(opencode_config_dir().parent_path() / "ai" / ".env");
            add(home_dir() / ".env");
            return paths;
        }

        std::optional<std::pair<std::string, std::string>> from_dotenv(std::span<std::string const> names) {
            for (auto const& path : dotenv_paths()) {
                std::error_code ec;
                if (!fs::is_regular_file(path, ec)) {
                    continue;
                }
                auto const values = parse_env_file(path);
                for (auto const& name : names) {
                    auto const it = values.find(name);
                    if (it != values.end() && !it->second.empty()) {
                        return std::pair{it->second, "dotenv:" + path.string()};
                    }
                }
            }
            return std::nullopt;
        }

        std::string strip_jsonc(std::string_view text) {
            std::string out;
            out.reserve(text.size());
            bool in_string = false;
            bool escape    = false;
            for (std::size_t i = 0; i < text.size();) {
                char const c = text[i];
                if (in_string) {
                    out.push_back(c);
                    if (escape) {
                        escape = false;
                    } else if (c == '\\') {
                        escape = true;
                    } else if (c == '"') {
                        in_string = false;
                    }
                    ++i;
                    continue;
                }
                if (c == '"') {
                    in_string = true;
                    out.push_back(c);
                    ++i;
                    continue;
                }
                if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
                    while (i < text.size() && text[i] != '\n') {
                        ++i;
                    }
                    continue;
                }
                if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
                    i += 2;
                    while (i + 1 < text.size() && (text[i] != '*' || text[i + 1] != '/')) {
                        ++i;
                    }
                    i += 2;
                    continue;
                }
                out.push_back(c);
                ++i;
            }
            std::string result;
            result.reserve(out.size());
            in_string = false;
            escape    = false;
            for (std::size_t i = 0; i < out.size(); ++i) {
                char const c = out[i];
                if (in_string) {
                    result.push_back(c);
                    if (escape) {
                        escape = false;
                    } else if (c == '\\') {
                        escape = true;
                    } else if (c == '"') {
                        in_string = false;
                    }
                    continue;
                }
                if (c == '"') {
                    in_string = true;
                    result.push_back(c);
                    continue;
                }
                if (c == ',') {
                    std::size_t j = i + 1;
                    while (j < out.size() && (out[j] == ' ' || out[j] == '\t' || out[j] == '\r' || out[j] == '\n')) {
                        ++j;
                    }
                    if (j < out.size() && (out[j] == '}' || out[j] == ']')) {
                        continue;
                    }
                }
                result.push_back(c);
            }
            return result;
        }

        std::string expand_env(std::string value) {
            std::string out;
            for (std::size_t i = 0; i < value.size();) {
                if (value.compare(i, 5, "{env:") == 0) {
                    auto const end = value.find('}', i + 5);
                    if (end != std::string::npos) {
                        out += env_value(value.substr(i + 5, end - (i + 5)));
                        i    = end + 1;
                        continue;
                    }
                }
                out.push_back(value[i]);
                ++i;
            }
            return out;
        }

        std::pair<std::map<std::string, std::string>, std::string> opencode_export() {
            for (auto const* const tool : {"opencode2", "opencode"}) {
                auto const exe =
                  prompt::fs::bin_tool(tool, prompt::fs::self_path().value_or(fs::path{}), prompt::fs::find_git_root());
                if (exe.empty()) {
                    continue;
                }
                auto const result = process::run_command(
                  std::vector<std::string>{exe.string(), "auth", "export"},
                  {},
                  {},
                  std::chrono::milliseconds(5000));
                if (result.exit_code != 0 || result.stdout_data.empty()) {
                    continue;
                }
                json payload = json::parse(result.stdout_data, nullptr, false);
                if (payload.is_discarded()) {
                    continue;
                }
                std::map<std::string, std::string> keys;
                if (payload.is_object()) {
                    for (auto const& [integration, value] : payload.items()) {
                        std::string key;
                        if (value.is_object()) {
                            key = value.value("key", "");
                        } else if (value.is_string()) {
                            key = value.get<std::string>();
                        }
                        if (!integration.empty() && !key.empty()) {
                            keys[integration] = key;
                        }
                    }
                } else if (payload.is_array()) {
                    for (auto const& entry : payload) {
                        if (!entry.is_object()) {
                            continue;
                        }
                        std::string integration = entry.value("integrationID", entry.value("integration", ""));
                        std::string key;
                        if (entry.contains("value")) {
                            auto const& value = entry["value"];
                            if (value.is_object()) {
                                key = value.value("key", "");
                            } else if (value.is_string()) {
                                key = value.get<std::string>();
                            }
                        }
                        if (!integration.empty() && !key.empty() && entry.value("active", true)) {
                            keys.emplace(integration, key);
                        }
                    }
                }
                if (!keys.empty()) {
                    return {std::move(keys), "opencode:" + std::string(tool) + " auth export"};
                }
            }
            return {};
        }

        std::pair<std::map<std::string, std::string>, std::string> opencode_auth_json() {
            fs::path const path = opencode_data_dir() / "auth.json";
            std::ifstream  stream(path);
            if (!stream) {
                return {};
            }
            std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            json        payload = json::parse(text, nullptr, false);
            if (payload.is_discarded() || !payload.is_object()) {
                return {};
            }
            std::map<std::string, std::string> keys;
            for (auto const& [integration, value] : payload.items()) {
                std::string key;
                if (value.is_object()) {
                    key = value.value("key", "");
                } else if (value.is_string()) {
                    key = value.get<std::string>();
                }
                if (!key.empty()) {
                    keys[integration] = key;
                }
            }
            return {std::move(keys), "opencode:" + path.string()};
        }

        std::pair<std::map<std::string, std::string>, std::string> opencode_keys() {
            if (truthy(env_value("AI_KEYS_NO_OPENCODE"))) {
                return {};
            }
            auto exported = opencode_export();
            if (!exported.first.empty()) {
                return exported;
            }
            return opencode_auth_json();
        }

        std::pair<std::map<std::string, std::string>, std::string> opencode_config() {
            fs::path        path = opencode_config_dir() / "opencode.jsonc";
            std::error_code ec;
            if (!fs::is_regular_file(path, ec)) {
                path = opencode_config_dir() / "opencode.json";
            }
            std::ifstream stream(path);
            if (!stream) {
                return {};
            }
            std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            json        payload = json::parse(strip_jsonc(text), nullptr, false);
            if (payload.is_discarded() || !payload.is_object()) {
                return {};
            }
            std::map<std::string, std::string> keys;
            if (payload.contains("provider") && payload["provider"].is_object()) {
                for (auto const& [provider_id, entry] : payload["provider"].items()) {
                    if (!entry.is_object() || !entry.contains("options") || !entry["options"].is_object()) {
                        continue;
                    }
                    auto const& options = entry["options"];
                    if (!options.contains("apiKey") || !options["apiKey"].is_string()) {
                        continue;
                    }
                    std::string key = expand_env(options["apiKey"].get<std::string>());
                    if (!key.empty() && key != "public" && key != "not-needed" && key != "none") {
                        keys[provider_id] = key;
                    }
                }
            }
            return {std::move(keys), "opencode:" + path.string()};
        }

        std::string canonical_integration(std::string_view provider) {
            if (provider == "dseek" || provider == "deepseek") {
                return "deepseek";
            }
            if (provider == "gapgpt" || provider == "gap") {
                return "gap";
            }
            if (provider == "pplx") {
                return "perplexity";
            }
            if (provider == "lmstudio" || provider == "lms") {
                return "lmstudio";
            }
            return std::string(provider);
        }

        std::vector<std::string> env_names_for(std::string_view provider, std::span<std::string const> extra) {
            std::vector<std::string> names(extra.begin(), extra.end());
            auto                     add = [&names](std::string_view name) {
                if (std::ranges::find(names, name) == names.end()) {
                    names.emplace_back(name);
                }
            };
            if (provider == "openrouter") {
                add("OPENROUTER_API_KEY");
                add("OPENROUTER_KEY");
            } else if (provider == "openai") {
                add("OPENAI_API_KEY");
            } else if (provider == "lms" || provider == "lmstudio") {
                add("LMS_API_KEY");
                add("LMSTUDIO_API_KEY");
            } else if (provider == "deepseek" || provider == "dseek") {
                add("DEEPSEEK_API_KEY");
            }
            return names;
        }

    } // namespace

    std::string mask(std::string_view value) {
        if (value.empty()) {
            return {};
        }
        if (value.size() <= 8) {
            std::string masked(value.size(), '*');
            return masked;
        }
        return std::string(value.substr(0, 6)) + "…" + std::string(value.substr(value.size() - 4));
    }

    resolved resolve(std::string_view             provider,
                     std::span<std::string const> env_names,
                     std::optional<std::string>   explicit_key) {
        if (explicit_key && !explicit_key->empty()) {
            std::string key = *explicit_key;
            while (!key.empty() && (key.back() == '\n' || key.back() == '\r' || key.back() == ' ')) {
                key.pop_back();
            }
            std::size_t start = key.find_first_not_of(" \t");
            if (start != std::string::npos) {
                key = key.substr(start);
            }
            return {.key = std::move(key), .source = "flag:--api-key"};
        }
        auto const names = env_names_for(provider, env_names);
        for (auto const& name : names) {
            std::string value = env_value(name);
            if (!value.empty()) {
                std::size_t start = value.find_first_not_of(" \t");
                if (start != std::string::npos) {
                    value = value.substr(start);
                }
                while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                    value.pop_back();
                }
                if (!value.empty()) {
                    return {.key = std::move(value), .source = "env:" + name};
                }
            }
        }
        if (auto dotenv = from_dotenv(names)) {
            return {.key = dotenv->first, .source = dotenv->second};
        }

        std::string const integration    = canonical_integration(provider);
        auto [opencode, opencode_source] = opencode_keys();
        if (auto const it = opencode.find(integration); it != opencode.end()) {
            return {.key = it->second, .source = opencode_source};
        }

        auto [config, config_source] = opencode_config();
        for (auto const& candidate : {integration, std::string(provider)}) {
            if (auto const it = config.find(candidate); it != config.end()) {
                return {.key = it->second, .source = config_source};
            }
        }
        return {};
    }

    std::pair<std::string, std::string> describe(std::string_view provider, std::span<std::string const> env_names) {
        auto const value = resolve(provider, env_names);
        return {value.source.empty() ? "(none)" : value.source, mask(value.key)};
    }

} // namespace prompt::ai::keys
