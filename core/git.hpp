#pragma once

#include "strings.hpp"

#include <cstddef>
#include <git2.h>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dana::git {

    // Annotate ordinary unified diffs using destination-file line numbers.
    // Combined diffs (multiple prefix columns) are left unchanged.
    std::string add_line_numbers_to_diff(std::string_view diff);

    // Keep/drop whole-file blocks of a unified diff.
    //   * `includes` non-empty => only blocks whose path matches one of them are kept.
    //   * `excludes`          => blocks whose path matches any of them are dropped.
    // Paths are matched both verbatim and by basename, using `detail::glob_match`.
    std::string filter_diff_by_path(std::string_view diff, text::string_views includes, text::string_views excludes);

    // Thread-safe process-wide runtime manager for libgit2.
    class [[nodiscard]] runtime final {
      public:
        static void ensure_initialized();

        runtime(runtime const&)                = delete;
        runtime& operator=(runtime const&)     = delete;
        runtime(runtime&&) noexcept            = delete;
        runtime& operator=(runtime&&) noexcept = delete;

      private:
        runtime();
        ~runtime();
    };

    // RAII deleter for git_repository handles.
    struct repository_deleter final {
        void operator()(git_repository* repo) const noexcept;
    };

    using repository_ptr = std::unique_ptr<git_repository, repository_deleter>;

    // Extracts the last libgit2 error message safely without throwing.
    [[nodiscard]] std::string_view last_error_message() noexcept;

    // Opens a git repository, searching parent directories from `path`.
    [[nodiscard]] repository_ptr open_repository(
      std::string_view path         = ".",
      unsigned int     flags        = GIT_REPOSITORY_OPEN_CROSS_FS,
      char const*      ceiling_dirs = nullptr);

    // Attempts to open a repository, returning an empty pointer if none exists.
    [[nodiscard]] repository_ptr try_open_repository(
      std::string_view path         = ".",
      unsigned int     flags        = GIT_REPOSITORY_OPEN_CROSS_FS,
      char const*      ceiling_dirs = nullptr) noexcept;

    // High-performance check for whether `path` is inside any git worktree.
    // Does not allocate a repository handle if none is found.
    [[nodiscard]] bool is_inside_worktree(std::string_view path = ".") noexcept;

    // -------------------------------------------------------------------------
    // Repository queries (libgit2; no subprocess)
    // -------------------------------------------------------------------------

    enum struct [[nodiscard]] diff_target : std::uint8_t {
        workdir,  // index -> working tree (git diff)
        cached,   // HEAD -> index (git diff --cached)
        revision, // tree of `revision` -> working tree (git diff REV)
    };

    struct [[nodiscard]] diff_request {
        diff_target              target = diff_target::workdir;
        std::string              revision{};         // revision target: base revision
        std::string              exclude_revision{}; // `--not`: diff exclude_revision..revision
        std::vector<std::string> pathspec{};         // cwd-relative or absolute paths
        bool                     name_only              = false;
        bool                     name_status            = false;
        bool                     include_untracked      = false;
        bool                     show_untracked_content = false;
    };

    // The current branch name, or "" for a detached/unborn HEAD
    // (`git branch --show-current`).
    std::string current_branch(std::string_view path = ".");

    // `git status --short`.
    std::string status_short(std::string_view path = ".");

    // `git log --oneline -<count>` (newest first, one trailing newline per line).
    std::string log_oneline(std::size_t count, std::string_view path = ".");

    // A unified diff (or a name list) produced by libgit2.
    std::string diff(std::string_view path, diff_request const& request);

    // Repository paths of tracked files (`git ls-files`), sorted. Paths are
    // relative to `path` and limited to its subtree, exactly like running
    // `git ls-files` in that directory. `pathspec` entries are cwd-relative or
    // absolute.
    std::vector<std::string> tracked_files(std::string_view path, std::vector<std::string> const& pathspec = {});

    // Paths of untracked-but-not-ignored files
    // (`git ls-files --others --exclude-standard`), sorted and relative to `path`.
    std::vector<std::string> untracked_files(std::string_view path, std::vector<std::string> const& pathspec = {});

    // Tracked plus untracked-but-not-ignored files, sorted and unique.
    std::vector<std::string> all_files(std::string_view path);

    // The absolute worktree root (`git rev-parse --show-toplevel`), or "".
    std::string toplevel(std::string_view path = ".");

    // The upstream tracking branch name (`git rev-parse --abbrev-ref @{u}`), or "".
    std::string upstream_branch(std::string_view path = ".");

    // True when `file` is tracked by the index (`git ls-files --error-unmatch`).
    bool is_tracked(std::string_view path, std::string_view file);

    // `git grep -E -l <patterns> -- <globs>`: true when any tracked file matches
    // one of the extended regular expressions.  Empty `globs` searches every file.
    bool grep(std::string_view path, text::string_views patterns, std::span<char const* const> globs = {});

} // namespace dana::git
