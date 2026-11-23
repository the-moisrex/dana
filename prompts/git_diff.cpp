#include "../core/fs.hpp"
#include "../core/git.hpp"
#include "../runtime/args.hpp"
#include "../runtime/embed.hpp"
#include "../runtime/file_filter.hpp"
#include "../runtime/prompt_registry.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
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

        std::string join_lines(std::vector<std::string> const& lines) {
            std::string out;
            for (auto const& line : lines) {
                out += line;
                out.push_back('\n');
            }
            return out;
        }

    } // namespace

    static void execute_git_diff(view const& v, result& r) {
        auto opts = file_options_or_throw(v);

        if (!git::is_inside_worktree()) [[unlikely]] {
            throw soft_error{"prompt git.diff: Error: Not inside a git repository.\n"};
        }

        auto const        files  = positional_args(v);
        file_filter const filter = file_filter::from(v.prior, v.env);

        std::filesystem::path git_root = v.env.git_root;

        // A scope is an implicit pathspec.
        std::string spec;
        if (filter.has_root() && !git_root.empty()) {
            spec = filter.root().lexically_relative(git_root).generic_string();
            if (spec == ".") {
                spec.clear();
            }
        }

        std::string output;
        if (!files.empty()) {
            std::vector<std::string> pathspec;
            for (auto const& f : files) {
                if (!filter.excluded(git_root / f)) {
                    pathspec.push_back(f);
                }
            }
            std::string diff_output = strip_trailing_newlines(
              git::diff(".", {.target = git::diff_target::workdir, .pathspec = std::move(pathspec)}));
            if (!filter.exclude().empty()) {
                diff_output = dana::git::filter_diff_by_path(diff_output, {}, filter.exclude());
            }
            if (!diff_output.empty()) {
                output += "Diffs:\n\n```diff\n";
                output += trim_context_nl(diff_output, opts.head_lines);
                output += "```\n";
            } else {
                r.out.clear();
                r.err = "No diffs found for the specified files.\n";
                return;
            }
        } else {
            std::vector<std::string> pathspec;
            if (!spec.empty()) {
                pathspec.push_back(spec);
            }
            std::string unstaged_diff =
              strip_trailing_newlines(git::diff(".", {.target = git::diff_target::workdir, .pathspec = pathspec}));
            std::string untracked = strip_trailing_newlines(join_lines(git::untracked_files(".", pathspec)));

            if (!filter.exclude().empty()) {
                unstaged_diff = dana::git::filter_diff_by_path(unstaged_diff, {}, filter.exclude());
            }

            if (!unstaged_diff.empty()) {
                output += "Unstaged changes:\n\n```diff\n";
                output += trim_context_nl(unstaged_diff, opts.head_lines);
                output += "```\n\n";
            }

            if (!untracked.empty()) {
                output            += "Untracked files:\n\n";
                // `while IFS= read -r filepath; do ... done <<< "$untracked"` — the
                // <<< adds one trailing newline, so split the stripped list on \n.
                std::size_t start  = 0;
                while (start <= untracked.size()) {
                    std::size_t const nl = untracked.find('\n', start);
                    std::string const filepath =
                      (nl == std::string::npos) ? untracked.substr(start) : untracked.substr(start, nl - start);
                    std::error_code             ec;
                    std::filesystem::path const full = std::filesystem::absolute(filepath, ec);
                    if (!filepath.empty() && !ec && std::filesystem::is_regular_file(full, ec) && !ec &&
                        !filter.excluded(full))
                    {
                        auto const        lang     = infer_lang(std::string_view{filepath.data(), filepath.size()});
                        std::string const content  = read_file(full);
                        output                    += std::format("File: {} (new)\n", filepath);
                        output                    += '\n';
                        output                    += std::format("```{}\n", lang);
                        output += number_lines(trim_context_nl(content, opts.head_lines), opts.numbered);
                        output += "```\n\n";
                    }
                    if (nl == std::string::npos) {
                        break;
                    }
                    start = nl + 1;
                }
            }

            if (unstaged_diff.empty() && untracked.empty()) {
                r.out.clear();
                r.err = "No changes found in the working tree.\n";
                return;
            }
        }

        r.out = std::move(output);
    }

    static void render_help_git_diff(std::ostream& os) {
        os << file_usage_help("git.diff", "[FILE...]", "") << R"EOF(
Shows Git diffs for the specified files.
If no files are provided, shows all unstaged changes and untracked files.

Options:
)EOF" << file_options_help("embedded context");
    }

    // This file is the prompt: it implements it and registers it at static-init time
    // (no central list — see sdk/prompt_registry.hpp).
    [[maybe_unused]] bool const registered = dana::register_prompt(
      "git.diff",
      "Shows Git diffs for the specified files. If no files are provided, shows all unstaged changes "
      "and untracked files.",
      &execute_git_diff,
      &render_help_git_diff);

} // namespace dana
