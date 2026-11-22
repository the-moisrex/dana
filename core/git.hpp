#pragma once

#include "strings.hpp"

#include <git2.h>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

namespace dana::git {

    // Run a git command and return stdout with trailing newlines stripped.
    std::string run(text::string_views args);

    std::string run(std::initializer_list<char const*> args);

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
        void operator()(git_repository* repo) const noexcept {
            if (repo != nullptr) {
                ::git_repository_free(repo);
            }
        }
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

} // namespace dana::git
