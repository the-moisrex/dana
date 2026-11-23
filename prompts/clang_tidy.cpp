#include "../core/fs.hpp"
#include "../core/git.hpp"
#include "../core/process.hpp"
#include "../runtime/args.hpp"
#include "../runtime/embed.hpp"
#include "../runtime/file_filter.hpp"
#include "../runtime/prompt_registry.hpp"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace dana {

    namespace {

        std::string strip_trailing_newlines(std::string s) {
            while (!s.empty() && s.back() == '\n') {
                s.pop_back();
            }
            return s;
        }

        std::string git_toplevel() {
            try {
                return git::toplevel();
            } catch (...) {
                return {};
            }
        }

    } // namespace

    static void execute_clang_tidy(view const& v, result& r) {
        namespace fs    = std::filesystem;
        auto head_lines = head_lines_or_throw(v);

        auto file_args = positional_args(v);

        std::string git_root = git_toplevel();

        // clang-tidy.sh runs on "${ARGS[@]}" (parse_arguments strips --head) but
        // displays "$*" — the MAIN positional params, which parse_arguments never
        // touches (a bash function's `set --`/`shift` is local). So `$*` keeps
        // --head verbatim. The `if [ $# -eq 0 ]` guard tests the raw arg count.
        file_filter const        filter = file_filter::from(v.prior, v.env);
        std::vector<std::string> run_args;     // ARGS
        std::vector<std::string> display_args; // $*
        if (v.args.empty()) {
            auto selected = filter.empty() ? select_files(v.env) : select_files(v.env, filter);
            if (selected.empty()) {
                r.out.clear();
                r.err = "No files specified or selected.\n";
                return;
            }
            for (auto const& s : selected) {
                display_args.emplace_back(s.string());
            }
            // Files from fzf only populate $@ (via `set --`), never ARGS.
        } else {
            for (std::string_view arg : v.args) {
                display_args.emplace_back(arg);
            }
            run_args = file_args;
        }

        // The chain-wide exclusions drop a file from both the run and the display.
        if (!filter.exclude().empty()) {
            std::erase_if(run_args, [&](std::string_view a) {
                return filter.excluded(a);
            });
            std::erase_if(display_args, [&](std::string_view a) {
                return filter.excluded(a);
            });
            if (run_args.empty() && display_args.empty()) {
                r.out.clear();
                r.err = "No files left after the chain's exclusions.\n";
                return;
            }
        }

        // find_build_dir: scan $PWD then the git root for a compile_commands.json.
        std::string build_dir;
        {
            std::vector<std::string> bases;
            bases.emplace_back(".");
            if (!git_root.empty()) {
                bases.push_back(git_root);
            }
            static constexpr std::array<char const*, 10> common_dirs = {
              "build",
              "build-dev",
              "build-dev-clang",
              "build-dev-gcc",
              "cmake-build-debug",
              "cmake-build-release",
              "out",
              "build/Debug",
              "build/Release",
              ".",
            };
            for (auto const& base : bases) {
                std::string found;
                for (char const* dir : common_dirs) {
                    auto const      check = fs::path(base) / dir / "compile_commands.json";
                    std::error_code ec;
                    if (fs::is_regular_file(check, ec)) {
                        found = fs::canonical(fs::path(base) / dir);
                        break;
                    }
                }
                if (!found.empty()) {
                    build_dir = found;
                    break;
                }
            }
        }

        std::vector<std::string> ct_args;
        ct_args.emplace_back("--quiet");
        if (!build_dir.empty()) {
            ct_args.emplace_back("-p");
            ct_args.push_back(build_dir);
        }

        std::string command_str = "clang-tidy ";
        for (std::size_t i = 0; i < ct_args.size(); ++i) {
            if (i != 0U) {
                command_str += ' ';
            }
            command_str += ct_args[i];
        }
        for (auto const& a : display_args) {
            command_str += ' ';
            command_str += a;
        }

        // clang-tidy "${CT_ARGS[@]}" "${ARGS[@]}" 2>&1 || true
        std::vector<std::string> storage = {"bash", "-c", "clang-tidy \"$@\" 2>&1", "clang-tidy"};
        for (auto const& a : ct_args) {
            storage.push_back(a);
        }
        for (auto const& a : run_args) {
            storage.push_back(a);
        }

        auto result = dana::process::run_command(storage);

        std::string output;
        output += '\n';
        output += "Review the following `" + command_str +
                  "` output. Identify the issues, explain why they were flagged, and provide the most robust "
                  "and idiomatic fixes. Suggest small git patches or refactored code blocks where "
                  "appropriate.\n";
        output += "\n\n```text\n";
        // clang-tidy.sh: run_output=$(clang-tidy ...); if --head then
        // run_output="$(printf "%s" "$run_output" | head -n N)"; then
        // printf '```text\n%s\n```\n' "$run_output". `head -n 0` prints nothing
        // (unlike trim_context's sed), and the inner $(...) re-strips trailing
        // newlines, so a trailing blank line is dropped.
        std::string run_output = strip_trailing_newlines(result.stdout_data);
        if (head_lines) {
            run_output = head_output(run_output, *head_lines);
        }
        output += run_output;
        output += "\n\n```\n";

        r.out = std::move(output);
    }

    static void render_help_clang_tidy(std::ostream& os) {
        os << R"EOF(Usage: prompt clang-tidy [--head N] [FILE...]
       some-command | prompt clang-tidy [--head N] [FILE...]

Runs `clang-tidy` on the provided files and outputs a prompt to fix the identified issues.
If no files are provided, `fzf -m` is used to choose them interactively.

Options:
  --head N   Keep only the first N lines of the output
  -h, --help Show this help
)EOF";
    }

    // This file is the prompt: it implements it and registers it at static-init time
    // (no central list — see sdk/prompt_registry.hpp).
    [[maybe_unused]] bool const registered = dana::register_prompt(
      "clang-tidy",
      "Runs `clang-tidy` on the provided files and outputs a prompt to fix the identified "
      "issues. If no files are provided, `fzf -m` is used to choose them interactively.",
      &execute_clang_tidy,
      &render_help_clang_tidy);

} // namespace dana
