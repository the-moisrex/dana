#include "../ai/http.hpp"
#include "../core/fs.hpp"
#include "../core/process.hpp"
#include "../core/strings.hpp"
#include "../runtime/args.hpp"
#include "../runtime/embed.hpp"
#include "../runtime/prompt_registry.hpp"
#include "../tools/find_skills.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <regex>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace dana {

    namespace {

        std::string_view env_or(std::string_view const key, std::string_view const fallback) {
            if (char const* v = std::getenv(key.data())) {
                return v;
            }
            return fallback;
        }

        std::string_view home_dir() {
            return env_or("HOME", "");
        }

        std::string skills_dir() {
            std::string xdg = std::string(env_or("XDG_DATA_HOME", ""));
            if (xdg.empty()) {
                xdg = std::string(home_dir()) + "/.local/share";
            }
            return xdg + "/agents/skills";
        }

        std::vector<std::string> legacy_dirs() {
            return {std::string(home_dir()) + "/.agents/skills", std::string(home_dir()) + "/.claude/skills"};
        }

        std::vector<std::string> cache_dirs() {
            auto dirs = legacy_dirs();
            dirs.insert(dirs.begin(), skills_dir());
            return dirs;
        }

        // Run a command, capturing stdout. stderr is inherited like a plain `$(...)`
        // capture, or discarded (`2>/dev/null`) when the bash call site redirects it.
        // Returns stdout and the exit code.
        std::string run_capture(
          text::string_views     cmd,
          std::string_view const stdin_data,
          int&                   exit_code,
          bool                   discard_stderr = false) {
            process::command c(cmd);
            c.stdin_data(std::string(stdin_data));
            if (discard_stderr) {
                c.discard_stderr();
            }
            auto const res = c.run();
            exit_code      = res.exit_code;
            return res.stdout_data;
        }

        template <std::size_t N>
        std::string run_capture(
          std::array<std::string, N> cmd,
          std::string_view const     stdin_data,
          int&                       exit_code,
          bool                       discard_stderr = false) {
            return run_capture(text::string_views(cmd), stdin_data, exit_code, discard_stderr);
        }

        template <std::size_t N>
        std::string run_capture(
          std::array<std::string_view, N> cmd,
          std::string_view const          stdin_data,
          int&                            exit_code,
          bool                            discard_stderr = false) {
            return run_capture(text::string_views(cmd), stdin_data, exit_code, discard_stderr);
        }

        // Port of parse_frontmatter: collect lines between the first two "---" lines,
        // then parse them with the same Python one-liner the script uses.
        nlohmann::ordered_json parse_frontmatter(std::filesystem::path const& file) {
            std::ifstream in(file, std::ios::binary);
            if (!in) {
                return nlohmann::ordered_json::object();
            }
            std::string fm;
            bool        in_fm = false;
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (line == "---") {
                    if (in_fm) {
                        break;
                    }
                    in_fm = true;
                    continue;
                }
                if (in_fm) {
                    fm += line + "\n";
                }
            }
            // bash pipes `echo "$fm_lines"`, i.e. fm + an extra trailing newline.
            std::string stdin_data = fm + "\n";

            // Port of the python one-liner the script pipes the frontmatter into:
            // `^(\w[\w_-]*):\s*(.*)` per line, value stripped and unquoted once when it
            // is wrapped in matching quotes. The result is a flat JSON object —
            // downstream consumers read it with jq, so the exact serialization does
            // not matter, only the keys and values.
            nlohmann::ordered_json data = nlohmann::ordered_json::object();
            std::istringstream     lines_in(stdin_data);
            for (std::string raw; std::getline(lines_in, raw);) {
                if (!raw.empty() && raw.back() == '\r') {
                    raw.pop_back();
                }
                raw        = dana::text::rstrip(raw);
                auto colon = raw.find(':');
                if (colon == std::string::npos || colon == 0) {
                    continue;
                }
                std::string key = raw.substr(0, colon);
                bool        ok  = (std::isalnum(static_cast<unsigned char>(key[0])) != 0) || key[0] == '_';
                for (char c : key) {
                    if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '_' && c != '-') {
                        ok = false;
                    }
                }
                if (!ok) {
                    continue;
                }
                std::string value = dana::text::strip(raw.substr(colon + 1));
                if (
                  value.size() >= 2 && value.front() == value.back() && (value.front() == '"' || value.front() == '\''))
                {
                    value = value.substr(1, value.size() - 2);
                }
                data[key] = value;
            }
            return data;
        }

        std::string find_cached_skill(std::string_view name) {
            for (auto const& dir : cache_dirs()) {
                std::error_code ec;
                auto            f = std::filesystem::path(dir) / name / "SKILL.md";
                if (std::filesystem::is_regular_file(f, ec)) {
                    return f.string();
                }
            }
            return {};
        }

        void list_cached_skills(std::string& out) {
            bool found = false;
            for (auto const& dir : cache_dirs()) {
                std::error_code ec;
                if (!std::filesystem::is_directory(dir, ec)) {
                    continue;
                }
                // bash iterates `"$dir"/*/` (glob => sorted by name).
                std::vector<std::filesystem::path> subdirs;
                std::error_code                    it_ec;
                for (std::filesystem::directory_iterator it(dir, it_ec), end; !it_ec && it != end; it.increment(it_ec))
                {
                    if (it_ec) {
                        break;
                    }
                    if (std::filesystem::is_directory(it->path())) {
                        subdirs.push_back(it->path());
                    }
                }
                std::sort(subdirs.begin(), subdirs.end());
                for (auto const& skill_path : subdirs) {
                    std::error_code fec;
                    auto            skill_file = skill_path / "SKILL.md";
                    if (!std::filesystem::is_regular_file(skill_file, fec)) {
                        continue;
                    }
                    auto const  meta = parse_frontmatter(skill_file);
                    std::string name = meta.value("name", std::string{});
                    if (name.empty()) {
                        name = skill_path.filename().string();
                    }
                    std::string desc = meta.value("description", std::string{});
                    if (desc.size() > 80) {
                        desc = desc.substr(0, 77) + "...";
                    }
                    std::string field = name;
                    if (field.size() < 24) {
                        field += std::string(24 - field.size(), ' ');
                    }
                    out   += std::format("  {} {}\n", field, desc);
                    found  = true;
                }
            }
            if (!found) {
                out += "  (no cached skills found)\n\n";
                out += std::format("  Skills are cached in: {}\n", skills_dir());
                out += "  Use 'prompt skill <query>' to search and download skills.\n";
            }
        }

        std::vector<std::string> cached_skill_names() {
            std::vector<std::string> names;
            for (auto const& dir : cache_dirs()) {
                std::error_code ec;
                if (!std::filesystem::is_directory(dir, ec)) {
                    continue;
                }
                for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
                    if (!std::filesystem::is_directory(it->path())) {
                        continue;
                    }
                    std::error_code fec;
                    if (!std::filesystem::is_regular_file(it->path() / "SKILL.md", fec)) {
                        continue;
                    }
                    names.push_back(it->path().filename().string());
                }
            }
            std::sort(names.begin(), names.end());
            names.erase(std::unique(names.begin(), names.end()), names.end());
            return names;
        }

        // Flatten the three registries into one indexed result list. The field
        // shape mirrors what the download path consumes: skills.sh carries a repo
        // and slug, clawhub has no repo, and GitHub has no slug or install count.
        nlohmann::json search_skills(std::string_view query) {
            auto const     data   = tools::find_skills(query, {.scan = 0});
            nlohmann::json merged = nlohmann::json::array();

            auto const add = [&](nlohmann::json const& list, bool has_repo, bool has_slug, bool has_installs) {
                if (!list.is_array()) {
                    return;
                }
                for (auto const& item : list) {
                    nlohmann::json rec;
                    rec["idx"]   = merged.size() + 1;
                    rec["name"]  = item.value("name", std::string{});
                    rec["board"] = item.value("board", std::string{});
                    rec["repo"] =
                      has_repo ? nlohmann::json(item.value("repo", std::string{})) : nlohmann::json(nullptr);
                    rec["slug"] =
                      has_slug ? nlohmann::json(item.value("slug", std::string{})) : nlohmann::json(nullptr);
                    rec["url"] = item.value("url", std::string{});
                    rec["installs"] =
                      has_installs ? nlohmann::json(item.value("installs", 0LL)) : nlohmann::json(nullptr);
                    rec["stars"]   = item.contains("stars") ? item["stars"] : nlohmann::json(nullptr);
                    rec["match"]   = item.value("match", 0);
                    rec["summary"] = item.contains("summary") ? item["summary"] : nlohmann::json(nullptr);
                    merged.push_back(std::move(rec));
                }
            };

            add(data["skills_sh"], true, true, true);
            add(data["clawhub"], false, true, true);
            add(data["github"], true, false, false);
            return merged;
        }

        // Exact name first, then a case-insensitive regex match, like the jq
        // `select(.name | test($n; "i"))` fallback.
        nlohmann::json find_rec(nlohmann::json const& results, std::string_view name) {
            if (!results.is_array()) {
                return nullptr;
            }
            for (auto const& rec : results) {
                if (rec.value("name", std::string{}) == name) {
                    return rec;
                }
            }
            try {
                std::regex const pattern(std::string(name), std::regex::ECMAScript | std::regex::icase);
                for (auto const& rec : results) {
                    if (std::regex_search(rec.value("name", std::string{}), pattern)) {
                        return rec;
                    }
                }
            } catch (std::regex_error const&) {
                // An invalid pattern simply matches nothing.
            }
            return nullptr;
        }

        // Fetch a registry record's SKILL.md into the skills directory. Returns
        // false (leaving no partial directory behind) when the download fails.
        bool download_skill(nlohmann::json const& rec, std::string& err) {
            std::string const           skill_name = rec.value("name", std::string{});
            std::error_code             ec;
            std::filesystem::path const target = std::filesystem::path(skills_dir()) / skill_name;
            std::filesystem::create_directories(target, ec);

            ai::http_request request;
            request.url             = rec.value("url", std::string{});
            request.timeout_seconds = 15;
            std::string body;
            try {
                body = ai::request_text(request);
            } catch (...) {
                body.clear();
            }
            if (body.empty()) {
                std::filesystem::remove_all(target, ec);
                return false;
            }
            std::ofstream out(target / "SKILL.md", std::ios::binary);
            out << body;
            err += "Installed '" + skill_name + "' to " + (target / "SKILL.md").string() + "\n";
            return true;
        }

    } // namespace

    static void execute_skill(view const& v, result& r) {
        using namespace std::string_literals;
        auto opts = file_options_or_throw(v);

        std::string first = v.args.empty() ? std::string() : std::string(v.args[0]);

        // Build the positional arg list (parse_arguments drops --head + value and
        // the value-less --num/--numbered line-number flags).
        auto positionals = [&] {
            std::vector<std::string> pos;
            for (std::size_t i = 0; i < v.args.size();) {
                if (v.args[i] == "--head") {
                    ++i;
                    if (i < v.args.size()) {
                        ++i;
                    }
                } else if (is_number_flag(v.args[i])) {
                    ++i;
                } else {
                    pos.emplace_back(v.args[i++]);
                }
            }
            return pos;
        };

        if (first == "list" || first == "install" || first == "uninstall") {
            auto        pos        = positionals();
            std::string subcommand = pos.empty() ? std::string() : pos[0];

            if (subcommand == "list") {
                std::string out;
                out += "Cached skills:\n\n";
                list_cached_skills(out);
                r.out = std::move(out);
                return;
            }

            if (subcommand == "uninstall") {
                // uninstall_skill uses the raw $1, which for this subcommand is the
                // word "uninstall" itself.
                std::string name  = "uninstall";
                bool        found = false;
                std::string out;
                for (auto const& dir : cache_dirs()) {
                    std::error_code ec;
                    auto            target = std::filesystem::path(dir) / name;
                    if (std::filesystem::is_directory(target, ec)) {
                        std::uintmax_t removed = std::filesystem::remove_all(target, ec);
                        (void) removed;
                        out +=
                          std::format("Uninstalled '{}' from {}\n", name, (std::filesystem::path(dir) / name).string());
                        found = true;
                    }
                }
                if (!found) {
                    throw soft_error{"error: skill '" + name + "' not found in any cache directory\n"};
                }
                r.out = std::move(out);
                return;
            }

            if (subcommand == "install") {
                // install_skill uses the raw $1 ("install"); this hits the network.
                std::string name = "install";
                std::string err;
                err            += std::format("Searching for '{}'...\n", name);
                auto const rec  = find_rec(search_skills(name), name);
                if (rec.is_null()) {
                    throw soft_error{err + "error: skill '" + name + "' not found in any registry\n"};
                }
                err +=
                  std::format("Found: {} on {}\n", rec.value("name", std::string{}), rec.value("board", std::string{}));
                std::string const skill_name  = rec.value("name", std::string{});
                err                          += std::format("Downloading {}...\n", skill_name);
                if (!download_skill(rec, err)) {
                    throw soft_error{err + "error: failed to download SKILL.md for '" + skill_name + "'\n"};
                }
                std::string out;
                out   += "\nInstalled skill content:\n\n";
                out   += read_file(std::filesystem::path(skills_dir()) / skill_name / "SKILL.md");
                r.out  = std::move(out);
                r.err  = std::move(err);
                return;
            }
        }

        // Default mode: prompt skill <name> [FILE...]
        auto        pos        = positionals();
        std::string skill_name = pos.empty() ? std::string() : pos[0];

        if (skill_name.empty()) {
            throw usage_error{
              "error: skill name required\n"
              "Usage: prompt skill <name> [FILE...]\n"
              "       prompt skill list   # to see cached skills\n"};
        }

        std::string err;
        std::string skill_file = find_cached_skill(skill_name);
        if (skill_file.empty()) {
            err            += std::format("Skill '{}' not cached, downloading...\n", skill_name);
            auto const rec  = find_rec(search_skills(skill_name), skill_name);
            if (rec.is_null()) {
                err += std::format("error: skill '{}' not found in cache or registries\n", skill_name);
                err += std::format("Use 'prompt skill list --remote {}' to search registries.\n", skill_name);
                throw soft_error{std::move(err)};
            }
            std::string const downloaded_skill  = rec.value("name", std::string{});
            err                                += std::format("Downloading {}...\n", downloaded_skill);
            if (!download_skill(rec, err)) {
                err += std::format("error: failed to download SKILL.md for '{}'\n", downloaded_skill);
                throw soft_error{std::move(err)};
            }
            skill_file = (std::filesystem::path(skills_dir()) / downloaded_skill / "SKILL.md").string();
        }

        std::string out = read_file(std::filesystem::path(skill_file));

        if (v.in.piped()) {
            std::string content(v.in.text());
            while (!content.empty() && content.back() == '\n') {
                content.pop_back();
            }
            if (!content.empty()) {
                out += '\n';
                out += content;
                out += '\n';
            }
        }

        for (std::size_t i = 1; i < pos.size(); ++i) {
            std::string_view      file = pos[i];
            std::error_code       ec;
            std::filesystem::path fp(file);
            if (std::filesystem::is_regular_file(fp, ec)) {
                std::string file_name  = fp.filename().string();
                out                   += '\n';
                out                   += std::format("File: {}\n", file_name);
                out                   += '\n';
                out                   += std::format("```{}\n", infer_lang(std::string_view{file_name}));
                out                   += number_lines(trim_context_nl(read_file(fp), opts.head_lines), opts.numbered);
                out                   += "```\n";
            } else {
                err += std::format("Warning: File '{}' not found.\n", file);
            }
        }

        r.out = std::move(out);
        r.err = std::move(err);
    }

    static void render_help_skill(std::ostream& os) {
        os << file_usage_help("skill", "[FILE...]", "some-command", "<name>")
           << R"EOF(       prompt skill list [--remote <query>]
       prompt skill install <name>
       prompt skill uninstall <name>

Loads a cached skill and outputs its content as an AI prompt.
If the skill is not cached, it is downloaded automatically.
Piped stdin and file arguments are embedded as context.
It also manages skills: list, install, and uninstall (see below).

Subcommands:
  list              List locally cached skills
  list --remote Q   Search remote registries for skills matching Q
  install <name>    Download a specific skill by exact name
  uninstall <name>  Remove a cached skill

Options:
)EOF" << file_options_help("the embedded context", true)
           << R"EOF(
Examples:
  prompt skill docker                      # load cached docker skill
  cat file.cpp | prompt skill cpp-testing  # load skill + embed file
  prompt skill list                        # show cached skills
  prompt skill install docker              # download docker skill
)EOF";
    }

    // Subcommands and cached skill names for `prompt skill <TAB>` /
    // `prompt skill install|uninstall <TAB>`.
    static void complete_skill(completion_request const& req, std::ostream& os) {
        static constexpr std::array<dana::completion::item, 3> kSubcommands = {
          {
           {.value = "list", .description = "List locally cached skills"},
           {.value = "install", .description = "Download a specific skill by exact name"},
           {.value = "uninstall", .description = "Remove a cached skill"},
           },
        };
        std::string_view first = req.prior_args.empty() ? std::string_view{} : req.prior_args.front();
        if (first == "install" || first == "uninstall") {
            for (auto const& name : cached_skill_names()) {
                if (dana::completion::matches(req.prefix, name)) {
                    dana::completion::write_item(os, name, "cached skill");
                }
            }
            return;
        }
        dana::completion::write_items(os, req.prefix, kSubcommands);
        for (auto const& name : cached_skill_names()) {
            if (dana::completion::matches(req.prefix, name)) {
                dana::completion::write_item(os, name, "cached skill");
            }
        }
    }

    // This file is the prompt: it implements it and registers it at static-init time
    // (no central list — see sdk/prompt_registry.hpp).
    [[maybe_unused]] bool const registered = dana::register_prompt(
      "skill",
      "Loads a cached skill and outputs its content as an AI prompt. If the skill is not cached, it is "
      "downloaded automatically. Piped stdin and file arguments are embedded as context. It also "
      "manages skills: list, install, and uninstall (see below).",
      &execute_skill,
      &render_help_skill,
      &complete_skill);

} // namespace dana
