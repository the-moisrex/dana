#include "../core/git.hpp"
#include "../runtime/args.hpp"
#include "../runtime/embed.hpp"
#include "../runtime/file_filter.hpp"
#include "../runtime/prompt_registry.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dana {

    namespace {

        std::string strip_trailing_newlines(std::string_view s) {
            std::string out(s);
            while (!out.empty() && out.back() == '\n') {
                out.pop_back();
            }
            return out;
        }

        // Detect the base branch exactly like the script: upstream (@{u}), falling
        // back to origin/main. `|| true` means a failing rev-parse yields "".
        std::string detect_base_branch(std::string_view given) {
            if (!given.empty()) {
                return std::string(given);
            }
            std::string upstream = git::upstream_branch();
            if (upstream.empty()) {
                return "origin/main";
            }
            return upstream;
        }

    } // namespace

    static void execute_git_worktree_diff(view const& v, result& r) {
        auto const head_lines = head_lines_or_throw(v);

        if (!git::is_inside_worktree()) [[unlikely]] {
            throw soft_error{"prompt git.worktree.diff: Error: Not inside a git repository.\n"};
        }

        // Custom flags: --not <commit>, --base <branch>; everything else is a file.
        auto rest = positional_args(v);

        std::string              base_branch;
        std::string              not_commit;
        std::vector<std::string> files;
        for (std::size_t i = 0; i < rest.size();) {
            if (rest[i] == "--not") {
                if (i + 1 >= rest.size()) {
                    throw usage_error{"Missing value for --not\n"};
                }
                not_commit  = rest[i + 1];
                i          += 2;
            } else if (rest[i] == "--base") {
                if (i + 1 >= rest.size()) {
                    throw usage_error{"Missing value for --base\n"};
                }
                base_branch  = rest[i + 1];
                i           += 2;
            } else {
                files.push_back(rest[i]);
                ++i;
            }
        }

        base_branch = detect_base_branch(base_branch);

        file_filter const filter = file_filter::from(v.prior, v.env);
        std::string       spec;
        if (filter.has_root() && !v.env.git_root.empty()) {
            spec = filter.root().string();
        }

        git::diff_request request;
        request.target           = git::diff_target::revision;
        request.revision         = base_branch;
        request.exclude_revision = not_commit;
        if (!files.empty()) {
            for (auto const& f : files) {
                if (!filter.excluded(v.env.git_root / f)) {
                    request.pathspec.push_back(f);
                }
            }
        } else if (!spec.empty()) {
            request.pathspec.push_back(spec);
        }

        std::string diff_output = strip_trailing_newlines(git::diff(".", request));
        if (!filter.exclude().empty()) {
            diff_output = dana::git::filter_diff_by_path(diff_output, {}, filter.exclude());
        }
        if (!diff_output.empty()) {
            std::string output;
            output += std::format("Diffs ({}..HEAD):\n", base_branch);
            output += "\n```diff\n";
            output += trim_context_nl(diff_output, head_lines);
            output += "```\n";
            r.out   = std::move(output);
            return;
        }
        r.out.clear();
        r.err = "No diffs found between current branch and " + base_branch + ".\n";
    }

    static void render_help_git_worktree_diff(std::ostream& os) {
        os << R"EOF(Usage: prompt git.worktree.diff [--not <commit>] [--base <branch>] [--head N] [FILE...]

Shows Git diffs for files changed since the upstream branch.
By default, compares against the upstream tracking branch (or origin/main).
Explicit FILE arguments restrict the diff to only those files.

Options:
  --not <commit>   Exclude a commit (and its ancestors) from the diff
  --base <branch>  Base branch to compare against (default: upstream or origin/main)
  --head N         Keep only the first N lines of embedded context
)EOF";
    }

    // This file is the prompt: it implements it and registers it at static-init time
    // (no central list — see sdk/prompt_registry.hpp).
    [[maybe_unused]] bool const registered = dana::register_prompt(
      "git.worktree.diff",
      "Shows Git diffs for files changed since the upstream branch. By default, compares against the "
      "upstream tracking branch (or origin/main). Explicit FILE arguments restrict the diff to only "
      "those files.",
      &execute_git_worktree_diff,
      &render_help_git_worktree_diff);

} // namespace dana
