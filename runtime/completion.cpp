#include "completion.hpp"

#include "../core/fuzzy.hpp"
#include "../core/git.hpp"
#include "prompt_registry.hpp"

#include <algorithm>
#include <cctype>
#include <clocale>
#include <cstring>
#include <exception>
#include <filesystem>
#include <new>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace dana {

    namespace {

        // Locale-aware name ordering, the collation the catalog itself uses.
        bool name_less(std::string_view a, std::string_view b) {
            static bool const locale_set = std::setlocale(LC_COLLATE, "") != nullptr;
            (void) locale_set;
            std::string const ka(a);
            std::string const kb(b);
            int const         c = std::strcoll(ka.c_str(), kb.c_str());
            if (c != 0) {
                return c < 0;
            }
            return a < b; // collation-equal names: deterministic tiebreak
        }

        // A fuzzy completion list is a menu, not a file listing: the top matches are
        // what the user can pick from, and a one-letter query would otherwise dump
        // the whole repository into the shell.
        constexpr std::size_t kMaxFileCandidates = 100;

        // Native traversal for a tree Git does not know (or when Git is unavailable).
        // Hidden directories are skipped, matching the file prompts' own walk.
        std::vector<std::string> walk_files(std::filesystem::path const& base) {
            std::vector<std::string> files;

            std::error_code                               error;
            std::filesystem::recursive_directory_iterator iterator(
              base,
              std::filesystem::directory_options::skip_permission_denied,
              error);
            std::filesystem::recursive_directory_iterator const end;
            if (error) {
                return files;
            }

            while (iterator != end) {
                auto const&     path = iterator->path();
                std::error_code type_error;
                if (iterator->is_directory(type_error) && path.filename().string().starts_with('.')) {
                    iterator.disable_recursion_pending();
                    iterator.increment(error);
                    continue;
                }
                if (!type_error && iterator->is_regular_file(type_error)) {
                    files.push_back(path.lexically_relative(base).generic_string());
                }
                iterator.increment(error);
            }
            return files;
        }

        // Repository-relative paths under `git_root`, or cwd-relative paths when
        // there is no repository.
        std::vector<std::string> candidate_files(std::filesystem::path const& git_root) {
            std::error_code error;
            auto const      cwd  = std::filesystem::current_path(error);
            auto const      base = !git_root.empty() ? git_root : cwd;
            if (base.empty()) {
                return {};
            }

            if (!git_root.empty()) {
                std::vector<std::string> files;
                try {
                    files = git::all_files(git_root.string());
                } catch (std::bad_alloc const&) {
                    throw;
                } catch (std::exception const&) {
                    // Not a repository (or libgit2 failed): walk it instead.
                    files.clear();
                }
                if (!files.empty()) {
                    return files;
                }
            }
            return walk_files(base);
        }

        // A prompt's usage block names its positional operands; a `FILE` operand
        // means the shell should be offered paths even when the prompt does not embed
        // them itself (`.cat`, `.fix`, `.review`, …). Only the usage block is
        // inspected, so a `--file` option's description cannot trigger it.
        bool usage_mentions_file(std::string_view help) {
            auto const                 gap   = help.find("\n\n");
            std::string_view const     usage = help.substr(0, gap);
            constexpr std::string_view kFile = "FILE";
            for (auto pos = usage.find(kFile); pos != std::string_view::npos; pos = usage.find(kFile, pos + 1)) {
                bool const before_ok = pos == 0 || std::isalnum(static_cast<unsigned char>(usage.at(pos - 1))) == 0;
                auto const after     = pos + kFile.size();
                bool const after_ok =
                  after >= usage.size() || std::isalnum(static_cast<unsigned char>(usage.at(after))) == 0;
                if (before_ok && after_ok) {
                    return true;
                }
            }
            return false;
        }

    } // namespace

    void write_file_candidates(
      std::ostream&                out,
      std::string_view             prefix,
      std::filesystem::path const& git_root,
      text::string_views           prior_args) {
        if (prefix.empty() || prefix.front() == '-' || prefix.front() == '+') {
            return;
        }

        std::string query{prefix};
        if (query.starts_with("./")) {
            query.erase(0, 2);
        }
        if (query.empty()) {
            return;
        }

        std::error_code error;
        auto const      cwd  = std::filesystem::current_path(error);
        auto const      base = !git_root.empty() ? git_root : cwd;
        if (base.empty()) {
            return;
        }

        struct listed_file {
            std::filesystem::path absolute;
            std::string           value;
        };

        std::vector<listed_file> listed;
        for (auto const& relative : candidate_files(git_root)) {
            std::filesystem::path const absolute = (base / relative).lexically_normal();
            std::filesystem::path       shown    = error ? absolute : absolute.lexically_relative(cwd);
            if (shown.empty()) {
                shown = absolute;
            }
            listed.push_back({.absolute = absolute, .value = shown.generic_string()});
        }

        // The files a prompt already embeds are not candidates again. A prior token
        // is matched exactly when it names a real file/directory, otherwise by a
        // confident fuzzy match (`.files tse_similarity.cpp` resolves to
        // `tools/tse_similarity.cpp`). A weak match is ignored so a flag value such
        // as `--format json` does not hide `package.json`.
        std::vector<std::filesystem::path> excluded_files;
        std::vector<std::filesystem::path> excluded_directories;
        for (auto const prior : prior_args) {
            if (prior.empty() || prior.front() == '-') {
                continue;
            }

            std::filesystem::path const arg{std::string(prior)};
            std::error_code             status_error;
            auto const                  classify = [&](std::filesystem::path const& path) {
                auto const status = std::filesystem::status(path, status_error);
                if (status_error) {
                    return false;
                }
                if (std::filesystem::is_directory(status)) {
                    excluded_directories.push_back(path.lexically_normal());
                    return true;
                }
                if (std::filesystem::is_regular_file(status)) {
                    excluded_files.push_back(path.lexically_normal());
                    return true;
                }
                return false;
            };

            if (arg.is_absolute()) {
                if (classify(arg)) {
                    continue;
                }
            } else {
                if (classify(cwd / arg)) {
                    continue;
                }
                if (!git_root.empty() && classify(git_root / arg)) {
                    continue;
                }
            }

            std::optional<fuzzy::rank> best_rank;
            std::string                best_value;
            std::filesystem::path      best_path;
            for (auto const& file : listed) {
                auto const rank = fuzzy::rank_path(file.value, prior);
                if (!rank) {
                    continue;
                }
                if (rank->kind != fuzzy::match_kind::exact_path && rank->kind != fuzzy::match_kind::exact_name) {
                    continue;
                }
                if (!best_rank || fuzzy::rank_key(*rank) < fuzzy::rank_key(*best_rank) ||
                    (fuzzy::rank_key(*rank) == fuzzy::rank_key(*best_rank) && file.value < best_value))
                {
                    best_rank  = rank;
                    best_value = file.value;
                    best_path  = file.absolute;
                }
            }
            if (best_rank) {
                excluded_files.push_back(best_path.lexically_normal());
            }
        }

        auto const is_under = [](std::filesystem::path const& child, std::filesystem::path const& parent) {
            std::string const child_text  = child.lexically_normal().generic_string();
            std::string const parent_text = parent.lexically_normal().generic_string();
            if (child_text == parent_text) {
                return true;
            }
            std::string prefix_text = parent_text;
            if (!prefix_text.empty() && prefix_text.back() != '/') {
                prefix_text += '/';
            }
            return child_text.starts_with(prefix_text);
        };

        struct candidate {
            std::string value;
            std::string description;
            fuzzy::rank rank{};
        };

        std::vector<candidate> matches;
        std::set<std::string>  directories;

        for (auto const& file : listed) {
            if (std::ranges::find(excluded_files, file.absolute) != excluded_files.end()) {
                continue;
            }
            if (std::ranges::any_of(excluded_directories,
                                    [&](std::filesystem::path const& directory) {
                                        return is_under(file.absolute, directory);
                                    }))
            {
                continue;
            }

            if (auto rank = fuzzy::rank_path(file.value, query)) {
                matches.push_back({.value = file.value, .description = "file", .rank = *rank});
            }

            // The full path is what gets inserted, so every ancestor is a valid
            // place to descend into.
            auto dir = std::filesystem::path{file.value}.parent_path();
            while (!dir.empty() && dir != ".") {
                std::string text = dir.generic_string();
                if (!text.empty() && text.back() != '/') {
                    text += '/';
                }
                directories.insert(std::move(text));

                auto const parent = dir.parent_path();
                if (parent == dir) {
                    break;
                }
                dir = parent;
            }
        }

        for (auto const& directory : directories) {
            std::filesystem::path const directory_path{directory};
            auto const                  directory_absolute =
              (directory_path.is_absolute() ? directory_path : cwd / directory_path).lexically_normal();
            if (std::ranges::find(excluded_directories, directory_absolute) != excluded_directories.end()) {
                continue;
            }

            std::string_view target{directory};
            if (target.ends_with('/')) {
                target.remove_suffix(1);
            }
            if (auto rank = fuzzy::rank_path(target, query)) {
                matches.push_back({.value = directory, .description = "directory", .rank = *rank});
            }
        }

        std::ranges::sort(matches, [](candidate const& left, candidate const& right) {
            if (fuzzy::rank_key(left.rank) != fuzzy::rank_key(right.rank)) {
                return fuzzy::rank_key(left.rank) < fuzzy::rank_key(right.rank);
            }
            return left.value < right.value;
        });
        if (matches.size() > kMaxFileCandidates) {
            matches.resize(kMaxFileCandidates);
        }
        for (auto const& match : matches) {
            completion::write_item(out, match.value, match.description);
        }
    }

    void write_prompt_candidates(
      prompt_descriptor const&     desc,
      std::string_view             prefix,
      text::string_views           prior_args,
      std::ostream&                out,
      std::filesystem::path const& git_root) {
        // A previous token that is a flag means this is its value (a number, a
        // format, ...), not another flag or a file.
        bool const after_flag = !prior_args.empty() && prior_args.back().starts_with('-');

        // Flags first (they are what an empty prefix mostly wants), then whatever
        // the prompt knows about its own operands.
        std::string help;
        if (desc.render_help_fn != nullptr) {
            std::ostringstream rendered;
            desc.render_help_fn(rendered);
            help = rendered.str();
            if (!after_flag) {
                completion::write_help_flags(out, prefix, help);
            }
        }
        if (desc.complete_fn != nullptr) {
            desc.complete_fn(completion_request{.prefix = prefix, .prior_args = prior_args, .git_root = git_root}, out);
        }
        // A file prompt's positional argument is a path.
        if ((desc.emits_files || usage_mentions_file(help)) && !after_flag) {
            write_file_candidates(out, prefix, git_root, prior_args);
        }
    }

    void write_step_names(std::ostream& out, std::string_view prefix, std::span<name_candidate const> candidates) {
        auto const                  prompts = get_all_prompts();
        std::vector<name_candidate> all;
        all.reserve(prompts.size() + candidates.size());
        for (auto const& desc : prompts) {
            all.push_back({.name = std::string(desc.name), .description = std::string(desc.help_summary)});
        }
        for (auto const& cand : candidates) {
            all.push_back(cand);
        }
        std::sort(all.begin(), all.end(), [](name_candidate const& a, name_candidate const& b) {
            return name_less(a.name, b.name);
        });

        for (auto const& cand : all) {
            if (completion::matches(prefix, cand.name)) {
                completion::write_item(out, cand.name, cand.description);
            }
            // `.name`/`-name` are the same step; offer them so a chain step can be
            // completed with the prefix the user is already typing.
            std::string alternate = "." + cand.name;
            if (completion::matches(prefix, alternate)) {
                completion::write_item(out, alternate, cand.description);
            }
            alternate = "-" + cand.name;
            if (completion::matches(prefix, alternate)) {
                completion::write_item(out, alternate, cand.description);
            }
        }
    }

    void write_prompt_names(std::ostream& out, std::string_view prefix) {
        write_step_names(out, prefix);
    }

} // namespace dana
