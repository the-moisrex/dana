#include "embed.hpp"

#include <climits>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

    // Packs up to 8 bytes into a little-endian integer so extensions can be switched on.
    constexpr std::uint64_t pack(std::string_view const src) noexcept try {
        std::uint64_t key = 0;
        for (std::size_t i = 0; i < src.size(); ++i) {
            key |= static_cast<std::uint64_t>(static_cast<unsigned char>(src.at(i))) << (CHAR_BIT * i);
        }
        return key;
    } catch (...) {
        return std::numeric_limits<std::uint64_t>::max();
    }

    // `ext` is without the leading dot. Returns "" when unknown.
    constexpr std::string_view lang_from_extension(std::string_view const ext) noexcept {
        if (ext.size() > std::size("dockerfile")) {
            return ext == "dockerfile" ? "dockerfile" : "";
        }

        switch (pack(ext)) {
            case pack("c"):
            case pack("h"): return "c";
            case pack("cc"):
            case pack("cp"):
            case pack("cpp"):
            case pack("cxx"):
            case pack("c++"):
            case pack("hpp"):
            case pack("hxx"):
            case pack("hh"):
            case pack("h++"):
            case pack("ixx"): return "cpp";
            case pack("rs"): return "rust";
            case pack("py"):
            case pack("pyi"): return "python";
            case pack("sh"):
            case pack("bash"): return "bash";
            case pack("js"):
            case pack("cjs"):
            case pack("mjs"): return "javascript";
            case pack("ts"):
            case pack("mts"):
            case pack("cts"): return "typescript";
            case pack("jsx"): return "jsx";
            case pack("tsx"): return "tsx";
            case pack("java"): return "java";
            case pack("kt"):
            case pack("kts"): return "kotlin";
            case pack("go"): return "go";
            case pack("rb"): return "ruby";
            case pack("lua"): return "lua";
            case pack("json"):
            case pack("stock"):
            case pack("tse"): return "json";
            case pack("jsonc"): return "jsonc";
            case pack("yaml"):
            case pack("yml"): return "yaml";
            case pack("toml"): return "toml";
            case pack("md"): return "markdown";
            case pack("txt"):
            case pack("log"): return "text";
            case pack("sql"): return "sql";
            case pack("csv"): return "csv";
            case pack("tsv"): return "tsv";
            case pack("html"):
            case pack("htm"): return "html";
            case pack("css"): return "css";
            case pack("xml"): return "xml";
            case pack("svg"): return "svg";
            case pack("ini"):
            case pack("cfg"):
            case pack("conf"): return "ini";
            case pack("diff"):
            case pack("patch"): return "diff";
            default: return "";
        }
    }

    // Whole-filename matches (no extension involved). Returns "" when unknown.
    constexpr std::string_view lang_from_filename(std::string_view const name) noexcept {
        if (name == "Dockerfile") {
            return "dockerfile";
        }
        if (name == "Makefile" || name == "makefile" || name == "GNUmakefile") {
            return "makefile";
        }
        if (name == "CMakeLists.txt") {
            return "cmake";
        }
        return "";
    }

} // namespace

namespace dana {

    // Infers the language id from a file name. Returns "text" if unknown.
    std::string_view infer_lang(std::string_view const name) noexcept try {
        if (auto const lang = lang_from_filename(name); !lang.empty()) {
            return lang;
        }
        if (auto const dot = name.rfind('.'); dot != std::string_view::npos && dot + 1 < name.size()) {
            if (auto const lang = lang_from_extension(name.substr(dot + 1)); !lang.empty()) {
                return lang;
            }
        }
        return "text";
    } catch (...) {
        return "text";
    }

    // Path overload. Note: filename().string() may allocate; prefer the string_view overload in hot loops.
    std::string_view infer_lang(std::filesystem::path const& file) {
        auto const str = file.filename().string();
        return infer_lang(std::string_view{str.data(), str.size()});
    }

    std::string trim_context(std::string_view content, std::optional<std::size_t> head_lines) {
        if (!head_lines) {
            return std::string(content);
        }

        // sed's `1,0p` prints line 1, so a 0 cap keeps a single line.
        std::size_t const limit = *head_lines == 0 ? 1 : *head_lines;
        std::size_t       lines = 0;
        std::size_t       pos   = 0;
        while (pos < content.size() && lines < limit) {
            pos = content.find('\n', pos);
            if (pos == std::string_view::npos) {
                break;
            }
            pos++;
            lines++;
        }
        return std::string(content.substr(0, pos));
    }

    std::string trim_context_nl(std::string_view content, std::optional<std::size_t> head_lines) {
        while (!content.empty() && content.back() == '\n') {
            content.remove_suffix(1);
        }
        if (!head_lines) {
            return std::string(content) + "\n";
        }

        std::size_t limit = *head_lines == 0 ? 1 : *head_lines;
        std::size_t lines = 0;
        std::size_t pos   = 0;
        while (pos < content.size() && lines < limit) {
            pos = content.find('\n', pos);
            if (pos == std::string_view::npos) {
                break;
            }
            ++pos;
            ++lines;
        }
        if (lines == limit) {
            return std::string(content.substr(0, pos));
        }
        return std::string(content) + "\n";
    }

    std::string head_output(std::string_view text, std::size_t n) {
        if (text.empty()) {
            return {};
        }

        std::vector<std::string_view> lines;
        std::size_t                   start = 0;
        while (start <= text.size()) {
            std::size_t end = text.find('\n', start);
            if (end == std::string_view::npos) {
                lines.push_back(text.substr(start));
                break;
            }
            lines.push_back(text.substr(start, end - start));
            start = end + 1;
            if (start == text.size()) {
                break;
            }
        }
        if (lines.size() <= n) {
            return std::string(text);
        }

        std::string out;
        for (std::size_t i = 0; i < n; ++i) {
            if (i != 0U) {
                out += '\n';
            }
            out += lines[i];
        }
        return out;
    }

    std::string read_file(std::filesystem::path const& path) {
        // A directory opens successfully but throws from the stream on read
        // (`basic_filebuf::underflow: Is a directory`), so gate on a regular file.
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) {
            return "";
        }
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return "";
        }
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    void append_numbered_line(std::string& output, std::size_t line_number, std::string_view line) {
        output += std::format("{}: {}\n", line_number, line);
    }

    std::string number_lines(std::string_view text, bool numbered) {
        if (!numbered || text.empty()) {
            return std::string(text);
        }

        bool const ends_with_newline = text.back() == '\n';

        std::vector<std::string_view> lines;
        std::size_t                   start = 0;
        while (start < text.size()) {
            std::size_t const end = text.find('\n', start);
            if (end == std::string_view::npos) {
                lines.push_back(text.substr(start));
                break;
            }
            lines.push_back(text.substr(start, end - start));
            start = end + 1;
        }
        if (lines.empty()) {
            return std::string(text);
        }

        std::size_t width = std::to_string(lines.size()).size();
        std::string out;
        out.reserve(text.size() + (lines.size() * (width + 3)));
        for (std::size_t i = 0; i < lines.size(); ++i) {
            std::string number = std::to_string(i + 1);
            out.append(width - number.size(), ' ');
            out += number;
            out += " | ";
            out += lines[i];
            out += '\n';
        }
        if (!ends_with_newline) {
            out.pop_back(); // the input had no final newline
        }
        return out;
    }

    std::string embed_file(std::filesystem::path const& path, std::string_view label, file_options opts) {
        if (!std::filesystem::exists(path)) {
            return "";
        }

        std::string name    = label.empty() ? path.filename().string() : std::string(label);
        std::string content = read_file(path);
        if (content.empty()) {
            return "";
        }

        std::string output;
        output += std::format("File: {}\n\n", name);
        output += std::format("```{}\n", infer_lang(path));
        output += number_lines(trim_context(content, opts.head_lines), opts.numbered);
        output += "\n```\n\n";
        return output;
    }

    std::string embed_stdin(std::string_view content, std::optional<std::size_t> head_lines) {
        (void) head_lines; // bash's embed_stdin does not apply --head
        while (!content.empty() && content.back() == '\n') {
            content.remove_suffix(1);
        }
        return std::string(content) + "\n\n";
    }

    std::string embed_file_block(std::filesystem::path const& file, file_options opts) {
        std::string output;
        output += std::format("File: {}\n\n", file.filename().string());
        output += std::format("```{}\n", infer_lang(file));
        output += number_lines(trim_context_nl(read_file(file), opts.head_lines), opts.numbered);
        output += "```\n\n";
        return output;
    }

    void warn_missing_file(std::string& errors, std::string_view name) {
        errors += "Warning: File '";
        errors += name;
        errors += "' not found.\n";
    }

} // namespace dana
