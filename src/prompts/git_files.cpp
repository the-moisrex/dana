#include "../core/fs.hpp"
#include "../sdk/args.hpp"
#include "../sdk/embed.hpp"
#include "../sdk/file_filter.hpp"
#include "../sdk/file_scope.hpp"
#include "../sdk/prompt_registry.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
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

    } // namespace

    static void execute_git_files(view const& v, result& r) {
        auto opts = file_options_or_throw(v);

        auto              file_args = positional_args(v);
        file_filter const filter    = file_filter::from(v.prior, v.env);

        // Resolve inputs. With no args the script falls back to `select_files`
        // (fzf -m); the native dispatcher wires select_files(v.env) to that.
        std::vector<std::filesystem::path> selected;
        std::string                        errors;
        if (file_args.empty()) {
            selected = filter.empty() ? select_files(v.env) : select_files(v.env, filter);
        } else {
            for (auto const& arg : file_args) {
                auto resolved = filter.resolve(v.env, arg);
                if (!resolved) {
                    errors += std::format("prompt git.files: file not found: {}\n", arg);
                    continue;
                }
                if (::access(resolved->c_str(), R_OK) != 0) {
                    errors += "prompt git.files: file not readable: \"" + arg + "\"; resolved to \"" +
                              resolved->string() + "\"\n";
                    continue;
                }
                selected.push_back(*resolved);
            }
        }

        std::optional<std::filesystem::path> git_root;
        if (!v.env.git_root.empty()) {
            git_root = v.env.git_root;
        }

        std::string output;
        file_scope  scope(v.prior);
        for (auto const& file : selected) {
            if (!std::filesystem::exists(file) || !std::filesystem::is_regular_file(file)) {
                continue;
            }
            if (filter.excluded(file)) {
                continue;
            }
            if (opts.dedup && scope.seen(file)) {
                continue;
            }

            auto const rel  = fs::relative_path(file, git_root);
            auto const lang = infer_lang(file.filename());
            auto const raw  = read_file(file);

            // [ -n "$content" ] where content = $(cat) with trailing newlines stripped.
            if (strip_trailing_newlines(raw).empty()) {
                continue;
            }

            scope.mark(file);
            output += std::format("File {}", rel.string());
            if (opts.head_lines) {
                output += " (first " + std::to_string(*opts.head_lines) + " lines)";
            }
            output += "\n\n";
            output += std::format("```{}\n", lang);
            output += number_lines(trim_context_nl(raw, opts.head_lines), opts.numbered);
            output += "\n```\n\n";
        }

        if (!errors.empty()) {
            r.err = std::move(errors);
        }
        r.out = std::move(output);
        r.published.set(keys::files, scope.fact());
    }

    static void render_help_git_files(std::ostream& os) {
        os << file_usage_help("git.files") << R"EOF(
Embeds the given files as Markdown code blocks.
File headings are printed relative to the Git repository root.
If no files are provided, `fzf -m` is used to choose from tracked files.

Options:
)EOF" << file_options_help("each embedded file");
    }

    // This file is the prompt: it implements it and registers it at static-init time
    // (no central list — see sdk/prompt_registry.hpp).
    [[maybe_unused]] bool const registered = dana::register_file_prompt(
      "git.files",
      "Embeds the given files as Markdown code blocks. File headings are printed relative to the Git "
      "repository root. If no files are provided, `fzf -m` is used to choose from tracked files.",
      &execute_git_files,
      &render_help_git_files);

} // namespace dana
