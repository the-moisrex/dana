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

    void write_file_candidates(std::ostream& out, std::string_view prefix, std::filesystem::path const& git_root) {
        if (prefix.empty() || prefix.front() == '-') {
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

        struct candidate {
            std::string value;
            std::string description;
            fuzzy::rank rank{};
        };

        std::vector<candidate> matches;
        std::set<std::string>  directories;

        for (auto const& relative : candidate_files(git_root)) {
            std::filesystem::path const absolute = base / relative;
            std::filesystem::path       shown    = error ? absolute : absolute.lexically_relative(cwd);
            if (shown.empty()) {
                shown = absolute;
            }
            std::string const value = shown.generic_string();

            if (auto rank = fuzzy::rank_path(value, query)) {
                matches.push_back({.value = value, .description = "file", .rank = *rank});
            }

            // The full path is what gets inserted, so every ancestor is a valid
            // place to descend into.
            auto dir = shown.parent_path();
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
        // Flags first (they are what an empty prefix mostly wants), then whatever
        // the prompt knows about its own operands.
        std::string help;
        if (desc.render_help_fn != nullptr) {
            std::ostringstream rendered;
            desc.render_help_fn(rendered);
            help = rendered.str();
            completion::write_help_flags(out, prefix, help);
        }
        if (desc.complete_fn != nullptr) {
            desc.complete_fn(completion_request{.prefix = prefix, .prior_args = prior_args}, out);
        }
        // A file prompt's positional argument is a path. The previous token being a
        // flag means this is its value (a number, a format, ...), not a file.
        bool const after_flag = !prior_args.empty() && prior_args.back().starts_with('-');
        if ((desc.emits_files || usage_mentions_file(help)) && !after_flag) {
            write_file_candidates(out, prefix, git_root);
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
