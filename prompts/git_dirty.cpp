#include "../core/fs.hpp"
#include "../core/git.hpp"
#include "../core/process.hpp"
#include "../runtime/args.hpp"
#include "../runtime/embed.hpp"
#include "../runtime/file_filter.hpp"
#include "../runtime/file_scope.hpp"
#include "../runtime/prompt_registry.hpp"

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace dana {

    namespace {

        std::string git_raw(text::string_views args) {
            return dana::process::run_command(args).stdout_data;
        }

        // Port of `var="$(git ...)"` — trailing newlines stripped (matters for the
        // `[[ -n ]]` content checks and the `echo "$var"` renders).
        std::string capture(text::string_views args) {
            std::string out = git_raw(args);
            while (!out.empty() && out.back() == '\n') {
                out.pop_back();
            }
            return out;
        }

        // Split an already-stripped (\n-free-trailing) string on '\n'; no trailing
        // empty entry — mirrors the `<<< "$var"` here-strings the script feeds to read.
        std::vector<std::string> split_lines(std::string_view s) {
            std::vector<std::string> out;
            if (s.empty()) {
                return out;
            }
            std::size_t start = 0;
            while (true) {
                auto const nl = s.find('\n', start);
                if (nl == std::string_view::npos) {
                    out.emplace_back(s.substr(start));
                    break;
                }
                out.emplace_back(s.substr(start, nl - start));
                start = nl + 1;
                if (start == s.size()) {
                    break;
                }
            }
            return out;
        }

    } // namespace

    static void execute_git_dirty(view const& v, result& r) {
        // This prompt parses its own flags (not the shared --head parser), and its
        // --head accepts any value verbatim. --help/-h are intercepted by the
        // dispatcher before we run.
        std::optional<std::size_t> head_lines;
        bool                       numbered         = false;
        bool                       filter_staged    = false;
        bool                       filter_unstaged  = false;
        bool                       filter_all       = false;
        bool                       except           = false;
        bool                       output_files     = false;
        bool                       output_diff      = false;
        bool                       output_full      = false;
        bool                       output_full_diff = false;
        bool                       dedup            = true;

        for (std::size_t i = 0; i < v.args.size();) {
            std::string_view a = v.args[i];
            if (a == "--help" || a == "-h") {
                ++i; // handled by the dispatcher; unreachable in practice
            } else if (a == "--head") {
                if (i + 1 >= v.args.size()) {
                    throw usage_error{"Missing value for --head\n"};
                }
                head_lines  = static_cast<std::size_t>(std::strtoull(std::string(v.args[i + 1]).c_str(), nullptr, 10));
                i          += 2;
            } else if (is_number_flag(a)) {
                numbered = true;
                ++i;
            } else if (a == "--staged" || a == "-s") {
                filter_staged = true;
                ++i;
            } else if (a == "--uncommitted" || a == "-u") {
                filter_unstaged = true;
                ++i;
            } else if (a == "--all" || a == "-a") {
                filter_all = true;
                ++i;
            } else if (a == "--except") {
                except = true;
                ++i;
            } else if (a == "--files" || a == "-l") {
                output_files = true;
                ++i;
            } else if (a == "--diff" || a == "-d") {
                output_diff = true;
                ++i;
            } else if (a == "--full" || a == "-f") {
                output_full = true;
                ++i;
            } else if (a == "--full-diff" || a == "-F") {
                output_full_diff = true;
                ++i;
            } else if (a == kNoDedupFlag) {
                dedup = false;
                ++i;
            } else {
                throw soft_error{"prompt git-dirty: unknown option: " + std::string(a) +
                                 "\nTry 'prompt git-dirty --help' for usage.\n"};
            }
        }

        if (!filter_staged && !filter_unstaged && !filter_all) {
            filter_all = true;
        }
        if (!output_files && !output_diff && !output_full && !output_full_diff) {
            output_files = true;
        }

        if (process::run_command({"git", "rev-parse", "--is-inside-work-tree"}).exit_code != 0) {
            throw soft_error{"prompt git-dirty: Error: Not inside a git repository.\n"};
        }

        std::filesystem::path git_root = v.env.git_root;
        file_filter const     filter   = file_filter::from(v.prior, v.env);

        // A scope is an implicit pathspec: the git commands only look below it.
        std::string spec;
        if (filter.has_root() && !git_root.empty()) {
            spec = filter.root().lexically_relative(git_root).generic_string();
            if (spec == ".") {
                spec.clear();
            }
        }
        auto const capture_in_scope = [&](std::vector<std::string> args) {
            if (!spec.empty()) {
                args.emplace_back("--");
                args.push_back(spec);
            }
            return capture(args);
        };

        std::string staged_files;
        std::string staged_diff;
        std::string unstaged_files;
        std::string unstaged_diff;
        std::string untracked_files;

        if (filter_staged) {
            staged_files = capture_in_scope(std::vector<std::string>{"git", "diff", "--cached", "--name-status"});
            staged_diff  = capture_in_scope(std::vector<std::string>{"git", "diff", "--cached"});
        }
        if (filter_unstaged) {
            unstaged_files = capture_in_scope(std::vector<std::string>{"git", "diff", "--name-status"});
            unstaged_diff  = capture_in_scope(std::vector<std::string>{"git", "diff"});
            untracked_files =
              capture_in_scope(std::vector<std::string>{"git", "ls-files", "--others", "--exclude-standard"});
        }
        if (filter_all) {
            staged_files   = capture_in_scope(std::vector<std::string>{"git", "diff", "--cached", "--name-status"});
            staged_diff    = capture_in_scope(std::vector<std::string>{"git", "diff", "--cached"});
            unstaged_files = capture_in_scope(std::vector<std::string>{"git", "diff", "--name-status"});
            unstaged_diff  = capture_in_scope(std::vector<std::string>{"git", "diff"});
            untracked_files =
              capture_in_scope(std::vector<std::string>{"git", "ls-files", "--others", "--exclude-standard"});
        }

        // --except inverts the active filter. The branch order (staged, unstaged,
        // all) matches the script's if/elif/elif, so e.g. --staged --all --except
        // takes the staged branch.
        if (except) {
            if (filter_staged) {
                unstaged_files = capture_in_scope(std::vector<std::string>{"git", "diff", "--name-status"});
                unstaged_diff  = capture_in_scope(std::vector<std::string>{"git", "diff"});
                untracked_files =
                  capture_in_scope(std::vector<std::string>{"git", "ls-files", "--others", "--exclude-standard"});
                staged_files.clear();
                staged_diff.clear();
            } else if (filter_unstaged) {
                staged_files = capture_in_scope(std::vector<std::string>{"git", "diff", "--cached", "--name-status"});
                staged_diff  = capture_in_scope(std::vector<std::string>{"git", "diff", "--cached"});
                unstaged_files.clear();
                unstaged_diff.clear();
                untracked_files.clear();
            } else if (filter_all) {
                staged_files.clear();
                staged_diff.clear();
                unstaged_files.clear();
                unstaged_diff.clear();
                untracked_files.clear();
            }
        }

        // The chain's exclusions drop whole files from every list and block.
        auto keep = [&](std::string_view filepath) {
            return !filter.excluded(git_root / std::string(filepath));
        };
        auto filter_name_status = [&](std::string& list) {
            if (filter.exclude().empty()) {
                return;
            }
            std::string kept;
            for (auto const& line : split_lines(list)) {
                std::size_t const      tab = line.find('\t');
                std::string_view const filepath =
                  tab == std::string::npos ? std::string_view{} : std::string_view(line).substr(tab + 1);
                if (filepath.empty() || keep(filepath)) {
                    kept += line;
                    kept += '\n';
                }
            }
            while (!kept.empty() && kept.back() == '\n') {
                kept.pop_back();
            }
            list = std::move(kept);
        };
        auto filter_plain = [&](std::string& list) {
            if (filter.exclude().empty()) {
                return;
            }
            std::string kept;
            for (auto const& line : split_lines(list)) {
                if (keep(line)) {
                    kept += line;
                    kept += '\n';
                }
            }
            while (!kept.empty() && kept.back() == '\n') {
                kept.pop_back();
            }
            list = std::move(kept);
        };
        filter_name_status(staged_files);
        filter_name_status(unstaged_files);
        filter_plain(untracked_files);
        if (!filter.exclude().empty()) {
            staged_diff   = dana::git::filter_diff_by_path(staged_diff, {}, filter.exclude());
            unstaged_diff = dana::git::filter_diff_by_path(unstaged_diff, {}, filter.exclude());
        }

        bool has_content = !staged_files.empty() || !unstaged_files.empty() || !untracked_files.empty();
        if (!has_content) {
            r.out.clear();
            r.err = "No changes found in the repository.\n";
            return;
        }

        std::string output;
        output += '\n'; // the unconditional `echo` after the content check

        file_scope scope(v.prior);

        // Reads GIT_ROOT/filepath as a full file block with the given heading
        // suffix (e.g. "(new)" or "(status: M)"); a no-op when the file is absent.
        // `track` marks a full-file emission for the chain-wide dedup; a diff-only
        // mode passes false, since a diff is not the file's content.
        auto emit_file = [&](std::string_view filepath, std::string_view heading, bool track) {
            std::filesystem::path full = git_root / filepath;
            std::error_code       ec;
            if (!std::filesystem::is_regular_file(full, ec) || ec) {
                return;
            }
            if (filter.excluded(full)) {
                return;
            }
            if (track && dedup && scope.seen(full)) {
                return;
            }
            auto const name    = full.filename();
            auto const content = read_file(full);
            if (track) {
                scope.mark(full);
            }
            output += std::format("File: {} {}\n", filepath, heading);
            output += '\n';
            output += std::format("```{}\n", infer_lang(name));
            output += number_lines(trim_context_nl(content, head_lines), numbered);
            output += "```\n\n";
        };

        // Iterate a name-status list ("M\tpath" per line), splitting on the first
        // tab exactly like `IFS=$'\t' read -r status filepath`.
        auto const each_name_status = [&](std::string_view files, auto&& fn) {
            for (auto const& line : split_lines(files)) {
                std::size_t const tab      = line.find('\t');
                std::string const status   = line.substr(0, tab);
                std::string const filepath = (tab == std::string::npos) ? std::string{} : line.substr(tab + 1);
                fn(status, filepath);
            }
        };

        if (output_files) {
            if (!staged_files.empty()) {
                output += "Staged changes:\n```text\n" + staged_files + "\n```\n\n";
            }
            if (!unstaged_files.empty()) {
                output += "Unstaged changes:\n```text\n" + unstaged_files + "\n```\n\n";
            }
            if (!untracked_files.empty()) {
                output += "Untracked files:\n```text\n" + untracked_files + "\n```\n\n";
            }
        } else if (output_diff) {
            if (!staged_diff.empty()) {
                output += std::format("Staged diff:\n\n```diff\n{}```\n\n", trim_context_nl(staged_diff, head_lines));
            }
            if (!unstaged_diff.empty()) {
                output +=
                  std::format("Unstaged diff:\n\n```diff\n{}```\n\n", trim_context_nl(unstaged_diff, head_lines));
            }
            if (!untracked_files.empty()) {
                output += "Untracked files:\n\n";
                for (auto const& filepath : split_lines(untracked_files)) {
                    emit_file(filepath, "(new)", false);
                }
            }
        } else if (output_full) {
            if (!staged_files.empty()) {
                output += "Staged files:\n\n";
                each_name_status(staged_files, [&](std::string_view const st, std::string_view const fp) {
                    emit_file(fp, std::format("(status: {})", st), true);
                });
            }
            if (!unstaged_files.empty()) {
                output += "Unstaged files:\n\n";
                each_name_status(unstaged_files, [&](std::string_view const st, std::string_view const fp) {
                    emit_file(fp, std::format("(status: {})", st), true);
                });
            }
            if (!untracked_files.empty()) {
                output += "Untracked files:\n\n";
                for (auto const& filepath : split_lines(untracked_files)) {
                    emit_file(filepath, "(new)", true);
                }
            }
        } else if (output_full_diff) {
            if (!staged_files.empty()) {
                output += "Staged files:\n\n";
                each_name_status(staged_files, [&](std::string_view const st, std::string_view fp) {
                    emit_file(fp, std::format("(status: {})", st), true);
                });
                if (!staged_diff.empty()) {
                    output += std::format("Diffs:\n\n```diff\n{}```\n\n", trim_context_nl(staged_diff, head_lines));
                }
            }
            if (!unstaged_files.empty()) {
                output += "Unstaged files:\n\n";
                each_name_status(unstaged_files, [&](std::string_view const st, std::string_view const fp) {
                    emit_file(fp, std::format("(status: {})", st), true);
                });
                if (!unstaged_diff.empty()) {
                    output += std::format("Diffs:\n\n```diff\n{}```\n\n", trim_context_nl(unstaged_diff, head_lines));
                }
            }
            if (!untracked_files.empty()) {
                output += "Untracked files:\n\n";
                for (auto const& filepath : split_lines(untracked_files)) {
                    emit_file(filepath, "(new)", true);
                }
            }
        }

        r.out = std::move(output);
        r.published.set(keys::files, scope.fact());
    }

    static void render_help_git_dirty(std::ostream& os) {
        os << R"EOF(Usage: prompt git-dirty [OPTIONS]

Show uncommitted/changed files in the current Git repository.
By default it lists all staged and unstaged changes (--all --files);
the options below switch to diffs or full file contents.

FILTER OPTIONS (what to show):
  --staged, -s           Only staged (cached) changes
  --uncommitted, -u      Only unstaged working tree changes
  --all, -a              Both staged and unstaged (default)
  --except               Invert the active filter
                         e.g. --staged --except = everything except staged
                              --all --except    = nothing (no filter)

OUTPUT OPTIONS (how to show):
  --files, -l            List filenames with status (default)
  --diff, -d             Show only the diffs
  --full, -f             Show full file contents
  --full-diff, -F        Show full files + diff context (@@ hunks with +/-)

OTHER:
)EOF" << file_options_help("embedded context", true)
           << R"EOF(
DEFAULTS: --all --files

EXAMPLES:
  prompt git-dirty                          # All changes, file list
  prompt git-dirty --staged --diff          # Staged diffs only
  prompt git-dirty --uncommitted --full     # Full contents of unstaged files
  prompt git-dirty --staged --except        # Everything except staged
  prompt git-dirty --full-diff              # Full files with diff markers
)EOF";
    }

    // This file is the prompt: it implements it and registers it at static-init time
    // (no central list — see sdk/prompt_registry.hpp).
    [[maybe_unused]] bool const registered = dana::register_file_prompt(
      "git-dirty",
      "Show uncommitted/changed files in the current Git repository. By default it lists all staged "
      "and unstaged changes (--all --files); the options below switch to diffs or full file contents.",
      &execute_git_dirty,
      &render_help_git_dirty);

} // namespace dana
