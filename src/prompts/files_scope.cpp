#include "../sdk/errors.hpp"
#include "../sdk/file_filter.hpp"
#include "../sdk/prompt_registry.hpp"

#include <filesystem>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>

namespace dana {

    namespace {

        constexpr std::string_view kSummary =
          "Restrict every file prompt in the same batch to one directory (fuzzy-picked when no directory is given).";

        void execute_files_scope(view const& v, result& r) {
            file_filter const filter = file_filter::from(v.prior, v.env);

            bool        clear = false;
            bool        list  = false;
            std::string wanted;

            for (auto const arg : v.args) {
                if (arg == "--clear") {
                    clear = true;
                } else if (arg == "--list") {
                    list = true;
                } else if (arg.size() > 1 && arg.front() == '-') {
                    throw usage_error{"files.scope: unknown option " + std::string(arg)};
                } else if (wanted.empty()) {
                    wanted = std::string(arg);
                } else {
                    throw usage_error{"files.scope: at most one DIRECTORY"};
                }
            }

            std::filesystem::path root;
            if (clear) {
                root.clear();
            } else if (!wanted.empty()) {
                auto const resolved = filter.resolve(v.env, wanted);
                if (!resolved) {
                    throw usage_error{"files.scope: directory not found: " + wanted};
                }
                std::error_code ec;
                if (!std::filesystem::is_directory(*resolved, ec) || ec) {
                    throw usage_error{"files.scope: not a directory: " + wanted};
                }
                root = std::filesystem::absolute(*resolved, ec).lexically_normal();
                if (ec) {
                    root = resolved->lexically_normal();
                }
            } else {
                auto const picked = select_directories(v.env, filter);
                if (picked.empty()) {
                    r.err = "files.scope: no directory selected\n";
                    return;
                }
                std::error_code ec;
                root = std::filesystem::absolute(picked.front(), ec).lexically_normal();
                if (ec) {
                    root = picked.front().lexically_normal();
                }
            }

            r.published.set(keys::file_root, root.generic_string());

            if (list || clear) {
                r.err = clear ? "files.scope: cleared\n" : "files.scope: " + root.generic_string() + "\n";
            }
        }

        void render_help_files_scope(std::ostream& os) {
            os << R"EOF(Usage: prompt .files.scope [DIRECTORY]
       prompt .files.scope --clear | --list

Restrict every file prompt in the same batch to DIRECTORY: it becomes the
default search path of `.grep`, `.glob`, `.files.grep` and `.read.grep`, the
candidate root of `.files`' and `.git.files`' `fzf` picker, the first place a
relative FILE argument is resolved, and (for the git prompts) an implicit
pathspec, so `.git-dirty`, `.git.diff`, `.verify` and `.commit` only see files
below it. The scope is batch-wide, so it applies even when written after the
file prompts it affects (a sink such as `.ai.*` still ends the batch).

DIRECTORY is resolved like any file argument (exact path, git root, then a
fuzzy match). With no DIRECTORY, `fzf -m` opens over the directories. Repeated
invocations replace the scope (the last one in the batch wins).

Options:
  --clear        Drop the scope
  --list         Print the effective scope on stderr
  -h, --help     Show this help

Examples:
  prompt .files.scope prompts/src .files
  prompt .files.scope src .grep TODO
  prompt .files.scope .files src            # pick the scope with fzf
)EOF";
        }

    } // namespace

    [[maybe_unused]] bool const registered_files_scope =
      dana::register_file_filter("files.scope", kSummary, &execute_files_scope, &render_help_files_scope);

} // namespace dana
