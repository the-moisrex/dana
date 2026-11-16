#include "file_filter.hpp"

#include "../core/glob.hpp"
#include "../core/process.hpp"
#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace prompt {

namespace fs_std = std::filesystem;

namespace {

// Split a newline-separated fact into its non-empty entries.
std::vector<std::string> split_lines(std::string_view value) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= value.size()) {
        std::size_t const end = value.find('\n', start);
        std::string_view const piece = value.substr(start, end == std::string_view::npos ? end : end - start);
        if (!piece.empty()) out.emplace_back(piece);
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return out;
}

std::string join_lines(std::vector<std::string> const& lines) {
    std::string out;
    for (auto const& line : lines) {
        out += line;
        out += '\n';
    }
    return out;
}

// The absolute, lexically normalized form of `path` (never touches the disk).
fs_std::path absolute_normal(fs_std::path const& path) {
    std::error_code ec;
    auto absolute = fs_std::absolute(path, ec);
    if (ec) return path.lexically_normal();
    return absolute.lexically_normal();
}

bool is_under(fs_std::path const& path, fs_std::path const& root) {
    if (root.empty()) return false;
    auto const rel = path.lexically_relative(root);
    if (rel.empty()) return path == root;
    auto const first = rel.begin();
    return first != rel.end() && *first != "..";
}

// True when any path component of `relative` equals `name`.
bool has_component(std::string_view relative, std::string_view name) {
    std::size_t start = 0;
    while (start <= relative.size()) {
        std::size_t const end = relative.find('/', start);
        std::string_view const piece = relative.substr(start, end == std::string_view::npos ? end : end - start);
        if (piece == name) return true;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return false;
}

// The file list git would show: tracked plus untracked-but-not-ignored files,
// NUL-separated so spaces and newlines survive.
std::optional<std::vector<fs_std::path>> git_listing(fs_std::path const& git_root) {
    auto const result = process::run_command(
        std::vector<std::string>{"git", "-C", git_root.string(), "ls-files", "--cached", "--others", "--exclude-standard",
                                 "-z"},
        "");
    if (result.exit_code != 0) return std::nullopt;

    std::vector<fs_std::path> files;
    std::string_view const bytes = result.stdout_data;
    std::size_t begin = 0;
    while (begin < bytes.size()) {
        auto const end = bytes.find('\0', begin);
        if (end == std::string_view::npos) break;
        if (end != begin) files.emplace_back(git_root / std::string(bytes.substr(begin, end - begin)));
        begin = end + 1;
    }
    return files;
}

// A native recursive walk, used outside a repository or when git cannot list.
void walk_files(fs_std::path const& base, std::vector<fs_std::path>& out) {
    std::error_code ec;
    fs_std::recursive_directory_iterator it(base, fs_std::directory_options::skip_permission_denied, ec);
    fs_std::recursive_directory_iterator const end;
    if (ec) return;

    while (it != end) {
        auto const& path = it->path();
        if (it->is_directory(ec) && path.filename().string().starts_with('.')) {
            it.disable_recursion_pending();
            it.increment(ec);
            continue;
        }
        if (!ec && it->is_regular_file(ec)) out.push_back(path);
        it.increment(ec);
    }
}

std::vector<fs_std::path> parse_picked(std::string_view output, fs_std::path const& base) {
    std::vector<fs_std::path> picked;
    std::size_t start = 0;
    while (start <= output.size()) {
        std::size_t const end = output.find('\n', start);
        std::string_view const line = output.substr(start, end == std::string_view::npos ? end : end - start);
        if (!line.empty()) {
            fs_std::path path{std::string(line)};
            picked.push_back(path.is_absolute() ? path : base / path);
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return picked;
}

} // namespace

file_filter file_filter::from(facts const& prior, environment const& env) {
    file_filter filter;
    filter.git_root_ = env.git_root;
    if (auto const value = prior.get(keys::excluded_files)) filter.exclude_ = split_lines(*value);
    if (auto const value = prior.get(keys::file_root); value && !value->empty()) filter.root_ = fs_std::path{*value};
    return filter;
}

bool file_filter::excluded(fs_std::path const& path) const {
    if (exclude_.empty()) return false;

    auto const absolute = absolute_normal(path);
    std::string const abs = absolute.generic_string();
    std::string const base = absolute.filename().generic_string();

    std::string rel_git;
    if (!git_root_.empty()) rel_git = absolute.lexically_relative(git_root_).generic_string();
    std::string rel_root;
    if (!root_.empty()) rel_root = absolute.lexically_relative(root_).generic_string();

    for (auto const& raw : exclude_) {
        std::string_view pattern = raw;
        while (!pattern.empty() && pattern.back() == '/') pattern.remove_suffix(1);
        if (pattern.empty()) continue;

        if (glob::match(pattern, abs) || glob::match(pattern, base)) return true;
        if (!rel_git.empty() && glob::match(pattern, rel_git)) return true;
        if (!rel_root.empty() && glob::match(pattern, rel_root)) return true;

        // A literal pattern also names a directory: everything below it is out,
        // and a bare name matches a directory component anywhere in the path.
        if (!glob::has_wildcard(pattern)) {
            std::string const literal(pattern);
            if (abs == literal || abs.starts_with(literal + "/")) return true;
            if (!rel_git.empty() && (rel_git == literal || rel_git.starts_with(literal + "/"))) return true;
            if (!rel_root.empty() && (rel_root == literal || rel_root.starts_with(literal + "/"))) return true;
            if (pattern.find('/') == std::string_view::npos &&
                (has_component(rel_git, pattern) || has_component(abs, pattern)))
                return true;
        }
    }
    return false;
}

std::optional<fs_std::path> file_filter::resolve(environment const& env, std::string_view arg) const {
    if (!root_.empty()) {
        fs_std::path const input{std::string(arg)};
        if (!input.is_absolute()) {
            auto const candidate = (root_ / input).lexically_normal();
            std::error_code ec;
            auto const status = fs_std::status(candidate, ec);
            if (!ec && (fs_std::is_regular_file(status) || fs_std::is_directory(status))) return candidate;
        }
    }
    return resolve_input_file(env, arg);
}

std::vector<fs_std::path> file_filter::candidates() const {
    std::vector<fs_std::path> files;

    auto const base = !root_.empty() ? root_ : (!git_root_.empty() ? git_root_ : fs_std::current_path());

    bool listed = false;
    if (!git_root_.empty() && (root_.empty() || is_under(root_, git_root_))) {
        if (auto listing = git_listing(git_root_)) {
            listed = true;
            for (auto& file : *listing) {
                if (!root_.empty() && !is_under(file.lexically_normal(), root_)) continue;
                if (excluded(file)) continue;
                files.push_back(std::move(file));
            }
        }
    }
    if (!listed) {
        std::vector<fs_std::path> walked;
        walk_files(base, walked);
        for (auto& file : walked) {
            if (excluded(file)) continue;
            files.push_back(std::move(file));
        }
    }

    std::ranges::sort(files);
    files.erase(std::unique(files.begin(), files.end()), files.end());
    return files;
}

std::vector<fs_std::path> file_filter::directories() const {
    auto const base = !root_.empty() ? root_ : (!git_root_.empty() ? git_root_ : fs_std::current_path());

    std::vector<fs_std::path> dirs{base};
    for (auto const& file : candidates()) {
        auto dir = file.parent_path();
        while (!dir.empty() && is_under(dir, base)) {
            dirs.push_back(dir);
            if (dir == base) break;
            auto const parent = dir.parent_path();
            if (parent == dir) break;
            dir = parent;
        }
    }

    std::ranges::sort(dirs);
    dirs.erase(std::unique(dirs.begin(), dirs.end()), dirs.end());
    return dirs;
}

std::vector<std::string> file_filter::rg_globs() const {
    std::vector<std::string> globs;
    for (auto const& raw : exclude_) {
        std::string pattern = "!" + raw;
        if (pattern.back() == '/') pattern += "**";
        globs.push_back(std::move(pattern));
    }
    return globs;
}

std::vector<std::string> file_filter::grep_excludes() const {
    std::vector<std::string> flags;
    for (auto const& raw : exclude_) {
        std::string_view pattern = raw;
        while (!pattern.empty() && pattern.back() == '/') pattern.remove_suffix(1);
        if (pattern.empty() || pattern.find('/') != std::string_view::npos) continue;
        flags.push_back("--exclude=" + std::string(pattern));
        if (!glob::has_wildcard(pattern)) flags.push_back("--exclude-dir=" + std::string(pattern));
    }
    return flags;
}

std::string file_filter::exclude_fact(std::vector<std::string> const& patterns) const {
    std::vector<std::string> merged = exclude_;
    for (auto const& pattern : patterns) {
        if (pattern.empty()) continue;
        if (std::ranges::find(merged, pattern) == merged.end()) merged.push_back(pattern);
    }
    return join_lines(merged);
}

std::string display_path(environment const& env, fs_std::path const& path) {
    auto const base = !env.git_root.empty() ? env.git_root : fs_std::current_path();
    auto const absolute = absolute_normal(path);
    if (is_under(absolute, base)) return absolute.lexically_relative(base).generic_string();
    return absolute.generic_string();
}

std::vector<fs_std::path> select_files(environment const& env, file_filter const& filter) {
    auto const base = !env.git_root.empty() ? env.git_root : fs_std::current_path();
    std::string input;
    for (auto const& file : filter.candidates()) {
        input += display_path(env, file);
        input += '\n';
    }

    auto const result = process::run_command(std::vector<std::string>{"fzf", "-m"}, input);
    return parse_picked(result.stdout_data, base);
}

std::vector<fs_std::path> select_directories(environment const& env, file_filter const& filter) {
    auto const base = !env.git_root.empty() ? env.git_root : fs_std::current_path();
    std::string input;
    for (auto const& dir : filter.directories()) {
        input += display_path(env, dir);
        input += '\n';
    }

    auto const result = process::run_command(std::vector<std::string>{"fzf", "-m"}, input);
    return parse_picked(result.stdout_data, base);
}

} // namespace prompt
