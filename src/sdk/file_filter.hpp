#pragma once

#include "facts.hpp"
#include "stage.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dana {

    // The chain-wide file selection filter, built from the facts `.files.exclude`
    // and `.files.scope` publish:
    //
    //   * `exclude_` — patterns matched against a file's absolute path, its path
    //     relative to the git root, its path relative to the scope and its
    //     basename. A pattern with no wildcard that names a directory excludes its
    //     whole subtree.
    //   * `root_` — an absolute directory that becomes the default search path, the
    //     picker's candidate root, and the first place a relative FILE argument is
    //     resolved.
    //
    // The filter is read-only: a prompt builds one from `v.prior` and publishes
    // whatever it wants to change.
    struct [[nodiscard]] file_filter {
        static file_filter from(facts const& prior, environment const& env);

        [[nodiscard]] bool empty() const {
            return exclude_.empty() && root_.empty();
        }

        [[nodiscard]] bool has_root() const {
            return !root_.empty();
        }

        [[nodiscard]] std::filesystem::path const& root() const {
            return root_;
        }

        [[nodiscard]] std::vector<std::string> const& exclude() const {
            return exclude_;
        }

        // True when `path` matches one of the exclusion patterns.
        [[nodiscard]] bool excluded(std::filesystem::path const& path) const;

        // Resolve a FILE argument: against the scope first (when set), then with
        // the normal resolver.
        [[nodiscard]] std::optional<std::filesystem::path> resolve(environment const& env, std::string_view arg) const;

        // The files a picker should offer: git's listing under the scope, or a
        // native traversal, minus the excluded ones. Absolute, sorted, deduplicated.
        [[nodiscard]] std::vector<std::filesystem::path> candidates() const;

        // The directories under the scope (plus the scope itself), for the scope
        // picker. Absolute, sorted, deduplicated.
        [[nodiscard]] std::vector<std::filesystem::path> directories() const;

        // ripgrep `-g '!pattern'` arguments and the GNU grep fallback flags. A path
        // pattern grep cannot express is skipped there; the prompts that can filter
        // in-process do so as well.
        [[nodiscard]] std::vector<std::string> rg_globs() const;
        [[nodiscard]] std::vector<std::string> grep_excludes() const;

        // The `keys::excluded_files` value: the prior patterns plus `patterns`,
        // newline-separated and deduplicated.
        [[nodiscard]] std::string exclude_fact(std::vector<std::string> const& patterns) const;

      private:
        std::vector<std::string> exclude_;
        std::filesystem::path    root_;
        std::filesystem::path    git_root_;
    };

    // `fzf -m` over the filter's candidates. An empty filter keeps the historical
    // behaviour (a bare `fzf -m`, so `$FZF_DEFAULT_COMMAND` still applies).
    std::vector<std::filesystem::path> select_files(environment const& env, file_filter const& filter);

    // `fzf -m` over the filter's directories (the scope picker).
    std::vector<std::filesystem::path> select_directories(environment const& env, file_filter const& filter);

    // The path a picker shows for `path`: relative to the git root (or the cwd
    // outside a repository) when that stays inside it, absolute otherwise.
    std::string display_path(environment const& env, std::filesystem::path const& path);

} // namespace dana
