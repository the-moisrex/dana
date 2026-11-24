#include "find_skills.hpp"

#include "../ai/http.hpp"
#include "../core/process.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dana::tools {

    namespace {

        using json = nlohmann::json;

        [[nodiscard]] long long now_ms() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now().time_since_epoch())
              .count();
        }

        [[nodiscard]] std::string url_encode(std::string_view text) {
            static constexpr char hex[] = "0123456789ABCDEF";
            std::string           out;
            out.reserve(text.size() * 3);
            for (unsigned char const c : text) {
                if (std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~') {
                    out.push_back(static_cast<char>(c));
                } else {
                    out.push_back('%');
                    out.push_back(hex[c >> 4U]);
                    out.push_back(hex[c & 0x0FU]);
                }
            }
            return out;
        }

        [[nodiscard]] std::string lower(std::string_view text) {
            std::string out(text);
            for (char& c : out) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return out;
        }

        [[nodiscard]] std::vector<std::string> query_tokens(std::string_view query) {
            std::vector<std::string> tokens;
            std::string              current;
            for (char const c : lower(query)) {
                if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
                    current.push_back(c);
                } else if (!current.empty()) {
                    tokens.push_back(std::move(current));
                    current.clear();
                }
            }
            if (!current.empty()) {
                tokens.push_back(std::move(current));
            }
            return tokens;
        }

        [[nodiscard]] std::string normalize_name(std::string_view name) {
            std::string out;
            for (char const c : lower(name)) {
                if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
                    out.push_back(c);
                }
            }
            return out;
        }

        [[nodiscard]] std::vector<std::string> installed_skills() {
            std::vector<std::string> names;
            char const* home = std::getenv("HOME"); // NOLINT(concurrency-mt-unsafe): read once at startup; no
                                                    // concurrent env mutation.
            if (home == nullptr) {
                return names;
            }
            for (
              std::string const& dir : {std::string(home) + "/.agents/skills", std::string(home) + "/.claude/skills"})
            {
                std::error_code ec;
                for (auto const& entry : std::filesystem::directory_iterator(dir, ec)) {
                    if (ec) {
                        break;
                    }
                    names.push_back(entry.path().filename().string());
                }
            }
            std::ranges::sort(names);
            names.erase(std::unique(names.begin(), names.end()), names.end());
            return names;
        }

        [[nodiscard]] int
        match_percent(std::string_view name, std::string_view summary, std::vector<std::string> const& tokens) {
            if (tokens.empty()) {
                return 0;
            }
            std::string const hay  = lower(std::string(name) + " " + std::string(summary));
            std::size_t       hits = 0;
            for (auto const& token : tokens) {
                if (hay.find(token) != std::string::npos) {
                    ++hits;
                }
            }
            return static_cast<int>(
              std::lround(100.0 * static_cast<double>(hits) / static_cast<double>(tokens.size())));
        }

        [[nodiscard]] std::string json_string(json const& value, std::string_view key) {
            auto const it = value.find(key);
            return it != value.end() && it->is_string() ? it->get<std::string>() : std::string{};
        }

        [[nodiscard]] long long json_number(json const& value, std::string_view key) {
            auto const it = value.find(key);
            return it != value.end() && it->is_number() ? it->get<long long>() : 0;
        }

        [[nodiscard]] json json_or_null(json const& value, std::string_view key) {
            auto const it = value.find(key);
            return it != value.end() ? *it : json(nullptr);
        }

        void enrich(json&                                  items,
                    std::vector<std::string> const&        tokens,
                    std::unordered_set<std::string> const& installed) {
            for (auto& item : items) {
                std::string const name    = json_string(item, "name");
                std::string const summary = json_string(item, "summary");
                item["match"]             = match_percent(name, summary, tokens);
                item["installed"]         = installed.contains(normalize_name(name));
            }
        }

        void mark_other_board(json& items, std::unordered_set<std::string> const& other) {
            for (auto& item : items) {
                item["also_other"] = other.contains(normalize_name(json_string(item, "name")));
            }
        }

        [[nodiscard]] std::unordered_set<std::string> normalized_names(json const& items) {
            std::unordered_set<std::string> names;
            for (auto const& item : items) {
                names.insert(normalize_name(json_string(item, "name")));
            }
            return names;
        }

        struct health_entry {
            std::string source;
            std::string status;
            long long   ms    = 0;
            std::size_t count = 0;
        };

        [[nodiscard]] json health_json(std::vector<health_entry> entries) {
            std::ranges::sort(entries, {}, &health_entry::source);
            json out = json::array();
            for (auto const& entry : entries) {
                out.push_back({
                  {"source", entry.source},
                  {"status", entry.status},
                  {    "ms",     entry.ms},
                  { "count",  entry.count}
                });
            }
            return out;
        }

        [[nodiscard]] json fetch_skills_sh(std::string_view query, int limit, health_entry& health) {
            long long const started = now_ms();
            json            out     = json::array();
            try {
                ai::http_request request;
                request.url = std::format("https://skills.sh/api/search?q={}&limit={}", url_encode(query), limit);
                request.timeout_seconds = 25;
                auto const payload      = ai::request_json(request);
                auto const skills       = payload.find("skills");
                if (skills != payload.end() && skills->is_array()) {
                    for (auto const& skill : *skills) {
                        std::string const name   = json_string(skill, "skillId");
                        std::string const source = json_string(skill, "source");
                        out.push_back({
                          {"board", "skills.sh"},
                          {"name", name},
                          {"summary", nullptr},
                          {"installs", json_number(skill, "installs")},
                          {"stars", nullptr},
                          {"version", nullptr},
                          {"updatedAt", nullptr},
                          {"url", std::format("https://www.skills.sh/{}/{}", source, name)},
                          {"repo", source},
                          {"slug", nullptr}
                        });
                    }
                }
                health = {.source = "skills.sh", .status = "ok", .ms = now_ms() - started, .count = out.size()};
            } catch (...) {
                health = {.source = "skills.sh", .status = "failed", .ms = now_ms() - started, .count = 0};
            }
            return out;
        }

        [[nodiscard]] json fetch_clawhub(std::string_view query, int limit, health_entry& health) {
            long long const started = now_ms();
            json            out     = json::array();
            try {
                ai::http_request request;
                request.url             = std::format("https://clawhub.ai/api/search?q={}", url_encode(query));
                request.timeout_seconds = 15;
                auto const search       = ai::request_json(request);
                auto const results      = search.find("results");
                if (results != search.end() && results->is_array()) {
                    int count = 0;
                    for (auto const& result : *results) {
                        if (count++ >= limit) {
                            break;
                        }
                        std::string const slug = json_string(result, "slug");
                        if (slug.empty()) {
                            continue;
                        }
                        try {
                            ai::http_request detail_request;
                            detail_request.url = std::format("https://clawhub.ai/api/skill?slug={}", url_encode(slug));
                            detail_request.timeout_seconds = 10;
                            auto const detail              = ai::request_json(detail_request);
                            auto const skill_it            = detail.find("skill");
                            json const skill =
                              skill_it != detail.end() && skill_it->is_object() ? *skill_it : json::object();
                            std::string const name = json_string(skill, "slug");
                            if (name.empty()) {
                                continue;
                            }
                            auto const stats    = skill.find("stats");
                            long long  installs = 0;
                            long long  stars    = 0;
                            if (stats != skill.end() && stats->is_object()) {
                                installs = json_number(*stats, "installsAllTime");
                                stars    = json_number(*stats, "stars");
                            }
                            std::string version;
                            if (auto const latest = skill.find("latestVersion");
                                latest != skill.end() && latest->is_object())
                            {
                                version = json_string(*latest, "version");
                            }
                            out.push_back({
                              {"board", "clawhub"},
                              {"name", name},
                              {"summary", json_or_null(skill, "summary")},
                              {"installs", installs},
                              {"stars", stars},
                              {"version", version.empty() ? json(nullptr) : json(version)},
                              {"updatedAt", json_or_null(skill, "updatedAt")},
                              {"url", std::format("https://clawhub.ai/skill/{}", name)},
                              {"repo", nullptr},
                              {"slug", name}
                            });
                        } catch (...) {
                            // A single failed detail request is skipped.
                            continue;
                        }
                    }
                }
                health = {.source = "clawhub", .status = "ok", .ms = now_ms() - started, .count = out.size()};
            } catch (...) {
                health = {.source = "clawhub", .status = "failed", .ms = now_ms() - started, .count = 0};
            }
            return out;
        }

        [[nodiscard]] json fetch_github(std::string_view query, int limit, health_entry& health) {
            long long const started = now_ms();
            json            out     = json::array();
            if (!process::tool_exists("gh")) {
                health = {.source = "github", .status = "skipped", .ms = 0, .count = 0};
                return out;
            }

            std::string const                     keyword = std::string(query.substr(0, query.find(' ')));
            std::unordered_map<std::string, json> unique;
            for (std::string_view const topic : {"claude-skills", "agent-skills", "claude-code-skills"}) {
                std::vector<std::string> const argv{
                  "gh",
                  "search",
                  "repos",
                  keyword,
                  "--topic",
                  std::string(topic),
                  "--limit",
                  std::to_string(limit),
                  "--json",
                  "fullName,stargazersCount,description,url"};
                auto const result = process::command{argv}.capture_stderr().run();
                if (result.exit_code != 0) {
                    continue;
                }
                auto const parsed = json::parse(result.stdout_data, nullptr, false);
                if (parsed.is_discarded() || !parsed.is_array()) {
                    continue;
                }
                for (auto const& repo : parsed) {
                    std::string const name = json_string(repo, "fullName");
                    if (name.empty() || unique.contains(name)) {
                        continue;
                    }
                    unique.emplace(
                      name,
                      json{
                        {"board", "github"},
                        {"name", name},
                        {"summary", json_or_null(repo, "description")},
                        {"stars", json_number(repo, "stargazersCount")},
                        {"url", json_string(repo, "url")},
                        {"repo", name}
                    });
                }
            }
            for (auto& [name, item] : unique) {
                (void) name;
                out.push_back(std::move(item));
            }
            health = {.source = "github", .status = "ok", .ms = now_ms() - started, .count = out.size()};
            return out;
        }

    } // namespace

    json find_skills(std::string_view query, find_skills_options const& options) {
        auto const                      tokens    = query_tokens(query);
        auto const                      installed = installed_skills();
        std::unordered_set<std::string> installed_normalized;
        for (auto const& name : installed) {
            installed_normalized.insert(normalize_name(name));
        }

        health_entry skills_health;
        health_entry claw_health;
        health_entry github_health;

        json skills_sh = fetch_skills_sh(query, options.limit, skills_health);
        json clawhub   = fetch_clawhub(query, options.limit, claw_health);
        json github    = fetch_github(query, options.limit, github_health);

        enrich(skills_sh, tokens, installed_normalized);
        enrich(clawhub, tokens, installed_normalized);
        enrich(github, tokens, installed_normalized);

        auto const claw_names  = normalized_names(clawhub);
        auto const skill_names = normalized_names(skills_sh);
        mark_other_board(skills_sh, claw_names);
        mark_other_board(clawhub, skill_names);

        std::stable_sort(skills_sh.begin(), skills_sh.end(), [](json const& a, json const& b) {
            auto const installs_a = json_number(a, "installs");
            auto const installs_b = json_number(b, "installs");
            if (installs_a != installs_b) {
                return installs_a > installs_b;
            }
            return json_string(a, "name") < json_string(b, "name");
        });
        std::stable_sort(clawhub.begin(), clawhub.end(), [](json const& a, json const& b) {
            auto const installs_a = json_number(a, "installs");
            auto const installs_b = json_number(b, "installs");
            if (installs_a != installs_b) {
                return installs_a > installs_b;
            }
            auto const stars_a = json_number(a, "stars");
            auto const stars_b = json_number(b, "stars");
            if (stars_a != stars_b) {
                return stars_a > stars_b;
            }
            return json_string(a, "name") < json_string(b, "name");
        });
        std::stable_sort(github.begin(), github.end(), [](json const& a, json const& b) {
            auto const match_a = json_number(a, "match");
            auto const match_b = json_number(b, "match");
            if (match_a != match_b) {
                return match_a > match_b;
            }
            auto const stars_a = json_number(a, "stars");
            auto const stars_b = json_number(b, "stars");
            if (stars_a != stars_b) {
                return stars_a > stars_b;
            }
            return json_string(a, "name") < json_string(b, "name");
        });

        return json{
          {          "query",                                       std::string(query)},
          {         "health", health_json({skills_health, claw_health, github_health})},
          {"installed_count",                                         installed.size()},
          {      "skills_sh",                                     std::move(skills_sh)},
          {        "clawhub",                                       std::move(clawhub)},
          {         "github",                                        std::move(github)}
        };
    }

} // namespace dana::tools
