#include "run_tool.hpp"

#include "../core/fs.hpp"
#include "../core/process.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace dana::tools {

    namespace {

        [[nodiscard]] std::filesystem::path project_root() {
            if (auto const root = fs::find_git_root()) {
                return *root;
            }
            return std::filesystem::current_path();
        }

        [[nodiscard]] bool has_file(std::filesystem::path const& dir, std::string_view name) {
            std::error_code ec;
            return std::filesystem::is_regular_file(dir / std::string(name), ec);
        }

        [[nodiscard]] std::vector<std::filesystem::path> discover_build_dirs(std::filesystem::path const& root) {
            bool const                         cmake_project = has_file(root, "CMakeLists.txt");
            std::vector<std::filesystem::path> dirs;
            std::unordered_set<std::string>    seen;

            auto const consider = [&](std::filesystem::path const& dir) {
                bool const found = has_file(dir, "build.ninja") || has_file(dir, "CMakeCache.txt") ||
                                   (!cmake_project && has_file(dir, "Makefile"));
                if (found) {
                    auto const canonical = std::filesystem::weakly_canonical(dir);
                    if (seen.insert(canonical.string()).second) {
                        dirs.push_back(canonical);
                    }
                }
            };

            consider(root);
            std::error_code ec;
            for (auto const& entry : std::filesystem::directory_iterator(root, ec)) {
                if (ec) {
                    break;
                }
                if (entry.is_directory(ec)) {
                    consider(entry.path());
                }
            }
            std::ranges::sort(dirs, {}, [](std::filesystem::path const& p) {
                return p.string();
            });
            return dirs;
        }

        [[nodiscard]] std::optional<std::filesystem::path> resolve_build_dir(
          std::filesystem::path const&                root,
          std::optional<std::filesystem::path> const& requested) {
            if (requested) {
                std::error_code ec;
                auto const      absolute = std::filesystem::absolute(*requested, ec);
                return ec ? std::nullopt : std::optional(absolute);
            }
            auto const dirs = discover_build_dirs(root);
            if (dirs.empty()) {
                return std::nullopt;
            }
            return dirs.front();
        }

        [[nodiscard]] std::vector<std::string> split_lines(std::string_view text) {
            std::vector<std::string> lines;
            std::size_t              start = 0;
            while (start < text.size()) {
                auto const end  = text.find('\n', start);
                auto const stop = end == std::string_view::npos ? text.size() : end;
                if (stop > start) {
                    lines.emplace_back(text.substr(start, stop - start));
                }
                start = end == std::string_view::npos ? text.size() : end + 1;
            }
            return lines;
        }

        [[nodiscard]] bool is_space(char const c) noexcept {
            return std::isspace(static_cast<unsigned char>(c)) != 0;
        }

        [[nodiscard]] std::string trim(std::string_view text) {
            while (!text.empty() && is_space(text.front())) {
                text.remove_prefix(1);
            }
            while (!text.empty() && is_space(text.back())) {
                text.remove_suffix(1);
            }
            return std::string(text);
        }

        [[nodiscard]] bool excluded_target(std::string_view name) noexcept {
            return name.empty() || name == "install" || name == "clean" || name == "help" || name == "all" ||
                   name == "depend" || name == "preinstall" || name == "codegen" || name == "list_install_components";
        }

        void filter_targets(std::vector<std::string>& targets) {
            std::unordered_set<std::string> seen;
            std::vector<std::string>        out;
            for (auto& target : targets) {
                if (excluded_target(target) || target.find_first_of(" \t") != std::string::npos) {
                    continue;
                }
                if (seen.insert(target).second) {
                    out.push_back(std::move(target));
                }
            }
            std::ranges::sort(out);
            targets = std::move(out);
        }

        [[nodiscard]] std::vector<std::string> ninja_targets(std::filesystem::path const& build_dir) {
            std::vector<std::string> targets;
            std::ifstream            in(build_dir / "build.ninja", std::ios::binary);
            std::string              line;
            while (std::getline(in, line)) {
                if (!line.starts_with("build ")) {
                    continue;
                }
                auto const colon = line.find(':');
                if (colon == std::string::npos) {
                    continue;
                }
                auto const name = trim(std::string_view(line).substr(6, colon - 6));
                if (!name.empty()) {
                    targets.push_back(name);
                }
            }
            return targets;
        }

        [[nodiscard]] std::vector<std::string> makefile_targets(std::filesystem::path const& build_dir) {
            std::vector<std::string> targets;
            std::ifstream            in(build_dir / "Makefile", std::ios::binary);
            std::string              line;
            while (std::getline(in, line)) {
                if (line.empty() || line.front() == '\t') {
                    continue;
                }
                auto const colon = line.find(':');
                if (colon == std::string::npos) {
                    continue;
                }
                auto const name = trim(std::string_view(line).substr(0, colon));
                if (!name.empty() && name.find_first_of(" \t") == std::string::npos) {
                    targets.push_back(name);
                }
            }
            return targets;
        }

        [[nodiscard]] std::vector<std::string> cmake_targets(std::filesystem::path const& build_dir) {
            std::vector<std::string> const argv{"cmake", "--build", build_dir.string(), "--target", "help"};
            auto const                     result = process::command{argv}.discard_stderr().run();
            std::vector<std::string>       targets;
            for (auto const& line : split_lines(result.stdout_data)) {
                if (line.find('/') != std::string::npos || line.find("CMAKE_SYMLINK_LIBRARY") != std::string::npos ||
                    line.find(".json") != std::string::npos || line.starts_with("clean") ||
                    line.find('[') != std::string::npos || line.find("build.ninja") != std::string::npos ||
                    line.starts_with("help") || line.starts_with("install") || line.starts_with("rebuild_cache") ||
                    line.starts_with("edit_cache") || line.starts_with("list_install_components") ||
                    line.starts_with("codegen") || line.starts_with("The following are"))
                {
                    continue;
                }
                auto const colon = line.find(':');
                if (colon == std::string::npos) {
                    continue;
                }
                auto name = trim(std::string_view(line).substr(0, colon));
                if (name.starts_with("... ")) {
                    name.erase(0, 4);
                }
                if (!name.empty()) {
                    targets.push_back(std::move(name));
                }
            }
            return targets;
        }

        [[nodiscard]] std::optional<std::filesystem::path> find_executable(
          std::filesystem::path const& build_dir,
          std::string const&           name) {
            auto            direct = build_dir / name;
            std::error_code ec;
            if (std::filesystem::is_regular_file(direct, ec)) {
                return direct;
            }
            for (auto const& entry : std::filesystem::recursive_directory_iterator(build_dir, ec)) {
                if (ec) {
                    break;
                }
                if (entry.is_regular_file(ec) && entry.path().filename() == name) {
                    auto const status = entry.status(ec);
                    if (!ec &&
                        (status.permissions() & std::filesystem::perms::owner_exec) != std::filesystem::perms::none)
                    {
                        return entry.path();
                    }
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] std::string join_command(std::vector<std::string> const& argv) {
            std::string out;
            for (std::size_t i = 0; i < argv.size(); ++i) {
                if (i != 0) {
                    out.push_back(' ');
                }
                out += argv[i];
            }
            return out;
        }

        [[nodiscard]] process::command_result run_merged(std::vector<std::string> const& argv) {
            return process::command{argv}.stderr_to_stdout().run();
        }

    } // namespace

    std::vector<std::string> list_targets(std::optional<std::filesystem::path> const& build_dir) {
        auto const root = project_root();
        auto const dir  = resolve_build_dir(root, build_dir);
        if (!dir) {
            return {};
        }

        std::vector<std::string> targets;
        if (has_file(*dir, "build.ninja")) {
            targets = ninja_targets(*dir);
        }
        if (targets.empty() && has_file(*dir, "Makefile")) {
            targets = makefile_targets(*dir);
        }
        if (targets.empty()) {
            targets = cmake_targets(*dir);
        }
        filter_targets(targets);
        return targets;
    }

    run_result run_target(std::vector<std::string> const& args, run_options const& options) {
        if (args.empty()) {
            return {.output = "No target specified.\n", .exit_code = 1};
        }

        auto const root = project_root();
        auto const dir  = resolve_build_dir(root, options.build_dir);
        if (!dir) {
            return {.output    = "Wrong build directory: no CMakeCache.txt, build.ninja or Makefile found.\n",
                    .exit_code = 1};
        }

        std::string const& target = args.front();

        std::vector<std::string> build_argv;
        if (has_file(*dir, "CMakeCache.txt")) {
            build_argv = {"cmake", "--build", dir->string(), "--target", target};
        } else if (has_file(*dir, "build.ninja")) {
            build_argv = {"ninja", "-C", dir->string(), "-v", target};
        } else if (has_file(std::filesystem::current_path(), "Makefile") || has_file(*dir, "Makefile")) {
            build_argv = {"make", "-C", dir->string(), target};
        } else {
            return {.output = std::format("Wrong build directory: '{}'\n", dir->string()), .exit_code = 1};
        }

        std::string output;
        output += std::format("Project root:    {}\n", root.string());
        output += std::format("Build Directory: {}\n", dir->string());
        output += std::format("Build command:   {}\n", join_command(build_argv));

        auto const build  = run_merged(build_argv);
        output           += build.stdout_data;
        if (!output.empty() && output.back() != '\n') {
            output.push_back('\n');
        }

        if (build.exit_code != 0) {
            return {.output = std::move(output), .exit_code = 1};
        }

        auto const executable = find_executable(*dir, target);
        if (!executable) {
            // A target that produces no executable (an object/library) is a success.
            output += std::format("[SUCCESS] Build Passed (No Executable Found): {}\n", target);
            return {.output = std::move(output), .exit_code = 0};
        }

        std::vector<std::string> exec_argv{executable->string()};
        exec_argv.insert(exec_argv.end(), args.begin() + 1, args.end());
        output += "----------------------------------------------------------------\n";
        output += std::format("Exec command:    {}\n", join_command(exec_argv));

        auto const exec  = run_merged(exec_argv);
        output          += exec.stdout_data;
        if (!output.empty() && output.back() != '\n') {
            output.push_back('\n');
        }
        return {.output = std::move(output), .exit_code = exec.exit_code == 0 ? 0 : 1};
    }

} // namespace dana::tools
