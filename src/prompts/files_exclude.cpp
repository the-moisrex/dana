#include "../sdk/errors.hpp"
#include "../sdk/file_filter.hpp"
#include "../sdk/prompt_registry.hpp"

#include <algorithm>
#include <format>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace dana {

    namespace {

        constexpr std::string_view kSummary =
          "Exclude files from every file prompt in the same batch (fuzzy-picked when no pattern is given).";

        void execute_files_exclude(view const& v, result& r) {
            file_filter const filter = file_filter::from(v.prior, v.env);

            bool                     clear = false;
            bool                     list  = false;
            std::vector<std::string> patterns;

            for (auto const arg : v.args) {
                if (arg == "--clear") {
                    clear = true;
                } else if (arg == "--list") {
                    list = true;
                } else if (arg.size() > 1 && arg.front() == '-') {
                    throw usage_error{"files.exclude: unknown option " + std::string(arg)};
                } else {
                    patterns.emplace_back(arg);
                }
            }

            if (clear) {
                patterns.clear();
            } else if (patterns.empty() && !list) {
                // No pattern names a file to exclude: pick them.
                for (auto const& picked : select_files(v.env, filter)) {
                    patterns.push_back(display_path(v.env, picked));
                }
                if (patterns.empty()) {
                    r.err = "files.exclude: no files selected\n";
                    return;
                }
            }

            auto const fact = clear ? std::string{} : filter.exclude_fact(patterns);
            r.published.set(keys::excluded_files, fact);

            if (list || clear) {
                std::string report = clear ? "files.exclude: cleared\n" : "files.exclude:\n";
                if (!clear) {
                    for (auto const& pattern : patterns) {
                        report += "  " + pattern + "\n";
                    }
                    for (auto const pattern : filter.exclude()) {
                        if (std::ranges::find(patterns, pattern) == patterns.end()) {
                            report += "  ";
                            report += pattern;
                            report += '\n';
                        }
                    }
                }
                r.err = std::move(report);
            }
        }

        void render_help_files_exclude(std::ostream& os) {
            os << R"EOF(Usage: prompt .files.exclude [PATTERN...]
       prompt .files.exclude --clear | --list

Exclude files from every file prompt in the same batch: `.files`, `.read`,
`.files.grep`, `.read.grep`, `.grep`, `.glob`, `.git.files`, `.git-dirty`,
`.git.diff`, `.git.worktree.*`, `.verify`, `.commit` and the other prompts that
select or embed files. The filter is batch-wide, so it applies even when written
after the file prompts it affects (a sink such as `.ai.*` still ends the batch).

A PATTERN is matched against a file's absolute path, its path relative to the
git root, its path relative to `.files.scope`, and its basename. `*` and `?` are
wildcards; a pattern that names a directory excludes everything below it, and a
bare name matches a directory of that name anywhere in the tree.

With no PATTERN, `fzf -m` opens over the candidate files and the picked ones are
excluded. Repeated invocations accumulate: each publishes the union of the
previous exclusions and its own.

Options:
  --clear        Drop every exclusion
  --list         Print the effective exclusions on stderr
  -h, --help     Show this help

Examples:
  prompt .files.exclude '*.test.cpp' .files src
  prompt .files.exclude tests/ vendor/ .read.grep TODO
  prompt .files.exclude .files src            # pick the exclusions with fzf
)EOF";
        }

    } // namespace

    [[maybe_unused]] bool const registered_files_exclude =
      dana::register_file_filter("files.exclude", kSummary, &execute_files_exclude, &render_help_files_exclude);

} // namespace dana
