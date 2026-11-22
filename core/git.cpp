#include "./git.hpp"

#include "glob.hpp"
#include "process.hpp"

#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace detail {

    inline std::string_view strip_quotes(std::string_view path) {
        if (path.size() >= 2 && path.front() == '"' && path.back() == '"') {
            path.remove_prefix(1);
            path.remove_suffix(1);
        }
        return path;
    }

    // Extract the destination path from a "diff --git a/<old> b/<new>" header.
    inline std::string path_from_diff_header(std::string_view line) {
        if (auto pos = line.rfind(" b/"); pos != std::string_view::npos) {
            return std::string(strip_quotes(line.substr(pos + 3)));
        }

        // Quoted form:  diff --git "a/foo bar" "b/foo bar"
        if (auto q = line.rfind("\"b/"); q != std::string_view::npos) {
            auto p = line.substr(q + 1);
            if (!p.empty() && p.back() == '"') {
                p.remove_suffix(1);
            }
            if (p.starts_with("b/")) {
                p.remove_prefix(2);
            }
            return std::string(p);
        }
        return {};
    }

} // namespace detail

// Run a git command and return stdout with trailing newlines stripped.
std::string dana::git::run(text::string_views args) {
    auto        result = process::run_command(args);
    std::string out    = std::move(result.stdout_data);
    while (!out.empty() && out.back() == '\n') {
        out.pop_back();
    }
    return out;
}

std::string dana::git::run(std::initializer_list<char const*> args) {
    std::vector<std::string> v;
    v.reserve(args.size());
    for (auto const* a : args) {
        v.emplace_back(a);
    }
    return run(v);
}

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
                auto pos = line.find(" +");
                if (pos != std::string::npos) {
                    try {
                        new_line_num = std::stoull(line.substr(pos + 2));
                        in_hunk      = true;
                    } catch (...) {
                        // Preserve malformed headers without annotation.
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

        char prefix = line[0];
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
        for (auto const& pat : patterns) {
            if (dana::glob::match(pat, path)) {
                return true;
            }
            auto slash = path.rfind('/');
            auto base  = slash == std::string_view::npos ? path : path.substr(slash + 1);
            if (dana::glob::match(pat, base)) {
                return true;
            }
        }
        return false;
    };

    std::istringstream in{std::string(diff)};
    std::string        line;
    std::string        leading;
    std::string        out;
    std::string        block;
    std::string        path;
    std::string        pending_old;
    bool               in_block = false;

    auto flush = [&] {
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

    while (std::getline(in, line)) {
        if (line.starts_with("diff ")) {
            flush();
            in_block = true;
            path     = detail::path_from_diff_header(line);
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

        std::string_view lv{line};
        if (lv.starts_with("--- ")) {
            auto p = detail::strip_quotes(lv.substr(4));
            if (p.starts_with("a/")) {
                pending_old = std::string(p.substr(2));
            }
        } else if (lv.starts_with("+++ ")) {
            auto p = detail::strip_quotes(lv.substr(4));
            if (p == "/dev/null") {
                path = pending_old;
            } else if (p.starts_with("b/")) {
                path = std::string(p.substr(2));
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
        static runtime instance;
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

    [[noreturn]] static void throw_git_error(std::string_view operation) {
        auto msg = last_error_message();
        throw std::runtime_error(std::string(operation) + ": " + std::string(msg));
    }

    // -----------------------------------------------------------------------------
    // Repository Utilities
    // -----------------------------------------------------------------------------

    repository_ptr open_repository(std::string_view path, unsigned int flags, char const* ceiling_dirs) {
        runtime::ensure_initialized();

        // Small stack-allocated null-terminated buffer helper if path lacks null termination,
        // or direct conversion assuming null-terminated inputs (std::string_view needs care if not null-terminated).
        // For standard string views, we use a temporary std::string if needed, or pass c_str() if null-terminated.
        // In C++26, standard string literals/views are commonly convertible. To be 100% safe against
        // non-null-terminated views:
        std::string null_terminated_path(path);

        git_repository* raw_repo = nullptr;
        int const result = ::git_repository_open_ext(&raw_repo, null_terminated_path.c_str(), flags, ceiling_dirs);

        repository_ptr repo(raw_repo);

        if (result < 0) [[unlikely]] {
            throw_git_error("git_repository_open_ext failed");
        }

        return repo;
    }

    repository_ptr try_open_repository(std::string_view path, unsigned int flags, char const* ceiling_dirs) noexcept {
        runtime::ensure_initialized();

        // To prevent any allocations on `try_open_repository` when called with string literals or c-strings,
        // we use a small thread-local or direct c_str check if it's guaranteed null-terminated,
        // or fallback safely. Assuming standard usage with null-terminated storage:
        // If path comes from a std::string or literal, path.data() is null-terminated if it came from
        // std::string::c_str() or a string literal. If not, we fall back to a safe stack/heap check. Here we use a safe
        // fallback:

        // Fast path: check if path.data()[path.size()] == '\0' (true for string literals and std::string)
        // Otherwise construct temporary string.
        char const* c_path = nullptr;
        std::string temp_storage;
        if (!path.empty() && path.data()[path.size()] == '\0') {
            c_path = path.data();
        } else {
            temp_storage = path;
            c_path       = temp_storage.c_str();
        }

        git_repository* raw_repo = nullptr;
        int const       result   = ::git_repository_open_ext(&raw_repo, c_path, flags, ceiling_dirs);

        if (result == GIT_ENOTFOUND) [[likely]] {
            return nullptr;
        }

        if (result < 0) [[unlikely]] {
            // Since this is `noexcept`, if libgit2 errors out unexpectedly during a `try_` query,
            // we terminate or handle it gracefully. Usually, unexpected codes call std::terminate
            // or return empty depending on policy. Let's return empty for all non-success/non-notfound codes
            // to keep `try_` strictly noexcept, or handle it:
            if (raw_repo != nullptr) {
                ::git_repository_free(raw_repo);
            }
            return nullptr;
        }

        return repository_ptr(raw_repo);
    }

    bool is_inside_worktree(std::string_view const path) noexcept try {
        runtime::ensure_initialized();

        char const* c_path = nullptr;
        std::string temp_storage;
        if (!path.empty() && path.data()[path.size()] == '\0') {
            c_path = path.data();
        } else {
            temp_storage = path;
            c_path       = temp_storage.c_str();
        }

        // Pass `nullptr` as the out-parameter to leverage libgit2's fast path discovery
        // without allocating or parsing a full `git_repository` structure when only checking presence.
        int const result = ::git_repository_open_ext(nullptr, c_path, GIT_REPOSITORY_OPEN_CROSS_FS, nullptr);

        if (result == 0) [[likely]] {
            // Repository found. (Note: git_repository_open_ext returns 0 if a valid repository is found,
            // even if it's a bare repository. If you want to strictly require a *worktree* and reject bare repos,
            // we quickly open and check `git_repository_is_bare`.)

            git_repository* raw_repo = nullptr;
            if (::git_repository_open_ext(&raw_repo, c_path, GIT_REPOSITORY_OPEN_CROSS_FS, nullptr) == 0) {
                repository_ptr const repo(raw_repo);
                return repo != nullptr && (::git_repository_is_bare(repo.get()) == 0);
            }
        }

        return false;
    } catch (...) {
        // todo: log the failure
        return false;
    }

} // namespace dana::git
