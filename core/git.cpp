#include "./git.hpp"

#include "glob.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

    std::string_view strip_quotes(std::string_view path) {
        if (path.size() >= 2 && path.front() == '"' && path.back() == '"') {
            path.remove_prefix(1);
            path.remove_suffix(1);
        }
        return path;
    }

    // Extract the destination path from a "diff --git a/<old> b/<new>" header.
    std::string path_from_diff_header(std::string_view line) {
        if (auto const pos = line.rfind(" b/"); pos != std::string_view::npos) {
            return std::string(strip_quotes(line.substr(pos + 3)));
        }

        // Quoted form:  diff --git "a/foo bar" "b/foo bar"
        if (auto const quoted = line.rfind("\"b/"); quoted != std::string_view::npos) {
            auto path = line.substr(quoted + 1);
            if (!path.empty() && path.back() == '"') {
                path.remove_suffix(1);
            }
            if (path.starts_with("b/")) {
                path.remove_prefix(2);
            }
            return std::string(path);
        }
        return {};
    }

} // namespace

// Annotate ordinary unified diffs using destination-file line numbers.
// Combined diffs (multiple prefix columns) are left unchanged.
std::string dana::git::add_line_numbers_to_diff(std::string_view diff) {
    std::istringstream stream{std::string(diff)};
    std::string        line;
    std::string        result;

    std::uint64_t new_line_num = 0;
    bool          in_hunk      = false;

    while (std::getline(stream, line)) {
        if (line.starts_with("diff ")) {
            in_hunk  = false;
            result  += line;
            result.push_back('\n');
            continue;
        }

        if (line.starts_with("@@")) {
            in_hunk = false;
            if (line.starts_with("@@ -")) {
                auto const pos = line.find(" +");
                if (pos != std::string::npos) {
                    try {
                        new_line_num = std::stoull(line.substr(pos + 2));
                        in_hunk      = true;
                    } catch (...) {
                        // Preserve malformed headers without annotation.
                        in_hunk = false;
                    }
                }
            }
            result += line;
            result.push_back('\n');
            continue;
        }

        if (!in_hunk) {
            result += line;
            result.push_back('\n');
            continue;
        }
        if (line.empty()) {
            result += std::format("{:5d}   \n", new_line_num++);
            continue;
        }

        char const prefix = line.front();
        if (prefix == '+') {
            result += std::format("{:5d} + {}", new_line_num++, line.substr(1));
        } else if (prefix == '-') {
            result += std::format("      - {}", line.substr(1));
        } else if (prefix == ' ') {
            result += std::format("{:5d}   {}", new_line_num++, line.substr(1));
        } else {
            result += line;
        }
        result.push_back('\n');
    }

    return result;
}

// Keep/drop whole-file blocks of a unified diff.
//   * `includes` non-empty => only blocks whose path matches one of them are kept.
//   * `excludes`          => blocks whose path matches any of them are dropped.
// Paths are matched both verbatim and by basename, using `detail::glob_match`.
std::string
dana::git::filter_diff_by_path(std::string_view diff, text::string_views includes, text::string_views excludes) {
    if (includes.empty() && excludes.empty()) {
        return std::string(diff);
    }

    auto matches = [](std::string_view path, text::string_views patterns) {
        if (path.empty()) {
            return false;
        }
        return std::ranges::any_of(patterns, [path](std::string_view pattern) {
            if (dana::glob::match(pattern, path)) {
                return true;
            }
            auto const slash = path.rfind('/');
            auto const base  = slash == std::string_view::npos ? path : path.substr(slash + 1);
            return dana::glob::match(pattern, base);
        });
    };

    std::istringstream input{std::string(diff)};
    std::string        line;
    std::string        leading;
    std::string        out;
    std::string        block;
    std::string        path;
    std::string        pending_old;
    bool               in_block = false;

    auto const flush = [&] {
        if (!in_block) {
            return;
        }
        bool keep = true;
        if (!includes.empty() && !matches(path, includes)) {
            keep = false;
        }
        if (matches(path, excludes)) {
            keep = false;
        }
        if (keep) {
            out += block;
        }
        block.clear();
    };

    while (std::getline(input, line)) {
        if (line.starts_with("diff ")) {
            flush();
            in_block = true;
            path     = path_from_diff_header(line);
            pending_old.clear();
            block += line;
            block.push_back('\n');
            continue;
        }
        if (!in_block) {
            leading += line;
            leading.push_back('\n');
            continue;
        }
        block += line;
        block.push_back('\n');

        std::string_view const line_view{line};
        if (line_view.starts_with("--- ")) {
            auto const old_path = strip_quotes(line_view.substr(4));
            if (old_path.starts_with("a/")) {
                pending_old = std::string(old_path.substr(2));
            }
        } else if (line_view.starts_with("+++ ")) {
            auto const new_path = strip_quotes(line_view.substr(4));
            if (new_path == "/dev/null") {
                path = pending_old;
            } else if (new_path.starts_with("b/")) {
                path = std::string(new_path.substr(2));
            }
        }
    }
    flush();

    return leading + out;
}

namespace dana::git {

    // -----------------------------------------------------------------------------
    // Runtime Management
    // -----------------------------------------------------------------------------

    runtime::runtime() {
        int const result = ::git_libgit2_init();
        if (result < 0) [[unlikely]] {
            auto const* err = ::git_error_last();
            char const* msg = (err != nullptr && err->message != nullptr) ? err->message : "unknown error";
            throw std::runtime_error(std::string("libgit2 initialization failed: ") + msg);
        }
    }

    runtime::~runtime() {
        ::git_libgit2_shutdown();
    }

    void runtime::ensure_initialized() {
        static runtime const instance;
        (void) instance;
    }

    // -----------------------------------------------------------------------------
    // Error Handling Helpers
    // -----------------------------------------------------------------------------

    std::string_view last_error_message() noexcept {
        auto const* err = ::git_error_last();
        if (err != nullptr && err->message != nullptr) [[likely]] {
            return err->message;
        }
        return "unknown libgit2 error";
    }

    namespace {

        [[noreturn]] void throw_git_error(std::string_view operation) {
            auto const message = last_error_message();
            throw std::runtime_error(std::string(operation) + ": " + std::string(message));
        }

    } // namespace

    // -----------------------------------------------------------------------------
    // Repository Utilities
    // -----------------------------------------------------------------------------

    repository_ptr open_repository(std::string_view path, unsigned int flags, char const* ceiling_dirs) {
        runtime::ensure_initialized();

        std::string const null_terminated_path(path);

        git_repository* raw_repo = nullptr;
        int const result = ::git_repository_open_ext(&raw_repo, null_terminated_path.c_str(), flags, ceiling_dirs);

        repository_ptr repo(raw_repo);

        if (result < 0) [[unlikely]] {
            throw_git_error("git_repository_open_ext failed");
        }

        return repo;
    }

    repository_ptr try_open_repository(std::string_view path, unsigned int flags, char const* ceiling_dirs) noexcept {
        try {
            runtime::ensure_initialized();

            std::string const null_terminated_path(path);

            git_repository* raw_repo = nullptr;
            int const result = ::git_repository_open_ext(&raw_repo, null_terminated_path.c_str(), flags, ceiling_dirs);

            if (result < 0) [[unlikely]] {
                if (raw_repo != nullptr) {
                    ::git_repository_free(raw_repo);
                }
                return nullptr;
            }

            return repository_ptr(raw_repo);
        } catch (...) {
            return nullptr;
        }
    }

    bool is_inside_worktree(std::string_view const path) noexcept try {
        runtime::ensure_initialized();

        std::string const null_terminated_path(path);

        git_repository* raw_repo = nullptr;
        int const       result =
          ::git_repository_open_ext(&raw_repo, null_terminated_path.c_str(), GIT_REPOSITORY_OPEN_CROSS_FS, nullptr);

        repository_ptr const repo(raw_repo);
        return result == 0 && repo != nullptr && (::git_repository_is_bare(repo.get()) == 0);
    } catch (...) {
        return false;
    }

    // -----------------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------------

    namespace {

        // Owns the char* array a git_strarray needs; the strings live in `entries`.
        class strarray_storage final {
          public:
            explicit strarray_storage(std::vector<std::string> const& entries) : m_entries(entries) {
                m_pointers.reserve(m_entries.size());
                for (auto const& entry : m_entries) {
                    m_pointers.push_back(entry.c_str());
                }
                m_value.strings =
                  m_pointers.empty()
                    ? nullptr
                    : const_cast<char**>(m_pointers.data()); // NOLINT(cppcoreguidelines-pro-type-const-cast)
                m_value.count = m_pointers.size();
            }

            [[nodiscard]] git_strarray const* get() const noexcept {
                return m_pointers.empty() ? nullptr : &m_value;
            }

          private:
            std::vector<std::string> m_entries;
            std::vector<char const*> m_pointers;
            git_strarray             m_value{};
        };

        std::string normalized_workdir(git_repository const* repo) {
            char const* workdir = ::git_repository_workdir(repo);
            if (workdir == nullptr) {
                return {};
            }
            std::string out(workdir);
            while (out.size() > 1 && out.back() == '/') {
                out.pop_back();
            }
            return out;
        }

        // Convert a cwd-relative or absolute path to a repository-relative
        // pathspec entry (libgit2 always interprets pathspecs from the root).
        std::string to_repo_relative(std::string_view workdir, std::string_view entry) {
            if (workdir.empty() || entry.empty()) {
                return std::string(entry);
            }
            std::error_code       error;
            std::filesystem::path path{std::string(entry)};
            if (!path.is_absolute()) {
                path = std::filesystem::current_path(error) / path;
                if (error) {
                    return std::string(entry);
                }
            }
            auto const relative = path.lexically_normal().lexically_relative(std::filesystem::path(workdir));
            if (relative.empty()) {
                return std::string(entry);
            }
            auto text = relative.generic_string();
            if (text == ".") {
                return {};
            }
            if (text.starts_with("..")) {
                return std::string(entry);
            }
            return text;
        }

        std::vector<std::string> normalize_pathspec(git_repository const*           repo,
                                                    std::vector<std::string> const& pathspec) {
            if (pathspec.empty()) {
                return {};
            }
            auto const               workdir = normalized_workdir(repo);
            std::vector<std::string> out;
            out.reserve(pathspec.size());
            for (auto const& entry : pathspec) {
                auto relative = to_repo_relative(workdir, entry);
                if (!relative.empty()) {
                    out.push_back(std::move(relative));
                }
            }
            return out;
        }

        bool matches_pathspec(std::string_view path, std::vector<std::string> const& pathspec) {
            if (pathspec.empty()) {
                return true;
            }
            for (auto const& spec : pathspec) {
                if (spec.empty() || spec == ".") {
                    return true;
                }
                if (dana::glob::has_wildcard(spec)) {
                    if (dana::glob::match(spec, path)) {
                        return true;
                    }
                    auto const slash = path.rfind('/');
                    auto const base  = slash == std::string_view::npos ? path : path.substr(slash + 1);
                    if (dana::glob::match(spec, base)) {
                        return true;
                    }
                    continue;
                }
                std::string_view prefix{spec};
                while (!prefix.empty() && prefix.back() == '/') {
                    prefix.remove_suffix(1);
                }
                if (path == prefix) {
                    return true;
                }
                if (path.size() > prefix.size() && path.starts_with(prefix) &&
                    path[prefix.size()] == '/') // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                {
                    return true;
                }
            }
            return false;
        }

        // The repo-relative directory prefix of `path` (empty at the root).
        std::string path_prefix(git_repository const* repo, std::string_view path) {
            auto const workdir = normalized_workdir(repo);
            if (workdir.empty()) {
                return {};
            }
            auto prefix = to_repo_relative(workdir, path);
            while (!prefix.empty() && prefix.back() == '/') {
                prefix.pop_back();
            }
            return prefix;
        }

        // Strip `prefix/` from a repo-relative path; nullopt when it is outside.
        std::optional<std::string> under_prefix(std::string_view path, std::string_view prefix) {
            if (prefix.empty()) {
                return std::string(path);
            }
            if (path == prefix) {
                return std::string{};
            }
            if (path.size() > prefix.size() && path.starts_with(prefix) &&
                path[prefix.size()] == '/') // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            {
                return std::string(path.substr(prefix.size() + 1));
            }
            return std::nullopt;
        }

        int append_patch_line(
          git_diff_delta const* /*delta*/,
          git_diff_hunk const* /*hunk*/,
          git_diff_line const* line,
          void*                payload) {
            auto* out = static_cast<std::string*>(payload);
            // The line content excludes the origin for context/add/delete lines;
            // git's patch format (and every diff parser) needs it.
            if (line->origin == GIT_DIFF_LINE_CONTEXT || line->origin == GIT_DIFF_LINE_ADDITION ||
                line->origin == GIT_DIFF_LINE_DELETION)
            {
                out->push_back(line->origin);
            }
            out->append(line->content, static_cast<std::size_t>(line->content_len));
            return 0;
        }

        git_tree* head_tree(git_repository* repo) {
            git_object* object = nullptr;
            if (::git_revparse_single(&object, repo, "HEAD") < 0) {
                return nullptr;
            }
            git_tree*   tree = nullptr;
            auto const* commit =
              reinterpret_cast<git_commit*>(object); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
            if (::git_object_type(object) == GIT_OBJECT_COMMIT && ::git_commit_tree(&tree, commit) < 0) {
                tree = nullptr;
            }
            ::git_object_free(object);
            return tree;
        }

        git_tree* tree_for(git_repository* repo, std::string const& revision) {
            git_object* object = nullptr;
            if (::git_revparse_single(&object, repo, revision.c_str()) < 0) {
                throw_git_error("could not resolve revision '" + revision + "'");
            }
            git_object* peeled = nullptr;
            if (::git_object_peel(&peeled, object, GIT_OBJECT_TREE) < 0) {
                ::git_object_free(object);
                throw_git_error("revision '" + revision + "' is not a tree");
            }
            ::git_object_free(object);
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            return reinterpret_cast<git_tree*>(peeled);
        }

        git_diff_options base_diff_options(strarray_storage const& paths) {
            git_diff_options options;
            ::git_diff_options_init(&options, GIT_DIFF_OPTIONS_VERSION);
            if (auto const* value = paths.get(); value != nullptr) {
                options.pathspec = *value;
            }
            return options;
        }

        git_diff* create_diff(git_repository* repo, diff_request const& request, git_diff_options const& options) {
            git_diff* result = nullptr;

            switch (request.target) {
                case diff_target::workdir: {
                    git_index* index = nullptr;
                    if (::git_repository_index(&index, repo) < 0) {
                        throw_git_error("could not open the index");
                    }
                    int const result_code = ::git_diff_index_to_workdir(&result, repo, index, &options);
                    ::git_index_free(index);
                    if (result_code < 0) {
                        throw_git_error("could not diff the working tree");
                    }
                    break;
                }
                case diff_target::cached: {
                    git_tree*  tree  = head_tree(repo);
                    git_index* index = nullptr;
                    if (::git_repository_index(&index, repo) < 0) {
                        if (tree != nullptr) {
                            ::git_tree_free(tree);
                        }
                        throw_git_error("could not open the index");
                    }
                    int const result_code = ::git_diff_tree_to_index(&result, repo, tree, index, &options);
                    ::git_index_free(index);
                    if (tree != nullptr) {
                        ::git_tree_free(tree);
                    }
                    if (result_code < 0) {
                        throw_git_error("could not diff the index");
                    }
                    break;
                }
                case diff_target::revision: {
                    if (!request.exclude_revision.empty()) {
                        git_tree* base        = tree_for(repo, request.exclude_revision);
                        git_tree* target      = tree_for(repo, request.revision);
                        int const result_code = ::git_diff_tree_to_tree(&result, repo, base, target, &options);
                        ::git_tree_free(base);
                        ::git_tree_free(target);
                        if (result_code < 0) {
                            throw_git_error("could not diff the revisions");
                        }
                    } else {
                        git_tree* tree        = tree_for(repo, request.revision);
                        int const result_code = ::git_diff_tree_to_workdir_with_index(&result, repo, tree, &options);
                        ::git_tree_free(tree);
                        if (result_code < 0) {
                            throw_git_error("could not diff the revision against the working tree");
                        }
                    }
                    break;
                }
            }

            return result;
        }

        bool has(git_status_t status, git_status_t flag) {
            return (static_cast<unsigned int>(status) & static_cast<unsigned int>(flag)) != 0U;
        }

        char index_status_code(git_status_t status) {
            if (has(status, GIT_STATUS_INDEX_NEW)) {
                return 'A';
            }
            if (has(status, GIT_STATUS_INDEX_MODIFIED)) {
                return 'M';
            }
            if (has(status, GIT_STATUS_INDEX_DELETED)) {
                return 'D';
            }
            if (has(status, GIT_STATUS_INDEX_RENAMED)) {
                return 'R';
            }
            if (has(status, GIT_STATUS_INDEX_TYPECHANGE)) {
                return 'T';
            }
            return ' ';
        }

        char workdir_status_code(git_status_t status) {
            if (has(status, GIT_STATUS_WT_NEW)) {
                return '?';
            }
            if (has(status, GIT_STATUS_WT_MODIFIED)) {
                return 'M';
            }
            if (has(status, GIT_STATUS_WT_DELETED)) {
                return 'D';
            }
            if (has(status, GIT_STATUS_WT_RENAMED)) {
                return 'R';
            }
            if (has(status, GIT_STATUS_WT_TYPECHANGE)) {
                return 'T';
            }
            return ' ';
        }

        char delta_status_code(git_delta_t status) {
            switch (status) {
                case GIT_DELTA_ADDED: return 'A';
                case GIT_DELTA_DELETED: return 'D';
                case GIT_DELTA_MODIFIED: return 'M';
                case GIT_DELTA_RENAMED: return 'R';
                case GIT_DELTA_COPIED: return 'C';
                case GIT_DELTA_TYPECHANGE: return 'T';
                case GIT_DELTA_UNTRACKED: return '?';
                default: return ' ';
            }
        }

        std::string render_delta_list(git_diff const* diff, bool name_status) {
            std::string       out;
            std::size_t const count = ::git_diff_num_deltas(diff);
            for (std::size_t i = 0; i < count; ++i) {
                auto const* delta = ::git_diff_get_delta(diff, i);
                if (delta == nullptr) {
                    continue;
                }
                if (delta->status == GIT_DELTA_UNMODIFIED || delta->status == GIT_DELTA_IGNORED) {
                    continue;
                }
                bool const renamed = delta->status == GIT_DELTA_RENAMED || delta->status == GIT_DELTA_COPIED;
                if (name_status) {
                    out.push_back(delta_status_code(delta->status));
                    if (renamed && delta->similarity > 0) {
                        out += std::to_string(delta->similarity);
                    }
                    out.push_back('\t');
                    if (renamed && delta->old_file.path != nullptr && delta->new_file.path != nullptr) {
                        out += delta->old_file.path;
                        out.push_back('\t');
                        out += delta->new_file.path;
                    } else if (delta->status == GIT_DELTA_DELETED) {
                        out += delta->old_file.path != nullptr ? delta->old_file.path : "";
                    } else {
                        out += delta->new_file.path != nullptr ? delta->new_file.path : "";
                    }
                } else if (delta->status == GIT_DELTA_DELETED) {
                    out += delta->old_file.path != nullptr ? delta->old_file.path : "";
                } else {
                    out += delta->new_file.path != nullptr ? delta->new_file.path : "";
                }
                out.push_back('\n');
            }
            return out;
        }

        std::vector<std::string>
        collect_status_paths(git_repository* repo, git_status_t wanted, std::vector<std::string> const& pathspec) {
            git_status_options options;
            ::git_status_options_init(&options, GIT_STATUS_OPTIONS_VERSION);
            options.show  = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
            options.flags = static_cast<unsigned int>(GIT_STATUS_OPT_INCLUDE_UNTRACKED) |
                            static_cast<unsigned int>(GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS) |
                            static_cast<unsigned int>(GIT_STATUS_OPT_SORT_CASE_SENSITIVELY);

            git_status_list* list = nullptr;
            if (::git_status_list_new(&list, repo, &options) < 0) {
                throw_git_error("could not read the repository status");
            }

            std::vector<std::string> paths;
            std::size_t const        count = ::git_status_list_entrycount(list);
            for (std::size_t i = 0; i < count; ++i) {
                auto const* entry = ::git_status_byindex(list, i);
                if (entry == nullptr ||
                    (static_cast<unsigned int>(entry->status) & static_cast<unsigned int>(wanted)) == 0U)
                {
                    continue;
                }
                std::string path;
                if (entry->index_to_workdir != nullptr && entry->index_to_workdir->new_file.path != nullptr) {
                    path = entry->index_to_workdir->new_file.path;
                } else if (entry->head_to_index != nullptr && entry->head_to_index->new_file.path != nullptr) {
                    path = entry->head_to_index->new_file.path;
                }
                if (!path.empty() && matches_pathspec(path, pathspec)) {
                    paths.push_back(std::move(path));
                }
            }
            ::git_status_list_free(list);

            std::ranges::sort(paths);
            paths.erase(std::ranges::unique(paths).begin(), paths.end());
            return paths;
        }

    } // namespace

    // -----------------------------------------------------------------------------
    // Repository queries
    // -----------------------------------------------------------------------------

    std::string current_branch(std::string_view path) {
        auto const repo = open_repository(path);

        git_reference* head = nullptr;
        if (::git_repository_head(&head, repo.get()) < 0) {
            // An unborn branch has no commit yet; HEAD still names it.
            if (::git_reference_lookup(&head, repo.get(), "HEAD") < 0) {
                return {};
            }
            char const* target = ::git_reference_symbolic_target(head);
            std::string name   = target != nullptr ? target : "";
            ::git_reference_free(head);
            constexpr std::string_view prefix = "refs/heads/";
            if (name.starts_with(prefix)) {
                name.erase(0, prefix.size());
            }
            return name;
        }

        std::string name;
        if (::git_reference_is_branch(head) != 0) {
            char const* branch = nullptr;
            if (::git_branch_name(&branch, head) == 0 && branch != nullptr) {
                name = branch;
            }
        }
        ::git_reference_free(head);
        return name;
    }

    std::string status_short(std::string_view path) {
        auto const repo = open_repository(path);

        git_status_options options;
        ::git_status_options_init(&options, GIT_STATUS_OPTIONS_VERSION);
        options.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
        options.flags =
          GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS | // NOLINT(bugprone-signed-bitwise)
          GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX | GIT_STATUS_OPT_RENAMES_INDEX_TO_WORKDIR |
          GIT_STATUS_OPT_SORT_CASE_SENSITIVELY;

        git_status_list* list = nullptr;
        if (::git_status_list_new(&list, repo.get(), &options) < 0) {
            throw_git_error("could not read the repository status");
        }

        std::string       out;
        std::string       untracked;
        std::size_t const count = ::git_status_list_entrycount(list);
        for (std::size_t i = 0; i < count; ++i) {
            auto const* entry = ::git_status_byindex(list, i);
            if (entry == nullptr) {
                continue;
            }
            if (has(entry->status, GIT_STATUS_IGNORED)) {
                continue;
            }

            if (has(entry->status, GIT_STATUS_CONFLICTED)) {
                std::string conflict_path;
                if (entry->index_to_workdir != nullptr && entry->index_to_workdir->new_file.path != nullptr) {
                    conflict_path = entry->index_to_workdir->new_file.path;
                } else if (entry->head_to_index != nullptr && entry->head_to_index->new_file.path != nullptr) {
                    conflict_path = entry->head_to_index->new_file.path;
                }
                out += "UU ";
                out += conflict_path;
                out.push_back('\n');
                continue;
            }

            if (has(entry->status, GIT_STATUS_WT_NEW) && !has(entry->status, GIT_STATUS_INDEX_NEW)) {
                std::string untracked_path;
                if (entry->index_to_workdir != nullptr && entry->index_to_workdir->new_file.path != nullptr) {
                    untracked_path = entry->index_to_workdir->new_file.path;
                }
                untracked += "?? ";
                untracked += untracked_path;
                untracked.push_back('\n');
                continue;
            }

            if (has(entry->status, GIT_STATUS_INDEX_RENAMED) && entry->head_to_index != nullptr) {
                auto const* delta  = entry->head_to_index;
                out               += "R  ";
                out               += delta->old_file.path != nullptr ? delta->old_file.path : "";
                out               += " -> ";
                out               += delta->new_file.path != nullptr ? delta->new_file.path : "";
                out.push_back('\n');
                continue;
            }

            if (has(entry->status, GIT_STATUS_WT_RENAMED) && entry->index_to_workdir != nullptr) {
                auto const* delta  = entry->index_to_workdir;
                out               += " R ";
                out               += delta->old_file.path != nullptr ? delta->old_file.path : "";
                out               += " -> ";
                out               += delta->new_file.path != nullptr ? delta->new_file.path : "";
                out.push_back('\n');
                continue;
            }

            std::string changed_path;
            if (entry->index_to_workdir != nullptr && entry->index_to_workdir->new_file.path != nullptr) {
                changed_path = entry->index_to_workdir->new_file.path;
            } else if (entry->head_to_index != nullptr && entry->head_to_index->new_file.path != nullptr) {
                changed_path = entry->head_to_index->new_file.path;
            }

            out.push_back(index_status_code(entry->status));
            out.push_back(workdir_status_code(entry->status));
            out.push_back(' ');
            out += changed_path;
            out.push_back('\n');
        }
        ::git_status_list_free(list);
        return out + untracked;
    }

    std::string log_oneline(std::size_t count, std::string_view path) {
        auto const repo = open_repository(path);

        git_revwalk* walk = nullptr;
        if (::git_revwalk_new(&walk, repo.get()) < 0) {
            throw_git_error("could not create a revision walker");
        }
        ::git_revwalk_sorting(walk, GIT_SORT_TIME);
        if (::git_revwalk_push_head(walk) < 0) {
            ::git_revwalk_free(walk);
            return {};
        }

        std::string out;
        git_oid     oid;
        std::size_t emitted = 0;
        while (emitted < count && ::git_revwalk_next(&oid, walk) == 0) {
            git_commit* commit = nullptr;
            if (::git_commit_lookup(&commit, repo.get(), &oid) < 0) {
                continue;
            }

            git_buf short_id = GIT_BUF_INIT;
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            if (::git_object_short_id(&short_id, reinterpret_cast<git_object*>(commit)) == 0 && short_id.ptr != nullptr)
            {
                out.append(short_id.ptr, short_id.size);
            } else {
                std::array<char, GIT_OID_HEXSZ + 1> buffer{};
                ::git_oid_tostr(buffer.data(), buffer.size(), &oid);
                out += buffer.data();
            }
            ::git_buf_dispose(&short_id);

            if (char const* summary = ::git_commit_summary(commit); summary != nullptr) {
                out.push_back(' ');
                out += summary;
            }
            out.push_back('\n');

            ::git_commit_free(commit);
            ++emitted;
        }

        ::git_revwalk_free(walk);
        return out;
    }

    std::string diff(std::string_view path, diff_request const& request) {
        auto const repo = open_repository(path);

        auto const             pathspec = normalize_pathspec(repo.get(), request.pathspec);
        strarray_storage const paths(pathspec);
        git_diff_options       options = base_diff_options(paths);

        if (request.include_untracked) {
            options.flags |=
              GIT_DIFF_INCLUDE_UNTRACKED | GIT_DIFF_RECURSE_UNTRACKED_DIRS; // NOLINT(bugprone-signed-bitwise)
        }
        if (request.show_untracked_content) {
            options.flags |= GIT_DIFF_SHOW_UNTRACKED_CONTENT;               // NOLINT(bugprone-signed-bitwise)
        }

        git_diff* result = create_diff(repo.get(), request, options);
        if (result == nullptr) {
            return {};
        }

        // Match git's default diff.renames=true: detect renames before rendering.
        git_diff_find_options find_options;
        ::git_diff_find_options_init(&find_options, GIT_DIFF_FIND_OPTIONS_VERSION);
        find_options.flags = GIT_DIFF_FIND_RENAMES;
        (void) ::git_diff_find_similar(result, &find_options);

        std::string out;
        if (request.name_only || request.name_status) {
            out = render_delta_list(result, request.name_status);
        } else {
            if (::git_diff_print(result, GIT_DIFF_FORMAT_PATCH, &append_patch_line, &out) < 0) {
                ::git_diff_free(result);
                throw_git_error("could not render the diff");
            }
        }

        ::git_diff_free(result);
        return out;
    }

    std::vector<std::string> tracked_files(std::string_view path, std::vector<std::string> const& pathspec) {
        auto const repo = open_repository(path);

        git_index* index = nullptr;
        if (::git_repository_index(&index, repo.get()) < 0) {
            throw_git_error("could not open the index");
        }

        auto const               normalized = normalize_pathspec(repo.get(), pathspec);
        auto const               prefix     = path_prefix(repo.get(), path);
        std::vector<std::string> files;
        std::size_t const        count = ::git_index_entrycount(index);
        files.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            auto const* entry = ::git_index_get_byindex(index, i);
            if (entry == nullptr || entry->path == nullptr) {
                continue;
            }
            std::string_view const file{entry->path};
            if (!matches_pathspec(file, normalized)) {
                continue;
            }
            auto relative = under_prefix(file, prefix);
            if (relative && !relative->empty()) {
                files.push_back(std::move(*relative));
            }
        }
        ::git_index_free(index);

        std::ranges::sort(files);
        files.erase(std::ranges::unique(files).begin(), files.end());
        return files;
    }

    std::vector<std::string> untracked_files(std::string_view path, std::vector<std::string> const& pathspec) {
        auto const repo       = open_repository(path);
        auto const normalized = normalize_pathspec(repo.get(), pathspec);
        auto const prefix     = path_prefix(repo.get(), path);

        std::vector<std::string> files;
        for (auto const& file : collect_status_paths(repo.get(), GIT_STATUS_WT_NEW, normalized)) {
            auto relative = under_prefix(file, prefix);
            if (relative && !relative->empty()) {
                files.push_back(std::move(*relative));
            }
        }
        return files;
    }

    std::vector<std::string> all_files(std::string_view path) {
        auto files     = tracked_files(path);
        auto untracked = untracked_files(path);
        files.insert(files.end(), untracked.begin(), untracked.end());
        std::ranges::sort(files);
        files.erase(std::ranges::unique(files).begin(), files.end());
        return files;
    }

    std::string toplevel(std::string_view path) {
        auto const repo = open_repository(path);
        return normalized_workdir(repo.get());
    }

    std::string upstream_branch(std::string_view path) {
        auto const repo = open_repository(path);

        git_reference* head = nullptr;
        if (::git_repository_head(&head, repo.get()) < 0) {
            return {};
        }
        if (::git_reference_is_branch(head) == 0) {
            ::git_reference_free(head);
            return {};
        }

        git_reference* upstream = nullptr;
        if (::git_branch_upstream(&upstream, head) < 0) {
            ::git_reference_free(head);
            return {};
        }

        std::string name;
        char const* branch = nullptr;
        if (::git_branch_name(&branch, upstream) == 0 && branch != nullptr) {
            name = branch;
        }
        ::git_reference_free(upstream);
        ::git_reference_free(head);
        return name;
    }

    bool is_tracked(std::string_view path, std::string_view file) {
        auto const repo = open_repository(path);

        auto const workdir = normalized_workdir(repo.get());
        if (workdir.empty()) {
            return false;
        }

        std::error_code       error;
        std::filesystem::path candidate{std::string(file)};
        if (!candidate.is_absolute()) {
            candidate = std::filesystem::current_path(error) / candidate;
            if (error) {
                return false;
            }
        }
        auto const relative = candidate.lexically_normal().lexically_relative(std::filesystem::path(workdir));
        if (relative.empty()) {
            return false;
        }
        auto const text = relative.generic_string();
        if (text.starts_with("..")) {
            return false;
        }

        git_index* index = nullptr;
        if (::git_repository_index(&index, repo.get()) < 0) {
            return false;
        }

        bool found = false;
        for (int stage = GIT_INDEX_STAGE_NORMAL; stage <= GIT_INDEX_STAGE_THEIRS; ++stage) {
            if (::git_index_get_bypath(index, text.c_str(), stage) != nullptr) {
                found = true;
                break;
            }
        }
        ::git_index_free(index);
        return found;
    }

    bool grep(std::string_view path, text::string_views patterns, std::span<char const* const> globs) {
        if (patterns.empty()) {
            return false;
        }

        std::vector<std::regex> expressions;
        expressions.reserve(patterns.size());
        for (auto const& pattern : patterns) {
            try {
                expressions.emplace_back(std::string(pattern), std::regex::extended);
            } catch (std::regex_error const&) {
                return false;
            }
        }

        std::error_code       error;
        std::filesystem::path base{std::string(path)};
        if (!base.is_absolute()) {
            base = std::filesystem::current_path(error) / base;
            if (error) {
                return false;
            }
        }

        auto const files = tracked_files(path);
        for (auto const& file : files) {
            if (!globs.empty()) {
                bool matched = false;
                for (auto const* glob : globs) {
                    if (glob == nullptr) {
                        continue;
                    }
                    auto const slash = file.rfind('/');
                    auto const base =
                      slash == std::string::npos ? std::string_view{file} : std::string_view{file}.substr(slash + 1);
                    if (dana::glob::match(glob, file) || dana::glob::match(glob, base)) {
                        matched = true;
                        break;
                    }
                }
                if (!matched) {
                    continue;
                }
            }

            std::ifstream input(base / file, std::ios::binary);
            if (!input) {
                continue;
            }
            std::string const content{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            if (content.contains('\0')) {
                continue; // binary
            }
            for (auto const& expression : expressions) {
                if (std::regex_search(content, expression)) {
                    return true;
                }
            }
        }
        return false;
    }

} // namespace dana::git
