#include "symbol_index.hpp"

#include "../compress/cpp_scan.hpp"
#include "../core/fuzzy.hpp"
#include "../core/git.hpp"
#include "../core/hash.hpp"
#include "../core/process.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dana::tools {

    namespace {

        constexpr std::string_view kCacheHeader      = "prompt-symbol-index 3";
        constexpr std::size_t      kMaxCandidates    = 100;
        constexpr std::size_t      kMinParallelFiles = 8;

        constexpr std::array<std::string_view, 11> kCppExtensions =
          {".c", ".h", ".cc", ".cpp", ".cxx", ".c++", ".hpp", ".hh", ".hxx", ".ixx", ".inl"};

        constexpr std::array<std::string_view, 11> kCppGlobs =
          {"*.c", "*.h", "*.cc", "*.cpp", "*.cxx", "*.c++", "*.hpp", "*.hh", "*.hxx", "*.ixx", "*.inl"};

        [[nodiscard]] bool is_word_char(char c) noexcept {
            return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
        }

        [[nodiscard]] std::string lower(std::string_view text) {
            std::string out(text);
            for (char& c : out) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return out;
        }

        [[nodiscard]] bool is_cpp_path(std::string_view path) {
            auto const ext = lower(std::filesystem::path(path).extension().string());
            return std::ranges::find(kCppExtensions, ext) != kCppExtensions.end();
        }

        [[nodiscard]] std::filesystem::path canonical_root(std::filesystem::path const& root) {
            std::error_code ec;
            auto const      current = root.empty() ? std::filesystem::current_path(ec) : root;
            if (ec) {
                return root;
            }
            auto const canonical = std::filesystem::weakly_canonical(current, ec);
            return ec ? current : canonical;
        }

        [[nodiscard]] std::vector<std::string> walk_files(std::filesystem::path const& base) {
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
                    auto const relative = path.lexically_relative(base).generic_string();
                    if (is_cpp_path(relative)) {
                        files.push_back(relative);
                    }
                }
                iterator.increment(error);
            }
            std::ranges::sort(files);
            return files;
        }

        [[nodiscard]] std::vector<std::string> cpp_files(std::filesystem::path const& root) {
            std::vector<std::string> files;
            if (git::is_inside_worktree(root.string())) {
                try {
                    files = git::all_files(root.string());
                } catch (std::exception const&) {
                    files.clear();
                }
            }
            if (files.empty()) {
                files = walk_files(root);
            }
            std::erase_if(files, [](std::string const& path) {
                return !is_cpp_path(path);
            });
            std::ranges::sort(files);
            files.erase(std::ranges::unique(files).begin(), files.end());
            return files;
        }

        [[nodiscard]] std::filesystem::path cache_directory() {
            if (char const* explicit_dir = std::getenv("PROMPT_SYMBOL_CACHE_DIR");
                explicit_dir != nullptr && *explicit_dir != '\0')
            {
                return explicit_dir;
            }
            if (char const* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && *xdg != '\0') {
                return std::filesystem::path(xdg) / "prompt" / "symbols";
            }
            if (char const* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
                return std::filesystem::path(home) / ".cache" / "prompt" / "symbols";
            }
            return {};
        }

        [[nodiscard]] std::filesystem::path cache_file(std::filesystem::path const& root) {
            auto const dir = cache_directory();
            if (dir.empty()) {
                return {};
            }
            auto const key = std::format("{:08x}", hash::fnv1a_32(root.generic_string()));
            return dir / (key + ".idx");
        }

        struct file_stamp {
            std::string    path;
            std::int64_t   mtime = 0;
            std::uintmax_t size  = 0;
        };

        [[nodiscard]] std::optional<file_stamp> stamp_of(std::filesystem::path const& root, std::string const& path) {
            std::error_code ec;
            auto const      full  = root / path;
            auto const      mtime = std::filesystem::last_write_time(full, ec);
            if (ec) {
                return std::nullopt;
            }
            auto const size = std::filesystem::file_size(full, ec);
            if (ec) {
                return std::nullopt;
            }
            return file_stamp{
              .path  = path,
              .mtime = static_cast<std::int64_t>(mtime.time_since_epoch().count()),
              .size  = size,
            };
        }

        [[nodiscard]] std::string read_text(std::filesystem::path const& path) {
            std::ifstream file(path, std::ios::binary);
            if (!file) {
                return {};
            }
            return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        }

        [[nodiscard]] std::vector<std::string_view> split_tabs(std::string_view line) {
            std::vector<std::string_view> fields;
            std::size_t                   start = 0;
            while (start <= line.size()) {
                auto const       tab = line.find('\t', start);
                std::string_view field =
                  line.substr(start, tab == std::string_view::npos ? std::string_view::npos : tab - start);
                fields.push_back(field);
                if (tab == std::string_view::npos) {
                    break;
                }
                start = tab + 1;
            }
            return fields;
        }

        [[nodiscard]] std::string single_line(std::string_view text) {
            std::string out(text);
            for (char& c : out) {
                if (c == '\n' || c == '\r' || c == '\t') {
                    c = ' ';
                }
            }
            return out;
        }

        [[nodiscard]] std::string entry_key(symbol_entry const& entry) {
            return entry.name + '\t' + entry.path + '\t' + std::to_string(entry.line);
        }

        [[nodiscard]] std::string ctags_version() {
            auto const result =
              process::command{
                std::vector<std::string>{"ctags", "--version"}
            }
                .capture_stderr()
                .run();
            if (result.exit_code != 0) {
                return {};
            }
            return std::format("{:08x}", hash::fnv1a_32(result.stdout_data));
        }

        [[nodiscard]] std::string relative_path(std::filesystem::path const& root, std::string_view path) {
            std::filesystem::path const given(path);
            if (!given.is_absolute()) {
                return given.generic_string();
            }
            auto const relative = given.lexically_relative(root);
            return relative.empty() ? given.generic_string() : relative.generic_string();
        }

        [[nodiscard]] std::vector<symbol_entry>
        parse_ctags(std::string const& output, std::filesystem::path const& root, bool tabbed) {
            std::vector<symbol_entry> entries;
            std::size_t               start = 0;
            while (start < output.size()) {
                auto const       nl = output.find('\n', start);
                std::string_view line =
                  std::string_view(output).substr(start, nl == std::string::npos ? nl : nl - start);
                start = nl == std::string::npos ? output.size() : nl + 1;
                if (line.empty()) {
                    continue;
                }

                std::string name;
                std::string path;
                std::string kind;
                std::size_t number = 0;

                if (tabbed) {
                    auto const fields = split_tabs(line);
                    if (fields.size() < 4) {
                        continue;
                    }
                    name = std::string(fields[0]);
                    path = std::string(fields[1]);
                    kind = std::string(fields[3]);
                    auto const [ptr, ec] =
                      std::from_chars(fields[2].data(), fields[2].data() + fields[2].size(), number);
                    if (ec != std::errc{} || ptr != fields[2].data() + fields[2].size()) {
                        continue;
                    }
                } else {
                    std::istringstream stream{std::string(line)};
                    stream >> name >> kind >> number >> path;
                }

                if (name.empty() || path.empty() || number == 0) {
                    continue;
                }
                entries.push_back(symbol_entry{
                  .name      = std::move(name),
                  .path      = relative_path(root, path),
                  .line      = number,
                  .end_line  = number,
                  .kind      = std::move(kind),
                  .signature = {},
                });
            }
            return entries;
        }

        [[nodiscard]] std::vector<symbol_entry> run_ctags(std::filesystem::path const&    root,
                                                          std::vector<std::string> const& files) {
            if (files.empty()) {
                return {};
            }

            std::string stdin_data;
            for (auto const& file : files) {
                stdin_data += (root / file).string();
                stdin_data.push_back('\n');
            }

            std::string const xformat = std::string("--_xformat=%N\t%F\t%n\t%K");
            auto              result =
              process::command{
                std::vector<std::string>{"ctags", "-x", xformat, "--languages=C,C++", "-L", "-"}
            }
                .stdin_data(stdin_data)
                .capture_stderr()
                .run();
            if (result.exit_code == 0 && !result.stdout_data.empty()) {
                return parse_ctags(result.stdout_data, root, true);
            }

            result =
              process::command{
                std::vector<std::string>{"ctags", "-x", "--languages=C,C++", "-L", "-"}
            }
                .stdin_data(stdin_data)
                .capture_stderr()
                .run();
            if (result.exit_code != 0) {
                return {};
            }
            return parse_ctags(result.stdout_data, root, false);
        }

        [[nodiscard]] std::vector<symbol_entry> scan_files(std::filesystem::path const&    root,
                                                           std::vector<std::string> const& files) {
            if (files.empty()) {
                return {};
            }

            auto const scan_chunk = [&](std::size_t begin, std::size_t end) {
                std::vector<symbol_entry> entries;
                for (std::size_t i = begin; i < end; ++i) {
                    std::string const content = read_text(root / files[i]);
                    if (content.empty()) {
                        continue;
                    }
                    auto const scanned = compress::cpp::scan(content);
                    for (auto const& sym : scanned.symbols) {
                        if (sym.close || sym.kind == "access" || sym.name.empty()) {
                            continue;
                        }
                        entries.push_back(symbol_entry{
                          .name      = sym.name,
                          .path      = files[i],
                          .line      = sym.line,
                          .end_line  = sym.end_line == 0 ? sym.line : sym.end_line,
                          .kind      = sym.kind,
                          .signature = single_line(sym.signature),
                        });
                    }
                }
                return entries;
            };

            unsigned const    hardware = std::thread::hardware_concurrency();
            std::size_t const workers =
              std::min<std::size_t>(std::max<std::size_t>(1, hardware == 0 ? 1 : hardware), files.size());
            if (files.size() < kMinParallelFiles || workers <= 1) {
                return scan_chunk(0, files.size());
            }

            std::size_t const                                   chunk = (files.size() + workers - 1) / workers;
            std::vector<std::future<std::vector<symbol_entry>>> futures;
            futures.reserve(workers);
            for (std::size_t begin = 0; begin < files.size(); begin += chunk) {
                std::size_t const end = std::min(files.size(), begin + chunk);
                futures.push_back(std::async(std::launch::async, [&, begin, end] {
                    return scan_chunk(begin, end);
                }));
            }

            std::vector<symbol_entry> entries;
            for (auto& future : futures) {
                auto part = future.get();
                entries.insert(entries.end(),
                               std::make_move_iterator(part.begin()),
                               std::make_move_iterator(part.end()));
            }
            return entries;
        }

        void append_unique(std::vector<symbol_entry>&       out,
                           std::unordered_set<std::string>& seen,
                           std::vector<symbol_entry> const& entries) {
            for (auto const& entry : entries) {
                if (seen.insert(entry_key(entry)).second) {
                    out.push_back(entry);
                }
            }
        }

        struct cache_data {
            std::vector<file_stamp>   files;
            std::vector<symbol_entry> entries;
            bool                      ctags = false;
            std::string               ctags_version;
        };

        [[nodiscard]] std::optional<cache_data> read_cache(std::filesystem::path const& path,
                                                           std::filesystem::path const& root) {
            if (path.empty()) {
                return std::nullopt;
            }
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                return std::nullopt;
            }

            std::string line;
            if (!std::getline(in, line) || line != kCacheHeader) {
                return std::nullopt;
            }
            if (!std::getline(in, line) || line != "root " + root.string()) {
                return std::nullopt;
            }

            cache_data data;
            if (!std::getline(in, line) || !line.starts_with("ctags ")) {
                return std::nullopt;
            }
            data.ctags = line == "ctags 1";
            if (!std::getline(in, line) || !line.starts_with("ctags_version ")) {
                return std::nullopt;
            }
            data.ctags_version = line.substr(std::string_view("ctags_version ").size());

            while (std::getline(in, line)) {
                if (line.starts_with("F\t")) {
                    auto const fields = split_tabs(line);
                    if (fields.size() < 4) {
                        continue;
                    }
                    std::int64_t   mtime = 0;
                    std::uintmax_t size  = 0;
                    auto const [mptr, mec] =
                      std::from_chars(fields[2].data(), fields[2].data() + fields[2].size(), mtime);
                    auto const [sptr, sec] =
                      std::from_chars(fields[3].data(), fields[3].data() + fields[3].size(), size);
                    if (mec != std::errc{} || sec != std::errc{} || mptr != fields[2].data() + fields[2].size() ||
                        sptr != fields[3].data() + fields[3].size())
                    {
                        continue;
                    }
                    data.files.push_back(file_stamp{
                      .path  = std::string(fields[1]),
                      .mtime = mtime,
                      .size  = size,
                    });
                } else if (line.starts_with("S\t")) {
                    auto const fields = split_tabs(line);
                    if (fields.size() < 7) {
                        continue;
                    }
                    std::size_t number = 0;
                    std::size_t end    = 0;
                    auto const [lptr, lec] =
                      std::from_chars(fields[3].data(), fields[3].data() + fields[3].size(), number);
                    auto const [eptr, eec] =
                      std::from_chars(fields[4].data(), fields[4].data() + fields[4].size(), end);
                    if (lec != std::errc{} || eec != std::errc{} || lptr != fields[3].data() + fields[3].size() ||
                        eptr != fields[4].data() + fields[4].size())
                    {
                        continue;
                    }
                    data.entries.push_back(symbol_entry{
                      .name      = std::string(fields[1]),
                      .path      = std::string(fields[2]),
                      .line      = number,
                      .end_line  = end,
                      .kind      = std::string(fields[5]),
                      .signature = std::string(fields[6]),
                    });
                }
            }
            return data;
        }

        void write_cache(std::filesystem::path const& path, std::filesystem::path const& root, cache_data const& data) {
            if (path.empty()) {
                return;
            }
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            if (ec) {
                return;
            }

            auto const temp = path.string() + ".tmp." + std::to_string(static_cast<long>(::getpid()));
            {
                std::ofstream out(temp, std::ios::binary | std::ios::trunc);
                if (!out) {
                    return;
                }
                out << kCacheHeader << '\n';
                out << "root " << root.string() << '\n';
                out << "ctags " << (data.ctags ? 1 : 0) << '\n';
                out << "ctags_version " << data.ctags_version << '\n';
                for (auto const& file : data.files) {
                    out << "F\t" << file.path << '\t' << file.mtime << '\t' << file.size << '\n';
                }
                for (auto const& entry : data.entries) {
                    out << "S\t" << entry.name << '\t' << entry.path << '\t' << entry.line << '\t' << entry.end_line
                        << '\t' << entry.kind << '\t' << single_line(entry.signature) << '\n';
                }
            }
            std::filesystem::rename(temp, path, ec);
            if (ec) {
                std::filesystem::remove(temp, ec);
            }
        }

        [[nodiscard]] std::vector<symbol_entry> build_index(std::filesystem::path const& root) {
            auto const files     = cpp_files(root);
            auto const path      = cache_file(root);
            auto const cached    = read_cache(path, root);
            bool const has_ctags = process::tool_exists("ctags");
            auto const version   = has_ctags ? ctags_version() : std::string{};
            bool const reusable  = cached.has_value() && cached->ctags == has_ctags && cached->ctags_version == version;

            std::unordered_map<std::string, file_stamp> old_stamps;
            if (reusable) {
                for (auto const& file : cached->files) {
                    old_stamps[file.path] = file;
                }
            }

            std::vector<file_stamp>         stamps;
            std::vector<std::string>        changed;
            std::unordered_set<std::string> current;
            stamps.reserve(files.size());
            for (auto const& file : files) {
                auto const stamp = stamp_of(root, file);
                if (!stamp) {
                    continue;
                }
                stamps.push_back(*stamp);
                current.insert(file);
                auto const old = old_stamps.find(file);
                if (!reusable || old == old_stamps.end() || old->second.mtime != stamp->mtime ||
                    old->second.size != stamp->size)
                {
                    changed.push_back(file);
                }
            }

            std::vector<symbol_entry> entries;
            if (reusable) {
                std::unordered_set<std::string> changed_set(changed.begin(), changed.end());
                for (auto const& entry : cached->entries) {
                    if (current.contains(entry.path) && !changed_set.contains(entry.path)) {
                        entries.push_back(entry);
                    }
                }
            }

            std::vector<symbol_entry> scanner_entries;
            std::vector<symbol_entry> ctags_entries;
            if (!changed.empty()) {
                auto ctags_future = std::async(std::launch::async, [&] {
                    return has_ctags ? run_ctags(root, changed) : std::vector<symbol_entry>{};
                });
                scanner_entries   = scan_files(root, changed);
                ctags_entries     = ctags_future.get();
            }

            std::unordered_set<std::string> seen;
            std::vector<symbol_entry>       merged;
            merged.reserve(entries.size() + scanner_entries.size() + ctags_entries.size());
            append_unique(merged, seen, entries);
            append_unique(merged, seen, scanner_entries);
            append_unique(merged, seen, ctags_entries);

            std::ranges::sort(merged, [](symbol_entry const& left, symbol_entry const& right) {
                if (left.name != right.name) {
                    return left.name < right.name;
                }
                if (left.path != right.path) {
                    return left.path < right.path;
                }
                return left.line < right.line;
            });

            write_cache(path,
                        root,
                        cache_data{.files = stamps, .entries = merged, .ctags = has_ctags, .ctags_version = version});
            return merged;
        }

        struct index_data {
            std::filesystem::path     root;
            std::vector<symbol_entry> entries;
        };

        // The in-process memo: one repository per run is the normal case, so the
        // index is built (and its cache revalidated) once.
        std::optional<index_data> g_index;

        [[nodiscard]] bool is_keyword(std::string_view word) {
            static constexpr std::array<std::string_view, 60> kKeywords = {
              "alignas",  "alignof",  "and",      "auto",      "bool",      "break",    "case",      "catch",
              "char",     "class",    "const",    "constexpr", "continue",  "decltype", "default",   "delete",
              "do",       "double",   "else",     "enum",      "explicit",  "extern",   "false",     "float",
              "for",      "friend",   "if",       "inline",    "int",       "long",     "namespace", "new",
              "noexcept", "nullptr",  "operator", "private",   "protected", "public",   "return",    "short",
              "signed",   "sizeof",   "static",   "struct",    "switch",    "template", "this",      "throw",
              "true",     "try",      "typedef",  "typename",  "union",     "unsigned", "using",     "virtual",
              "void",     "volatile", "while",    "std",
            };
            return std::ranges::find(kKeywords, word) != kKeywords.end();
        }

        // The header target of an `#include` line, or "" for anything else.
        [[nodiscard]] std::string_view include_target(std::string_view line) {
            auto const pos = line.find("#include");
            if (pos == std::string_view::npos) {
                return {};
            }
            std::string_view rest = line.substr(pos + std::string_view("#include").size());
            while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) {
                rest.remove_prefix(1);
            }
            if (rest.size() < 2 || (rest.front() != '"' && rest.front() != '<')) {
                return {};
            }
            char const closer = rest.front() == '"' ? '"' : '>';
            auto const end    = rest.find(closer, 1);
            if (end == std::string_view::npos) {
                return {};
            }
            return rest.substr(1, end - 1);
        }

        [[nodiscard]] std::optional<std::string>
        resolve_include(std::filesystem::path const& root, std::string const& from, std::string_view target) {
            std::vector<std::filesystem::path> const bases{
              (root / from).parent_path(),
              root,
              root / "include",
              root / "src",
              root / "inc",
            };
            std::error_code ec;
            for (auto const& base : bases) {
                auto const candidate  = base / std::filesystem::path(target);
                auto const normalized = candidate.lexically_normal();
                if (std::filesystem::is_regular_file(normalized, ec)) {
                    auto const relative = normalized.lexically_relative(root);
                    if (!relative.empty()) {
                        return relative.generic_string();
                    }
                }
            }
            return std::nullopt;
        }

        // The project files reachable from `start` through `#include` lines, each
        // with its shortest include depth. Used to keep dependency resolution
        // inside the target's own header closure and to prefer nearby headers.
        [[nodiscard]] std::unordered_map<std::string, std::size_t> include_closure(
          std::filesystem::path const& root,
          std::string const&           start) {
            constexpr std::size_t kMaxFiles = 256;

            std::unordered_map<std::string, std::size_t>    closure;
            std::deque<std::pair<std::string, std::size_t>> queue;
            closure.emplace(start, 0);
            queue.emplace_back(start, 0);

            while (!queue.empty() && closure.size() < kMaxFiles) {
                auto [current, depth] = std::move(queue.front());
                queue.pop_front();

                std::string const content = read_text(root / current);
                if (content.empty()) {
                    continue;
                }
                for (auto const& include : compress::cpp::scan(content).includes) {
                    auto const target = include_target(include);
                    if (target.empty()) {
                        continue;
                    }
                    auto const resolved = resolve_include(root, current, target);
                    if (!resolved) {
                        continue;
                    }
                    if (closure.emplace(*resolved, depth + 1).second) {
                        queue.emplace_back(*resolved, depth + 1);
                    }
                }
            }
            return closure;
        }

        [[nodiscard]] std::string range_text(
          symbol_entry const&                           entry,
          std::filesystem::path const&                  root,
          std::unordered_map<std::string, std::string>& contents) {
            auto it = contents.find(entry.path);
            if (it == contents.end()) {
                it = contents.emplace(entry.path, read_text(root / entry.path)).first;
            }
            std::string const& content = it->second;
            if (content.empty()) {
                return {};
            }

            std::size_t const start_line = entry.line == 0 ? 1 : entry.line;
            std::size_t const end_line   = std::max(start_line, entry.end_line == 0 ? start_line : entry.end_line);

            std::string out;
            std::size_t line_no = 0;
            std::size_t start   = 0;
            while (start <= content.size()) {
                auto const       nl = content.find('\n', start);
                std::string_view line =
                  std::string_view(content).substr(start, nl == std::string::npos ? nl : nl - start);
                ++line_no;
                if (line_no >= start_line && line_no <= end_line) {
                    if (!out.empty()) {
                        out.push_back('\n');
                    }
                    out.append(line);
                }
                if (line_no > end_line || nl == std::string::npos) {
                    break;
                }
                start = nl + 1;
            }
            return out;
        }

        template <typename Fn>
        void for_each_identifier(std::string_view text, Fn&& fn) {
            std::string const code = compress::cpp::structural_view(text);
            std::size_t       i    = 0;
            while (i < code.size()) {
                if (!is_word_char(code[i])) {
                    ++i;
                    continue;
                }
                std::size_t const begin = i;
                while (i < code.size() && is_word_char(code[i])) {
                    ++i;
                }
                fn(std::string_view(code).substr(begin, i - begin));
            }
        }

        [[nodiscard]] std::unordered_map<std::string, std::vector<symbol_entry const*>> by_name(
          std::vector<symbol_entry> const& entries) {
            std::unordered_map<std::string, std::vector<symbol_entry const*>> map;
            for (auto const& entry : entries) {
                if (!entry.name.empty()) {
                    map[entry.name].push_back(&entry);
                }
            }
            return map;
        }

        [[nodiscard]] std::vector<std::string> reference_files(
          std::filesystem::path const&    root,
          std::string_view                name,
          std::vector<std::string> const& all_files) {
            std::vector<std::string> files;
            if (git::is_inside_worktree(root.string())) {
                std::vector<std::string>
                  argv{"git", "-C", root.string(), "grep", "-l", "-w", "--untracked", "-e", std::string(name), "--"};
                for (auto const glob : kCppGlobs) {
                    argv.emplace_back(glob);
                }
                auto const result = process::command{argv}.capture_stderr().run();
                if (result.exit_code == 0) {
                    std::size_t start = 0;
                    while (start < result.stdout_data.size()) {
                        auto const nl = result.stdout_data.find('\n', start);
                        if (nl > start) {
                            files.emplace_back(result.stdout_data.substr(start, nl - start));
                        }
                        start = nl == std::string::npos ? result.stdout_data.size() : nl + 1;
                    }
                    return files;
                }
                if (result.exit_code == 1) {
                    return files; // no matches
                }
            }
            return all_files;
        }

        [[nodiscard]] std::vector<symbol_entry> callers_in_file(
          std::vector<symbol_entry const*> const& entries,
          std::string const&                      file,
          std::string_view                        name,
          std::filesystem::path const&            root,
          symbol_entry const&                     target) {
            std::string const content = read_text(root / file);
            if (content.empty()) {
                return {};
            }
            std::string const code = compress::cpp::structural_view(content);

            std::vector<symbol_entry>       result;
            std::unordered_set<std::string> seen;
            std::size_t                     line_no = 0;
            std::size_t                     start   = 0;
            while (start <= code.size()) {
                auto const       nl   = code.find('\n', start);
                std::string_view line = std::string_view(code).substr(start, nl == std::string::npos ? nl : nl - start);
                ++line_no;

                for (std::size_t pos = line.find(name); pos != std::string_view::npos; pos = line.find(name, pos + 1)) {
                    bool const left  = pos == 0 || !is_word_char(line[pos - 1]);
                    bool const right = pos + name.size() >= line.size() || !is_word_char(line[pos + name.size()]);
                    if (!left || !right) {
                        continue;
                    }

                    symbol_entry const* enclosing = nullptr;
                    for (auto const* entry : entries) {
                        if (entry->line == 0) {
                            continue;
                        }
                        std::size_t const end = entry->end_line == 0 ? entry->line : entry->end_line;
                        if (entry->line <= line_no && line_no <= end &&
                            (enclosing == nullptr || entry->line > enclosing->line))
                        {
                            enclosing = entry;
                        }
                    }
                    if (enclosing == nullptr || entry_key(*enclosing) == entry_key(target)) {
                        continue;
                    }
                    if (seen.insert(entry_key(*enclosing)).second) {
                        result.push_back(*enclosing);
                    }
                }

                if (nl == std::string::npos) {
                    break;
                }
                start = nl + 1;
            }
            return result;
        }

    } // namespace

    std::vector<symbol_entry> const& symbol_index(std::filesystem::path const& root) {
        auto const canonical = canonical_root(root);
        if (!g_index || g_index->root != canonical) {
            g_index = index_data{.root = canonical, .entries = build_index(canonical)};
        }
        return g_index->entries;
    }

    void reset_symbol_index() {
        g_index.reset();
    }

    std::optional<symbol_entry> find_symbol(std::string_view name, std::filesystem::path const& root) {
        std::optional<symbol_entry> best;
        for (auto const& entry : symbol_index(root)) {
            if (entry.name != name) {
                continue;
            }
            // Prefer the definition (the widest range) over a header declaration,
            // so dependency extraction sees the body.
            auto const span      = entry.end_line > entry.line ? entry.end_line - entry.line : 0;
            auto const best_span = best && best->end_line > best->line ? best->end_line - best->line : 0;
            if (!best || span > best_span) {
                best = entry;
            }
        }
        return best;
    }

    std::vector<symbol_entry> find_symbols(std::string_view name, std::filesystem::path const& root) {
        std::vector<symbol_entry> matches;
        for (auto const& entry : symbol_index(root)) {
            if (entry.name == name) {
                matches.push_back(entry);
            }
        }
        return matches;
    }

    void complete_symbols(
      std::string_view             prefix,
      text::string_views           prior,
      std::filesystem::path const& root,
      std::ostream&                out) {
        if (prefix.empty() || prefix.front() == '-') {
            return;
        }

        std::unordered_set<std::string_view> typed;
        for (auto const arg : prior) {
            if (!arg.empty() && arg.front() != '+' && arg.front() != '-') {
                typed.insert(arg);
            }
        }

        struct candidate {
            symbol_entry const* entry = nullptr;
            fuzzy::rank         rank{};
        };

        std::unordered_map<std::string, candidate> best;
        for (auto const& entry : symbol_index(root)) {
            if (entry.name.empty() || typed.contains(entry.name)) {
                continue;
            }
            auto const rank = fuzzy::rank_path(entry.name, prefix);
            if (!rank) {
                continue;
            }
            auto const it = best.find(entry.name);
            if (it == best.end() || fuzzy::rank_key(*rank) < fuzzy::rank_key(it->second.rank) ||
                (fuzzy::rank_key(*rank) == fuzzy::rank_key(it->second.rank) && entry.path < it->second.entry->path))
            {
                best[entry.name] = candidate{.entry = &entry, .rank = *rank};
            }
        }

        std::vector<std::pair<std::string, candidate>> matches;
        matches.reserve(best.size());
        for (auto const& [name, value] : best) {
            matches.emplace_back(name, value);
        }
        std::ranges::sort(matches, [](auto const& left, auto const& right) {
            if (fuzzy::rank_key(left.second.rank) != fuzzy::rank_key(right.second.rank)) {
                return fuzzy::rank_key(left.second.rank) < fuzzy::rank_key(right.second.rank);
            }
            return left.first < right.first;
        });
        if (matches.size() > kMaxCandidates) {
            matches.resize(kMaxCandidates);
        }
        for (auto const& [name, value] : matches) {
            out << name << '\t' << value.entry->path << ':' << value.entry->line << '\n';
        }
    }

    std::vector<symbol_entry>
    dependencies(symbol_entry const& target, std::filesystem::path const& root, dependency_options const& options) {
        if (options.max_symbols == 0) {
            return {};
        }

        auto const                                   entries = symbol_index(root);
        auto const                                   names   = by_name(entries);
        std::unordered_map<std::string, std::string> contents;
        auto const                                   closure    = include_closure(root, target.path);
        auto const                                   target_dir = std::filesystem::path(target.path).parent_path();

        std::vector<symbol_entry>                        result;
        std::unordered_set<std::string>                  visited;
        std::deque<std::pair<symbol_entry, std::size_t>> queue;
        visited.insert(entry_key(target));
        queue.emplace_back(target, 0);

        while (!queue.empty()) {
            auto [current, depth] = queue.front();
            queue.pop_front();

            std::string const text = range_text(current, root, contents);
            if (text.empty()) {
                continue;
            }

            for_each_identifier(text, [&](std::string_view identifier) {
                if (result.size() >= options.max_symbols || identifier.size() < 2 || is_keyword(identifier)) {
                    return;
                }
                auto const found = names.find(std::string(identifier));
                if (found == names.end()) {
                    return;
                }
                // One entry per name: prefer a nearby header, then the definition
                // (widest range) over a declaration, so the dependency carries a
                // body and comes from the target's own include neighborhood.
                symbol_entry const* chosen = nullptr;
                for (auto const* dependency : found->second) {
                    // The definition's own name is in its range; skip every
                    // declaration of the target, not just the exact entry.
                    if (dependency->name == target.name || !closure.contains(dependency->path)) {
                        continue;
                    }
                    if (chosen == nullptr) {
                        chosen = dependency;
                        continue;
                    }

                    auto const include_depth = closure.at(dependency->path);
                    auto const best_depth    = closure.at(chosen->path);
                    if (include_depth != best_depth) {
                        if (include_depth < best_depth) {
                            chosen = dependency;
                        }
                        continue;
                    }

                    bool const same_dir      = std::filesystem::path(dependency->path).parent_path() == target_dir;
                    bool const best_same_dir = std::filesystem::path(chosen->path).parent_path() == target_dir;
                    if (same_dir != best_same_dir) {
                        if (same_dir) {
                            chosen = dependency;
                        }
                        continue;
                    }

                    auto const span =
                      dependency->end_line > dependency->line ? dependency->end_line - dependency->line : 0;
                    auto const best_span = chosen->end_line > chosen->line ? chosen->end_line - chosen->line : 0;
                    if (span > best_span) {
                        chosen = dependency;
                    }
                }
                if (chosen == nullptr) {
                    return;
                }
                auto const key = entry_key(*chosen);
                if (!visited.insert(key).second) {
                    return;
                }
                result.push_back(*chosen);
                if (options.transitive && depth + 1 < options.max_depth) {
                    queue.emplace_back(*chosen, depth + 1);
                }
            });
        }
        return result;
    }

    std::vector<symbol_entry>
    callers(symbol_entry const& target, std::filesystem::path const& root, dependency_options const& options) {
        if (options.max_symbols == 0) {
            return {};
        }

        auto const                                                        entries = symbol_index(root);
        std::vector<std::string>                                          all_files;
        std::unordered_set<std::string>                                   seen_files;
        std::unordered_map<std::string, std::vector<symbol_entry const*>> by_file;
        all_files.reserve(entries.size());
        for (auto const& entry : entries) {
            by_file[entry.path].push_back(&entry);
            if (seen_files.insert(entry.path).second) {
                all_files.push_back(entry.path);
            }
        }

        std::vector<symbol_entry>                        result;
        std::unordered_set<std::string>                  visited;
        std::deque<std::pair<symbol_entry, std::size_t>> queue;
        visited.insert(entry_key(target));
        queue.emplace_back(target, 0);

        while (!queue.empty()) {
            auto [current, depth] = queue.front();
            queue.pop_front();

            auto const files = reference_files(root, current.name, all_files);
            for (auto const& file : files) {
                auto const in_file = by_file.find(file);
                if (in_file == by_file.end()) {
                    continue;
                }
                auto found = callers_in_file(in_file->second, file, current.name, root, target);
                for (auto& caller : found) {
                    if (result.size() >= options.max_symbols) {
                        return result;
                    }
                    auto const key = entry_key(caller);
                    if (!visited.insert(key).second) {
                        continue;
                    }
                    result.push_back(caller);
                    if (options.transitive && depth + 1 < options.max_depth) {
                        queue.emplace_back(caller, depth + 1);
                    }
                }
            }
        }
        return result;
    }

} // namespace dana::tools
