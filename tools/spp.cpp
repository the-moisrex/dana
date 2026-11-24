#include "spp.hpp"

#include "../core/fs.hpp"
#include "../core/process.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <unordered_set>
#include <vector>

namespace dana::tools {

    namespace {

        constexpr std::string_view c_extensions[]   = {".c", ".h"};
        constexpr std::string_view cxx_extensions[] = {".cc", ".cpp", ".cxx", ".c++", ".hpp", ".hh", ".hxx", ".ixx"};
        constexpr std::string_view c_patterns[]     = {"*.c", "*.h"};
        constexpr std::string_view cxx_patterns[] =
          {"*.cpp", "*.cxx", "*.ixx", "*.cc", "*.c++", "*.h", "*.hpp", "*.hh", "*.hxx"};

        [[nodiscard]] bool is_space(char const c) noexcept {
            return std::isspace(static_cast<unsigned char>(c)) != 0;
        }

        [[nodiscard]] std::string_view trim(std::string_view text) noexcept {
            while (!text.empty() && is_space(text.front())) {
                text.remove_prefix(1);
            }
            while (!text.empty() && is_space(text.back())) {
                text.remove_suffix(1);
            }
            return text;
        }

        [[nodiscard]] std::string lower(std::string_view text) {
            std::string out(text);
            for (char& c : out) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return out;
        }

        [[nodiscard]] bool in_list(std::string_view value, auto const& list) noexcept {
            for (auto const entry : list) {
                if (value == entry) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] bool is_header(std::string_view path) noexcept {
            auto const ext = lower(std::filesystem::path(path).extension().string());
            return ext == ".h" || ext == ".hpp" || ext == ".hh" || ext == ".hxx" || ext == ".ixx";
        }

        [[nodiscard]] std::string detect_language(std::string_view path, bool c_as_cxx, bool cxx_as_c) {
            auto const ext = lower(std::filesystem::path(path).extension().string());
            if (c_as_cxx && in_list(ext, c_extensions)) {
                return "c++";
            }
            if (cxx_as_c && in_list(ext, cxx_extensions)) {
                return "c";
            }
            if (in_list(ext, cxx_extensions)) {
                return "c++";
            }
            if (in_list(ext, c_extensions)) {
                return "c";
            }
            return "c++";
        }

        [[nodiscard]] std::string language_std(std::string_view language) {
            return language == "c++" ? "-std=c++26" : "-std=c23";
        }

        // A small POSIX shlex: whitespace splits, single/double quotes group, and
        // backslash escapes the next character (outside single quotes).
        [[nodiscard]] std::vector<std::string> shlex(std::string_view line) {
            std::vector<std::string> tokens;
            std::string              current;
            bool                     have_token = false;
            char                     quote      = '\0';
            for (std::size_t i = 0; i < line.size(); ++i) {
                char const c = line[i];
                if (quote == '\0') {
                    if (is_space(c)) {
                        if (have_token) {
                            tokens.push_back(std::move(current));
                            current.clear();
                            have_token = false;
                        }
                        continue;
                    }
                    if (c == '\'' || c == '"') {
                        quote      = c;
                        have_token = true;
                        continue;
                    }
                    if (c == '\\' && i + 1 < line.size()) {
                        current.push_back(line[++i]);
                        have_token = true;
                        continue;
                    }
                    current.push_back(c);
                    have_token = true;
                } else if (c == quote) {
                    quote = '\0';
                } else if (c == '\\' && quote == '"' && i + 1 < line.size()) {
                    current.push_back(line[++i]);
                } else {
                    current.push_back(c);
                }
            }
            if (have_token) {
                tokens.push_back(std::move(current));
            }
            return tokens;
        }

        [[nodiscard]] bool keep_flag(std::string_view token) noexcept {
            return token.starts_with('-') && !token.starts_with("--header-insertion") && !token.starts_with("--limit");
        }

        [[nodiscard]] std::string strip_chars(std::string_view text, std::string_view chars) {
            while (!text.empty() && chars.find(text.front()) != std::string_view::npos) {
                text.remove_prefix(1);
            }
            while (!text.empty() && chars.find(text.back()) != std::string_view::npos) {
                text.remove_suffix(1);
            }
            return std::string(text);
        }

        void read_flags_file(std::filesystem::path const& path, std::vector<std::string>& flags) {
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                return;
            }
            std::string line;
            while (std::getline(in, line)) {
                std::string_view const trimmed = trim(line);
                if (trimmed.empty() || trimmed.starts_with('#')) {
                    continue;
                }
                if (auto const colon = trimmed.find(':'); colon != std::string_view::npos) {
                    std::string_view key = trim(trimmed.substr(0, colon));
                    while (key.starts_with('-')) {
                        key.remove_prefix(1);
                    }
                    if (key == "Add" || key == "Remove" || key == "CompileFlags" || key == "Flags") {
                        std::string const body_storage = strip_chars(trim(trimmed.substr(colon + 1)), "[]");
                        std::string_view  body         = body_storage;
                        std::size_t       start        = 0;
                        while (start <= body.size()) {
                            auto const       comma = body.find(',', start);
                            std::string_view token =
                              trim(body.substr(start, comma == std::string_view::npos ? comma : comma - start));
                            if (keep_flag(token)) {
                                flags.emplace_back(token);
                            }
                            if (comma == std::string_view::npos) {
                                break;
                            }
                            start = comma + 1;
                        }
                        continue;
                    }
                }
                for (auto const& raw : shlex(trimmed)) {
                    auto const token = strip_chars(raw, ",[]");
                    if (token.empty() || token == "-") {
                        continue;
                    }
                    if (keep_flag(token)) {
                        flags.push_back(token);
                    }
                }
            }
        }

        [[nodiscard]] std::vector<std::string> read_clang_flags() {
            std::vector<std::string> flags;
            auto const               root = fs::find_git_root();
            if (!root) {
                return flags;
            }
            read_flags_file(*root / ".clang", flags);
            read_flags_file(*root / ".clangd", flags);
            return flags;
        }

        [[nodiscard]] std::vector<std::string>
        grep_files(std::string_view function, std::string_view language, bool c_as_cxx, bool cxx_as_c) {
            std::vector<std::string_view> patterns;
            if (language == "c") {
                patterns.insert(patterns.end(), std::begin(c_patterns), std::end(c_patterns));
                if (cxx_as_c) {
                    patterns.insert(patterns.end(), std::begin(cxx_patterns), std::end(cxx_patterns));
                }
            } else if (language == "c++") {
                patterns.insert(patterns.end(), std::begin(cxx_patterns), std::end(cxx_patterns));
                if (c_as_cxx) {
                    patterns.insert(patterns.end(), std::begin(c_patterns), std::end(c_patterns));
                }
            } else {
                patterns.insert(patterns.end(), std::begin(c_patterns), std::end(c_patterns));
                patterns.insert(patterns.end(), std::begin(cxx_patterns), std::end(cxx_patterns));
            }

            std::vector<std::string>        argv{"git", "grep", "-l", std::string(function), "--"};
            std::unordered_set<std::string> seen;
            for (auto const pattern : patterns) {
                if (seen.insert(std::string(pattern)).second) {
                    argv.emplace_back(pattern);
                }
            }

            auto const               result = process::command{argv}.capture_stderr().run();
            std::vector<std::string> files;
            std::size_t              start = 0;
            while (start < result.stdout_data.size()) {
                auto const end  = result.stdout_data.find('\n', start);
                auto const stop = end == std::string::npos ? result.stdout_data.size() : end;
                if (stop > start) {
                    files.emplace_back(result.stdout_data.substr(start, stop - start));
                }
                start = end == std::string::npos ? result.stdout_data.size() : end + 1;
            }
            return files;
        }

        struct temp_file_guard {
            std::filesystem::path path;
            temp_file_guard()                                  = default;
            temp_file_guard(temp_file_guard const&)            = delete;
            temp_file_guard& operator=(temp_file_guard const&) = delete;

            ~temp_file_guard() {
                if (!path.empty()) {
                    std::error_code ec;
                    std::filesystem::remove(path, ec);
                }
            }
        };

        [[nodiscard]] std::filesystem::path make_temp_tu(std::filesystem::path const& header,
                                                         std::string_view             language) {
            static unsigned counter = 0;
            auto const      suffix  = language == "c++" ? ".cpp" : ".c";
            auto const      path = std::filesystem::temp_directory_path() /
                                   std::format("prompt-spp-{}-{}{}", static_cast<long>(::getpid()), ++counter, suffix);
            std::ofstream   out(path, std::ios::binary);
            out << "#include \"" << std::filesystem::absolute(header).string() << "\"\n";
            return path;
        }

        [[nodiscard]] std::string extract_file(
          std::string_view                function,
          std::string const&              file,
          spp_options const&              options,
          std::vector<std::string> const& flags) {
            std::error_code ec;
            if (!std::filesystem::exists(file, ec)) {
                return {};
            }

            std::string const language =
              options.language.value_or(detect_language(file, options.c_as_cxx, options.cxx_as_c));
            std::string const standard = language_std(language);

            std::optional<temp_file_guard> guard;
            std::string                    target = file;
            if (is_header(file)) {
                guard.emplace();
                guard->path = make_temp_tu(file, language);
                target      = guard->path.string();
            }

            std::vector<std::string> argv{
              "clang",
              "-x",
              language,
              "-Xclang",
              "-ast-dump-filter=" + std::string(function),
              "-fsyntax-only",
              "-Xclang",
              "-ast-print",
              "-fno-color-diagnostics",
              "-fno-caret-diagnostics",
              "-fno-delayed-template-parsing",
              target,
              standard};
            for (auto const& flag : flags) {
                bool const is_std     = flag.starts_with("-std=");
                bool const is_cxx_std = flag.contains("++");
                if (is_std && ((language == "c" && is_cxx_std) || (language == "c++" && !is_cxx_std))) {
                    continue;
                }
                argv.push_back(flag);
            }
            for (auto const& dir : options.include_dirs) {
                argv.emplace_back("-I");
                argv.push_back(dir);
            }

            auto const result = process::command{argv}.capture_stderr().run();
            if (trim(result.stdout_data).empty()) {
                return {};
            }

            std::string output;
            std::size_t start = 0;
            while (start <= result.stdout_data.size()) {
                auto const       nl = result.stdout_data.find('\n', start);
                std::string_view line =
                  std::string_view(result.stdout_data).substr(start, nl == std::string::npos ? nl : nl - start);
                if (!line.starts_with("Printing ")) {
                    if (!output.empty()) {
                        output.push_back('\n');
                    }
                    output.append(line);
                }
                if (nl == std::string::npos) {
                    break;
                }
                start = nl + 1;
            }
            return output;
        }

        void add_blocks(std::string_view                 text,
                        std::vector<std::string>&        blocks,
                        std::unordered_set<std::string>& seen) {
            std::size_t start = 0;
            while (start <= text.size()) {
                auto const       sep   = text.find("\n\n", start);
                std::string_view block = text.substr(start, sep == std::string_view::npos ? sep : sep - start);
                block                  = trim(block);
                if (!block.empty()) {
                    std::string candidate(block);
                    if (seen.insert(candidate).second) {
                        blocks.push_back(std::move(candidate));
                    }
                }
                if (sep == std::string_view::npos) {
                    break;
                }
                start = sep + 2;
            }
        }

    } // namespace

    std::string expand_symbol(std::string_view function, spp_options const& options) {
        std::vector<std::string> files = options.files;
        if (files.empty()) {
            files = grep_files(function, options.language.value_or(""), options.c_as_cxx, options.cxx_as_c);
        }

        auto const flags = read_clang_flags();

        std::vector<std::string>        blocks;
        std::unordered_set<std::string> seen;
        for (auto const& file : files) {
            add_blocks(extract_file(function, file, options, flags), blocks, seen);
        }

        if (blocks.empty()) {
            return {};
        }
        std::string out;
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            if (i != 0) {
                out += "\n\n";
            }
            out += blocks[i];
        }
        out.push_back('\n');
        return out;
    }

} // namespace dana::tools
