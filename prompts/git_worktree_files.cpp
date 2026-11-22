#include "../core/fs.hpp"
#include "../core/process.hpp"
#include "../runtime/args.hpp"
#include "../runtime/embed.hpp"
#include "../runtime/file_filter.hpp"
#include "../runtime/file_scope.hpp"
#include "../runtime/prompt_registry.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace dana {

    namespace {

        // bash captures with `2>/dev/null` — the tool's diagnostics never reach the
        // user (git_raw is for `$(...)` of a command that may legitimately fail).
        std::string git_raw(text::string_views args) {
            return dana::process::command{args}.discard_stderr().run().stdout_data;
        }

        std::string strip_trailing_newlines(std::string_view s) {
            std::string out(s);
            while (!out.empty() && out.back() == '\n') {
                out.pop_back();
            }
            return out;
        }

        // Port of `changed_files=($(git diff ... --name-only))` — bash word-splits the
        // captured output on IFS (space/tab/newline).
        std::vector<std::string> word_split(std::string_view s) {
            std::vector<std::string> out;
            std::size_t              i = 0;
            while (i < s.size()) {
                while (i < s.size() && (s.at(i) == ' ' || s.at(i) == '\t' || s.at(i) == '\n' || s.at(i) == '\r')) {
                    ++i;
                }
                std::size_t start = i;
                while (i < s.size() && s.at(i) != ' ' && s.at(i) != '\t' && s.at(i) != '\n' && s.at(i) != '\r') {
                    ++i;
                }
                if (i > start) {
                    out.emplace_back(s.substr(start, i - start));
                }
            }
            return out;
        }

        std::string detect_base_branch(std::string_view const given) {
            if (!given.empty()) {
                return std::string(given);
            }
            std::string upstream = strip_trailing_newlines(git_raw({"git", "rev-parse", "--abbrev-ref", "@{u}"}));
            if (upstream.empty()) {
                return "origin/main";
            }
            return upstream;
        }

    } // namespace

    static void execute_git_worktree_files(view const& v, result& r) {
        auto opts = file_options_or_throw(v);

        if (dana::process::run_command({"git", "rev-parse", "--is-inside-work-tree"}).exit_code != 0) {
            throw soft_error{"prompt git.worktree.files: Error: Not inside a git repository.\n"};
        }

        auto rest = positional_args(v);

        std::string              base_branch;
        std::string              not_commit;
        std::vector<std::string> files;
        for (std::size_t i = 0; i < rest.size();) {
            if (rest.at(i) == "--not") {
                if (i + 1 >= rest.size()) {
                    throw usage_error{"Missing value for --not\n"};
                }
                not_commit  = rest[i + 1];
                i          += 2;
            } else if (rest.at(i) == "--base") {
                if (i + 1 >= rest.size()) {
                    throw usage_error{"Missing value for --base\n"};
                }
                base_branch  = rest[i + 1];
                i           += 2;
            } else {
                files.push_back(rest.at(i));
                ++i;
            }
        }

        base_branch = detect_base_branch(base_branch);

        file_filter const filter = file_filter::from(v.prior, v.env);
        std::string       spec;
        if (filter.has_root() && !v.env.git_root.empty()) {
            spec = filter.root().lexically_relative(v.env.git_root).generic_string();
            if (spec == ".") {
                spec.clear();
            }
        }

        std::vector<std::string> changed_files;
        if (!files.empty()) {
            changed_files = files;
        } else {
            std::vector<std::string> diff_args = {base_branch};
            if (!not_commit.empty()) {
                diff_args.emplace_back("--not");
                diff_args.push_back(not_commit);
            }
            std::vector<std::string> git_cmd = {"git", "diff"};
            for (auto const& a : diff_args) {
                git_cmd.push_back(a);
            }
            git_cmd.emplace_back("--name-only");
            git_cmd.emplace_back("--");
            if (!spec.empty()) {
                git_cmd.push_back(spec);
            }
            changed_files = word_split(git_raw(git_cmd));
        }

        if (!filter.exclude().empty()) {
            std::erase_if(changed_files, [&](std::string_view file) {
                return filter.excluded(v.env.git_root / file);
            });
        }

        if (changed_files.empty()) {
            r.out.clear();
            r.err = "No changed files found between current branch and " + base_branch + ".\n";
            return;
        }

        std::optional<std::filesystem::path> git_root;
        if (!v.env.git_root.empty()) {
            git_root = v.env.git_root;
        }

        std::string output;
        std::string error;
        file_scope  scope(v.prior);
        for (auto const& file : changed_files) {
            auto resolved = resolve_input_file(v.env, file);
            if (!resolved) {
                error += std::format("prompt git.worktree.files: file not found: {}\n", file);
                continue;
            }
            if (::access(resolved->c_str(), R_OK) != 0) {
                error += "prompt git.worktree.files: file not readable: \"" + file + "\"; resolved to \"" +
                         resolved->string() + "\"\n";
                continue;
            }
            if (opts.dedup && scope.seen(*resolved)) {
                continue;
            }
            if (filter.excluded(*resolved)) {
                continue;
            }

            auto const rel  = fs::relative_path(*resolved, git_root);
            auto const lang = infer_lang(resolved->filename());
            auto const raw  = read_file(*resolved);
            if (strip_trailing_newlines(raw).empty()) {
                continue;
            }

            scope.mark(*resolved);
            output += std::format("File {}", rel.string());
            if (opts.head_lines) {
                output += " (first " + std::to_string(*opts.head_lines) + " lines)";
            }
            output += "\n\n";
            output += std::format("```{}\n", lang);
            output += number_lines(trim_context_nl(raw, opts.head_lines), opts.numbered);
            output += "\n```\n\n";
        }

        if (!error.empty()) {
            r.err = std::move(error);
        }
        r.out = std::move(output);
        r.published.set(keys::files, scope.fact());
    }

    static void render_help_git_worktree_files(std::ostream& os) {
        os << file_usage_help("git.worktree.files", "[FILE...]", "", "[--not <commit>] [--base <branch>]") << R"EOF(
Embeds files changed since the upstream branch as Markdown code blocks.
By default, compares against the upstream tracking branch (or origin/main).
Explicit FILE arguments restrict embedding to only those changed files.

Options:
)EOF" << file_options_help("each embedded file")
           << R"EOF(  --not <commit>   Exclude a commit (and its ancestors) from the diff
  --base <branch>  Base branch to compare against (default: upstream or origin/main)
)EOF";
    }

    // This file is the prompt: it implements it and registers it at static-init time
    // (no central list — see sdk/prompt_registry.hpp).
    [[maybe_unused]] bool const registered = dana::register_file_prompt(
      "git.worktree.files",
      "Embeds files changed since the upstream branch as Markdown code blocks. By default, compares "
      "against the upstream tracking branch (or origin/main). Explicit FILE arguments restrict "
      "embedding to only those changed files.",
      &execute_git_worktree_files,
      &render_help_git_worktree_files);

} // namespace dana
