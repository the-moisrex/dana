#include "../core/process.hpp"
#include "../sdk/errors.hpp"
#include "../sdk/prompt_registry.hpp"
#include "../sdk/stage.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <ostream>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dana {

    namespace {

        enum struct [[nodiscard]] repo_format : std::uint8_t {
            compact,
            tree,
            paths,
            dirs
        };

        struct repo_options {
            repo_format              format = repo_format::compact;
            std::size_t              depth  = 0; // 0 = unlimited
            std::string_view         depth_str;
            std::vector<std::string> dirs;
        };

        // Single-quote a value for embedding inside a bash -c pipeline the prompt will
        // re-parse (run_command adds the outer layer of quoting).
        std::string sh_quote(std::string_view s) {
            std::string out = "'";
            for (char c : s) {
                if (c == '\'') {
                    out += "'\\''";
                } else {
                    out += c;
                }
            }
            out += '\'';
            return out;
        }

        // Port of `git ls-files [-- DIR] | tree [-L depth] --fromfile`. Run as one
        // bash -c pipeline: tree reads the file list from the pipe exactly like the
        // script (its colour/summary output depends on how it is invoked — run_pipeline
        // feeds it a temp file and tree changes its default output under popen).
        std::string run_tree(text::string_views ls_dirs, text::string_views tree_args) {
            std::string ls = "git ls-files";
            if (!ls_dirs.empty()) {
                ls += " --";
                for (auto const& d : ls_dirs) {
                    ls += ' ' + sh_quote(d);
                }
            }
            std::string tr = "tree";
            for (auto const& a : tree_args) {
                tr += ' ' + sh_quote(a);
            }
            tr                               += " --fromfile";
            std::vector<std::string> storage  = {"bash", "-c", ls + " | " + tr};
            return dana::process::run_command(storage).stdout_data;
        }

        repo_format parse_format(std::string_view value) {
            if (value == "compact") {
                return repo_format::compact;
            }
            if (value == "tree") {
                return repo_format::tree;
            }
            if (value == "paths") {
                return repo_format::paths;
            }
            if (value == "dirs") {
                return repo_format::dirs;
            }
            throw usage_error{"repo: unknown --format '" + std::string(value) + "' (compact, tree, paths, dirs)"};
        }

        std::size_t parse_depth(std::string_view const text) {
            if (text.empty()) {
                throw usage_error{"repo: --depth needs a number"};
            }
            std::size_t value = 0;
            for (char const c : text) {
                if (c < '0' || c > '9') {
                    throw usage_error{"repo: --depth needs a positive number, got '" + std::string(text) + "'"};
                }
                value = (value * 10) + static_cast<std::size_t>(c - '0');
            }
            if (value == 0) {
                throw usage_error{"repo: --depth must be at least 1"};
            }
            return value;
        }

        repo_options parse_repo_args(view const& v) {
            repo_options opts;
            for (std::size_t i = 0; i < v.args.size();) {
                std::string_view const arg = v.args[i];
                if (arg == "--format") {
                    if (i + 1 >= v.args.size()) {
                        throw usage_error{"repo: --format needs a value"};
                    }
                    opts.format  = parse_format(v.args[i + 1]);
                    i           += 2;
                } else if (arg.starts_with("--format=")) {
                    opts.format = parse_format(arg.substr(std::string_view{"--format="}.size()));
                    ++i;
                } else if (arg == "--tree") {
                    opts.format = repo_format::tree;
                    ++i;
                } else if (arg == "--depth") {
                    if (i + 1 >= v.args.size()) {
                        throw usage_error{"repo: --depth needs a value"};
                    }
                    opts.depth_str  = v.args[i + 1];
                    opts.depth      = parse_depth(opts.depth_str);
                    i              += 2;
                } else if (arg.starts_with("--depth=")) {
                    opts.depth_str = arg.substr(std::string_view{"--depth="}.size());
                    opts.depth     = parse_depth(opts.depth_str);
                    ++i;
                } else if (arg.size() > 1 && arg.front() == '-') {
                    throw usage_error{"repo: unknown option " + std::string(arg)};
                } else {
                    opts.dirs.emplace_back(arg);
                    ++i;
                }
            }
            return opts;
        }

        // `git ls-files -z` once: the compact/paths/dirs formats need no `tree` binary,
        // and the NUL separator keeps any path intact.
        std::vector<std::string> list_tracked(text::string_views dirs) {
            std::vector<std::string> argv{"git", "ls-files", "-z"};
            if (!dirs.empty()) {
                argv.emplace_back("--");
                argv.insert(argv.end(), dirs.begin(), dirs.end());
            }
            auto const res = process::command(argv).capture_stderr().run();
            if (res.exit_code != 0) {
                throw soft_error{"prompt repo: git ls-files failed: " + res.stderr_data};
            }

            std::vector<std::string> files;
            std::size_t              start = 0;
            while (start < res.stdout_data.size()) {
                std::size_t const end  = res.stdout_data.find('\0', start);
                std::size_t const stop = end == std::string::npos ? res.stdout_data.size() : end;
                if (stop > start) {
                    files.emplace_back(res.stdout_data.substr(start, stop - start));
                }
                if (end == std::string::npos) {
                    break;
                }
                start = end + 1;
            }
            std::sort(files.begin(), files.end());
            return files;
        }

        struct dir_node {
            std::set<std::string_view> dirs;  // child directory names
            std::set<std::string_view> files; // file names directly in this directory
        };

        using dir_map = std::map<std::string_view, dir_node>;

        // One node per directory, keyed by its root-relative path ("" is the root).
        dir_map build_tree(text::string_views files) {
            dir_map tree;
            for (auto const path : files) {
                std::size_t const slash = path.rfind('/');
                auto const        dir   = slash == std::string_view::npos ? std::string_view{} : path.substr(0, slash);
                auto const        name  = slash == std::string_view::npos ? path : path.substr(slash + 1);
                tree[dir].files.insert(name);

                std::string prefix;
                std::size_t start = 0;
                while (start <= dir.size()) {
                    std::size_t const next = dir.find('/', start);
                    auto const        child =
                      dir.substr(start, next == std::string_view::npos ? std::string_view::npos : next - start);
                    if (child.empty()) {
                        break;
                    }
                    tree[prefix].dirs.insert(child);
                    if (!prefix.empty()) {
                        prefix += '/';
                    }
                    prefix += child;
                    if (next == std::string::npos) {
                        break;
                    }
                    start = next + 1;
                }
            }
            return tree;
        }

        std::size_t dir_depth(std::string_view key) {
            if (key.empty()) {
                return 0;
            }
            return static_cast<std::size_t>(std::count(key.begin(), key.end(), '/')) + 1;
        }

        // Files under each directory, accumulated bottom-up: the map is sorted, so a
        // child key ("a/b") always follows its parent ("a"), and reverse iteration
        // visits every descendant before its ancestor.
        std::map<std::string_view, std::size_t> subtree_file_counts(dir_map const& tree) {
            std::map<std::string_view, std::size_t> counts;
            for (auto const& [key, node] : tree) {
                counts[key] = node.files.size();
            }
            for (auto it = counts.rbegin(); it != counts.rend(); ++it) {
                std::size_t const slash = it->first.rfind('/');
                if (slash == std::string::npos) {
                    continue;
                }
                counts[it->first.substr(0, slash)] += it->second;
            }
            return counts;
        }

        std::string_view plural_files(std::size_t count) {
            return count == 1 ? "file" : "files";
        }

        std::string
        render_compact(dir_map const& tree, std::map<std::string_view, std::size_t> const& counts, std::size_t limit) {
            std::string out;
            for (auto const& [key, node] : tree) {
                std::size_t const depth = dir_depth(key);
                if (limit > 0 && depth > limit) {
                    continue;
                }
                if (node.dirs.empty() && node.files.empty()) {
                    continue;
                }
                if (limit > 0 && depth == limit && !node.dirs.empty()) {
                    std::size_t const files  = counts.at(key);
                    out                     += std::format("{}/ ({} {})\n", key, files, plural_files(files));
                    continue;
                }
                out        += key.empty() ? "." : key;
                out        += ": ";
                bool first  = true;
                for (auto const& d : node.dirs) {
                    if (!first) {
                        out += ' ';
                    }
                    out   += d;
                    out   += '/';
                    first  = false;
                }
                for (auto const& f : node.files) {
                    if (!first) {
                        out += ' ';
                    }
                    out   += f;
                    first  = false;
                }
                out += '\n';
            }
            return out;
        }

        std::string
        render_dirs(dir_map const& tree, std::map<std::string_view, std::size_t> const& counts, std::size_t limit) {
            std::string out;
            for (auto const& [key, node] : tree) {
                if (limit > 0 && dir_depth(key) > limit) {
                    continue;
                }
                if (node.dirs.empty() && node.files.empty()) {
                    continue;
                }
                std::size_t const files = counts.at(key);
                out += std::format("{}/: {} {}\n", key.empty() ? "." : key, files, plural_files(files));
            }
            return out;
        }

        std::string render_paths(text::string_views files) {
            std::string out;
            for (auto const& f : files) {
                out += f;
                out += '\n';
            }
            return out;
        }

    } // namespace

    static void execute_repo(view const& v, result& r) {
        // repo.sh scans every arg for --help/-h itself (before the git check);
        // the native dispatcher already intercepted those before we run.

        if (process::run_command({"git", "rev-parse", "--is-inside-work-tree"}).exit_code != 0) [[unlikely]] {
            throw soft_error{"prompt repo: Not inside a git repository.\n"};
        }

        repo_options const opts = parse_repo_args(v);

        if (opts.format == repo_format::tree) {
            std::vector<std::string_view> tree_args;
            if (opts.depth > 0) {
                tree_args.emplace_back("-L");
                tree_args.emplace_back(opts.depth_str);
            }
            std::string output = "# Repository file structure (git-tracked files)\n\n";
            if (opts.dirs.empty()) {
                output += run_tree({}, tree_args);
            } else {
                for (auto const& dir : opts.dirs) {
                    output += run_tree({dir}, tree_args);
                }
            }
            r.out = std::move(output);
            return;
        }

        std::vector<std::string> const files = list_tracked(opts.dirs);

        if (opts.format == repo_format::paths) {
            std::vector<std::string> visible;
            for (auto const& f : files) {
                std::size_t const depth = static_cast<std::size_t>(std::count(f.begin(), f.end(), '/'));
                if (opts.depth > 0 && depth > opts.depth) {
                    continue;
                }
                visible.push_back(f);
            }
            r.out = render_paths(visible);
            return;
        }

        dir_map const     tree      = build_tree(files);
        auto const        counts    = subtree_file_counts(tree);
        std::size_t const dir_count = tree.contains(std::string{}) ? tree.size() - 1 : tree.size();
        std::string       output    = std::format(
          "# Repository file structure (git-tracked, {} files, {} directories)\n\n",
          files.size(),
          dir_count);

        switch (opts.format) {
            case repo_format::compact: output += render_compact(tree, counts, opts.depth); break;
            case repo_format::dirs: output += render_dirs(tree, counts, opts.depth); break;
            case repo_format::paths:
            case repo_format::tree: break; // handled above
        }
        r.out = std::move(output);
    }

    static void render_help_repo(std::ostream& os) {
        os << R"EOF(Usage: prompt repo [--format compact|tree|paths|dirs] [--depth N] [DIR...]

Displays the repository's file structure using only git-tracked files.
Useful for giving an AI a high-level view of the codebase.

The default `compact` format prints one line per directory (`dir: subdir/ file ...`),
which carries the same paths as a tree at a fraction of the tokens.

Options:
  --format FMT  Output format: compact (default), tree, paths, dirs
  --tree        Shorthand for --format tree (the box-drawing tree, for humans)
  --depth N     Limit directory depth in every format (default: unlimited); in
                compact a directory at the limit is shown as `path/ (N files)`
  --help, -h    Show this help message

Formats:
  compact  One line per directory: `dir: subdir/ file file ...` (default)
  tree     The classic `tree` rendering (needs the `tree` binary)
  paths    Plain `git ls-files` paths, one per line, no header
  dirs     One line per directory with the number of files under it

Arguments:
  DIR...        Optional directory prefixes to limit the view (e.g., bin/ prompts/)
)EOF";
    }

    // This file is the prompt: it implements it and registers it at static-init time
    // (no central list — see sdk/prompt_registry.hpp).
    [[maybe_unused]] bool const registered = dana::register_prompt(
      "repo",
      "Displays the repository's file structure using only git-tracked files. Useful for giving an AI "
      "a high-level view of the codebase.",
      &execute_repo,
      &render_help_repo);

} // namespace dana
