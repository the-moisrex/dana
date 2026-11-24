#include "environment.hpp"

#include "core/clipboard.hpp"
#include "core/fs.hpp"
#include "core/fuzzy.hpp"
#include "core/git.hpp"
#include "core/process.hpp"

#include <exception>
#include <filesystem>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace dana {

    namespace {

        // `fzf -m` over whatever the caller pipes in: the file prompts' interactive
        // picker, used when a prompt is invoked with no FILE arguments.
        std::vector<std::filesystem::path> select_files_with_fzf() {
            auto                               result = process::run_command({"fzf", "-m"});
            std::vector<std::filesystem::path> files;
            std::string                        line;
            for (char c : result.stdout_data) {
                if (c == '\n') {
                    if (!line.empty()) {
                        files.emplace_back(line);
                        line.clear();
                    }
                } else {
                    line += c;
                }
            }
            if (!line.empty()) {
                files.emplace_back(line);
            }
            return files;
        }

        namespace stdfs = std::filesystem;

        bool is_file(stdfs::path const& p) {
            std::error_code ec;
            return stdfs::is_regular_file(p, ec);
        }

        bool is_file_or_directory(stdfs::path const& p) {
            std::error_code ec;
            auto const      status = stdfs::status(p, ec);
            return !ec && (stdfs::is_regular_file(status) || stdfs::is_directory(status));
        }

        // Recognizes both ordinary repositories (.git directory) and worktrees /
        // submodules (.git file). No subprocess is needed for root discovery.
        std::optional<stdfs::path> find_git_root(stdfs::path dir) {
            for (;;) {
                if (is_file_or_directory(dir / ".git")) {
                    return dir;
                }

                auto const parent = dir.parent_path();
                if (parent.empty() || parent == dir) {
                    return std::nullopt;
                }

                dir = parent;
            }
        }

        // Git is an optional accelerator; no listing selects native traversal.
        [[nodiscard]] std::optional<std::vector<std::string>> try_git_listing(stdfs::path const& root) try {
            return dana::git::all_files(root.string());
        } catch (std::bad_alloc const&) {
            throw;
        } catch (std::exception const&) {
            // Narrow to the process API's launch exception when available.
            return std::nullopt;
        }

        // Resolve an exact path, then a repository-relative path, then a ranked
        // match from Git's file list. Fall back to native filesystem traversal
        // when Git is unavailable or produces no usable match.
        // Exact matches may be files or directories; fuzzy matches are files only.
        std::optional<std::filesystem::path> resolve_input_file(std::string_view name) {
            // Filesystem paths cannot contain embedded NULs.
            if (name.empty() || name.contains('\0')) {
                return std::nullopt;
            }

            std::error_code   ec;
            stdfs::path const input{std::string(name)};

            if (is_file_or_directory(input)) {
                auto absolute = stdfs::absolute(input, ec);
                if (!ec) {
                    return absolute;
                }
                return std::nullopt;
            }

            // Do not reinterpret an invalid absolute path as a fuzzy relative query.
            if (input.is_absolute()) {
                return std::nullopt;
            }

            auto const cwd = stdfs::current_path(ec);
            if (ec) {
                return std::nullopt;
            }

            auto const        git_root = find_git_root(cwd);
            stdfs::path const root     = git_root ? *git_root : cwd;

            if (git_root) {
                auto candidate = root / input;
                if (is_file_or_directory(candidate)) {
                    return candidate;
                }
            }

            std::string const query = input.lexically_normal().generic_string();
            if (query.empty() || query == ".") {
                return std::nullopt;
            }

            std::optional<stdfs::path> best_path;
            std::optional<fuzzy::rank> best_rank;
            std::string                best_relative;

            auto consider = [&](stdfs::path const& relative) {
                std::string const text = relative.generic_string();
                auto const        rank = fuzzy::rank_path(text, query);
                if (!rank) {
                    return;
                }

                // Stable tie-breaking, independent of Git/filesystem listing order.
                if (best_rank) {
                    if (fuzzy::rank_key(*rank) > fuzzy::rank_key(*best_rank)) {
                        return;
                    }
                    if (fuzzy::rank_key(*rank) == fuzzy::rank_key(*best_rank) && text >= best_relative) {
                        return;
                    }
                }

                auto candidate = root / relative;

                // Only stat candidates that could improve the current result.
                // This also rejects tracked files deleted from the working tree.
                if (!is_file(candidate)) {
                    return;
                }

                best_path     = std::move(candidate);
                best_rank     = rank;
                best_relative = text;
            };

            if (git_root) {
                auto const listing = try_git_listing(root);

                if (listing) {
                    for (auto const& entry : *listing) {
                        stdfs::path const relative{entry};

                        // Git normally guarantees repository-relative paths.
                        if (!relative.is_absolute()) {
                            consider(relative);
                        }
                    }
                }

                if (best_path) {
                    return best_path;
                }
            }

            // Native fallback: searches the entire repository, not just cwd.
            //
            // Use an explicit directory stack so a failure reading one directory
            // does not abandon other queued directories. Do not follow directory
            // symlinks, avoiding cycles.
            std::vector<stdfs::path> pending{root};

            while (!pending.empty()) {
                stdfs::path directory = std::move(pending.back());
                pending.pop_back();

                ec.clear();
                stdfs::directory_iterator       it(directory, stdfs::directory_options::skip_permission_denied, ec);
                stdfs::directory_iterator const end;

                while (!ec && it != end) {
                    stdfs::directory_entry const entry = *it;

                    if (entry.path().filename() != ".git") {
                        std::error_code status_ec;
                        auto const      status = entry.symlink_status(status_ec);

                        if (!status_ec) {
                            if (stdfs::is_directory(status)) {
                                pending.push_back(entry.path());
                            } else if (stdfs::is_regular_file(status) || stdfs::is_symlink(status)) {
                                // Lexical only: unlike stdfs::relative(), no extra
                                // canonicalization or filesystem queries.
                                consider(entry.path().lexically_relative(root));
                            }
                        }
                    }

                    it.increment(ec);
                }
            }

            return best_path;
        }

    } // namespace

    environment make_environment() {
        std::error_code ec;
        auto            working_dir = std::filesystem::current_path(ec);
        if (ec) {
            working_dir.clear();
        }

        return environment{
          .git_root             = fs::find_git_root().value_or(std::filesystem::path{}),
          .working_dir          = std::move(working_dir),
          .exe_path             = fs::self_path().value_or(std::filesystem::path{}),
          .clipboard_content    = &clipboard::paste,
          .clipboard_identifier = [] -> std::optional<std::string> {
              return clipboard::detect_identifier(clipboard::paste());
          },
          .select_files       = &select_files_with_fzf,
          .resolve_input_file = &resolve_input_file,
        };
    }

} // namespace dana
