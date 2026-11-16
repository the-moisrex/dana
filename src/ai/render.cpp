#include "render.hpp"

#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace prompt::ai {

namespace {

constexpr std::string_view kReset = "\x1b[0m";
constexpr std::string_view kDim = "\x1b[2m";
constexpr std::string_view kBold = "\x1b[1m";
constexpr std::string_view kItalic = "\x1b[3m";
constexpr std::string_view kUnderline = "\x1b[4m";
constexpr std::string_view kStrike = "\x1b[9m";
constexpr std::string_view kCyan = "\x1b[36m";
constexpr std::string_view kBlue = "\x1b[34m";

bool stdout_is_tty() { return ::isatty(STDOUT_FILENO) != 0; }
bool stderr_is_tty() { return ::isatty(STDERR_FILENO) != 0; }

bool starts_with(std::string_view text, std::string_view prefix) { return text.starts_with(prefix); }

std::string_view trim_left(std::string_view text) {
    std::size_t index = 0;
    while (index < text.size() && (text[index] == ' ' || text[index] == '\t')) ++index;
    return text.substr(index);
}

bool is_alnum(char value) { return std::isalnum(static_cast<unsigned char>(value)) != 0; }
bool is_space(char value) { return std::isspace(static_cast<unsigned char>(value)) != 0; }

enum class inline_style : unsigned char { bold, italic, strike };

std::string_view code_for(inline_style style) {
    switch (style) {
    case inline_style::bold:
        return kBold;
    case inline_style::italic:
        return kItalic;
    case inline_style::strike:
        return kStrike;
    }
    return {};
}

void append_styles(std::string& out, std::vector<inline_style> const& stack) {
    for (auto style : stack) out.append(code_for(style));
}

// Inline Markdown (emphasis, code, links) with a style stack so a nested span
// restores whatever was active before it and every span closes with a reset.
std::string style_inline(std::string_view text) {
    std::string out;
    std::vector<inline_style> stack;
    auto reapply = [&] { append_styles(out, stack); };
    std::size_t index = 0;
    while (index < text.size()) {
        char const current = text[index];
        if (current == '\\' && index + 1 < text.size()) {
            out += text[index + 1];
            index += 2;
            continue;
        }
        if (starts_with(text.substr(index), "**") || starts_with(text.substr(index), "__")) {
            if (!stack.empty() && stack.back() == inline_style::bold) {
                stack.pop_back();
                out += kReset;
                reapply();
            } else {
                stack.push_back(inline_style::bold);
                out += kBold;
            }
            index += 2;
            continue;
        }
        if (starts_with(text.substr(index), "~~")) {
            if (!stack.empty() && stack.back() == inline_style::strike) {
                stack.pop_back();
                out += kReset;
                reapply();
            } else {
                stack.push_back(inline_style::strike);
                out += kStrike;
            }
            index += 2;
            continue;
        }
        if (current == '`') {
            auto const close = text.find('`', index + 1);
            if (close != std::string_view::npos) {
                out += kCyan;
                out.append(text.substr(index + 1, close - index - 1));
                out += kReset;
                reapply();
                index = close + 1;
                continue;
            }
        }
        if (current == '*' || current == '_') {
            bool const closing = !stack.empty() && stack.back() == inline_style::italic;
            bool allowed = true;
            if (current == '_') {
                allowed = closing ? (index + 1 >= text.size() || !is_alnum(text[index + 1]))
                                  : (index == 0 || !is_alnum(text[index - 1]));
            }
            if (allowed) {
                if (closing) {
                    stack.pop_back();
                    out += kReset;
                    reapply();
                } else {
                    stack.push_back(inline_style::italic);
                    out += kItalic;
                }
                ++index;
                continue;
            }
        }
        if (current == '[') {
            auto const label_end = text.find("](", index + 1);
            if (label_end != std::string_view::npos) {
                auto const url_end = text.find(')', label_end + 2);
                if (url_end != std::string_view::npos) {
                    auto const label = text.substr(index + 1, label_end - index - 1);
                    auto const url = text.substr(label_end + 2, url_end - label_end - 2);
                    out += "\x1b]8;;";
                    out.append(url);
                    out += "\x1b\\";
                    out += kUnderline;
                    out += kBlue;
                    out.append(label);
                    out += kReset;
                    reapply();
                    out += "\x1b]8;;\x1b\\";
                    index = url_end + 1;
                    continue;
                }
            }
        }
        out += current;
        ++index;
    }
    if (!stack.empty()) out += kReset;
    return out;
}

} // namespace

renderer::renderer(render_options const& options)
    : renderer(
          options,
          [](std::string_view chunk) {
              std::cout.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
              std::cout.flush();
          },
          [](std::string_view chunk) {
              std::cerr.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
              std::cerr.flush();
          }) {}

renderer::renderer(render_options const& options, writer out, writer err)
    : options_(options), out_(std::move(out)), err_(std::move(err)) {
    tty_ = options_.force_tty || stdout_is_tty();
    err_tty_ = options_.force_tty || stderr_is_tty();
    if (std::getenv("NO_COLOR") != nullptr) options_.color = false;
    options_.color = options_.color && tty_;
}

void renderer::write_out(std::string_view chunk) {
    if (out_) out_(chunk);
}

void renderer::write_err(std::string_view chunk) {
    if (err_) err_(chunk);
}

void renderer::handle(stream_event const& event) {
    switch (event.kind) {
    case event_kind::text:
        text_ += event.text;
        emit_text(event.text);
        break;
    case event_kind::thinking:
        thinking_ += event.text;
        emit_thinking(event.text);
        break;
    case event_kind::tool_call:
        emit_tool(event);
        break;
    case event_kind::usage:
        if (event.data.is_object()) {
            bool first = true;
            for (auto const& [key, value] : event.data.items()) {
                if (value.is_null()) continue;
                if (!first) usage_ += ", ";
                first = false;
                usage_ += key + "=" + (value.is_string() ? value.get<std::string>() : value.dump());
            }
        }
        break;
    case event_kind::error:
        error_ = event.text;
        if (thinking_dim_open_) {
            write_err(kReset);
            thinking_dim_open_ = false;
        }
        write_err(std::string(options_.color ? kDim : "") + "error: " + std::string(options_.color ? kReset : "") +
                  event.text + "\n");
        break;
    case event_kind::done:
    case event_kind::raw:
        break;
    }
}

void renderer::emit_text(std::string_view chunk) {
    if (!tty_ || options_.raw) {
        write_out(chunk);
        return;
    }
    if (!started_answer_) {
        started_answer_ = true;
        if (emitted_thinking_) {
            // The reasoning went to stderr; close its dim span, move past it
            // and leave one blank line before the answer, the way the rich
            // live region does.
            if (thinking_dim_open_) {
                write_err(kReset);
                thinking_dim_open_ = false;
            }
            write_err(thinking_line_open_ ? "\n\n" : "\n");
            thinking_line_open_ = false;
        } else if (emitted_tool_) {
            write_err("\n");
        }
    }
    line_buffer_.append(chunk);
    flush_line();
}

void renderer::flush_line() {
    for (;;) {
        auto const newline = line_buffer_.find('\n');
        if (newline == std::string::npos) break;
        std::string line = line_buffer_.substr(0, newline);
        line_buffer_.erase(0, newline + 1);
        write_out(style_line(line));
        write_out("\n");
    }
}

std::string renderer::style_line(std::string_view line) {
    std::string_view trimmed = trim_left(line);
    bool const fence = starts_with(trimmed, "```") || starts_with(trimmed, "~~~");
    if (fence) {
        in_code_fence_ = !in_code_fence_;
        if (options_.color) return std::string(kDim) + std::string(line) + kReset;
        return std::string(line);
    }
    if (in_code_fence_) {
        if (options_.color) return std::string(kCyan) + std::string(line) + kReset;
        return std::string(line);
    }
    if (!options_.color) return std::string(line);

    if (!trimmed.empty() && trimmed[0] == '#') {
        std::size_t level = 0;
        while (level < trimmed.size() && trimmed[level] == '#') ++level;
        if (level <= 6 && level < trimmed.size() && trimmed[level] == ' ') {
            std::string out;
            out += kBold;
            out += kUnderline;
            out.append(trimmed.substr(level + 1));
            out += kReset;
            return out;
        }
    }

    if (trimmed.size() >= 3 && (trimmed[0] == '-' || trimmed[0] == '*' || trimmed[0] == '_')) {
        char const mark = trimmed[0];
        bool rule = true;
        for (char value : trimmed) {
            if (value != mark && !is_space(value)) {
                rule = false;
                break;
            }
        }
        if (rule) return std::string(kDim) + std::string(line) + kReset;
    }

    if (!trimmed.empty() && trimmed[0] == '>') {
        return std::string(kDim) + std::string(kItalic) + std::string(trimmed) + kReset;
    }

    std::size_t const indent = line.size() - trimmed.size();
    std::size_t marker = 0;
    if (trimmed.size() >= 2 && (trimmed[0] == '-' || trimmed[0] == '*' || trimmed[0] == '+') && trimmed[1] == ' ') {
        marker = 2;
    } else {
        std::size_t digits = 0;
        while (digits < trimmed.size() && std::isdigit(static_cast<unsigned char>(trimmed[digits])) != 0) ++digits;
        if (digits > 0 && digits + 1 < trimmed.size() && trimmed[digits] == '.' && trimmed[digits + 1] == ' ') {
            marker = digits + 2;
        }
    }
    if (marker > 0) {
        std::string out(line.substr(0, indent));
        out += kCyan;
        out.append(trimmed.substr(0, marker));
        out += kReset;
        out.append(style_inline(trimmed.substr(marker)));
        return out;
    }

    return style_inline(line);
}

void renderer::emit_thinking(std::string_view chunk) {
    if (!options_.show_thinking || chunk.empty()) return;
    bool const styled = options_.color && err_tty_;
    if (styled && !thinking_dim_open_) {
        write_err(kDim);
        thinking_dim_open_ = true;
    }
    if (!emitted_thinking_) write_err("💭 ");
    write_err(chunk);
    emitted_thinking_ = true;
    thinking_line_open_ = chunk.back() != '\n';
}

void renderer::emit_tool(stream_event const& event) {
    if (!event.status.empty() && event.status != "start" && event.args.empty()) return;
    std::string const label = event.name.empty() ? "tool" : event.name;
    std::string const note = "⚙ " + label + " (" + (event.status.empty() ? "running" : event.status) + ")";
    tool_notes_ += note;
    tool_notes_ += '\n';
    emitted_tool_ = true;
    if (thinking_dim_open_) {
        write_err(kReset);
        thinking_dim_open_ = false;
    }
    if (thinking_line_open_) {
        write_err("\n");
        thinking_line_open_ = false;
    }
    if (options_.color && err_tty_) {
        write_err(std::string(kDim) + note + kReset + "\n");
    } else {
        write_err(note + "\n");
    }
}

void renderer::write_debug(stream_event const& event) {
    if (!options_.debug || event.kind != event_kind::raw || event.text.empty()) return;
    write_err("[raw] " + event.text + "\n");
}

std::string renderer::finish() {
    if (!line_buffer_.empty()) {
        write_out(style_line(line_buffer_));
        line_buffer_.clear();
    }
    if (!text_.empty() && text_.back() != '\n') write_out("\n");
    if (thinking_dim_open_) {
        write_err(kReset);
        thinking_dim_open_ = false;
    }
    if (thinking_line_open_) {
        write_err("\n");
        thinking_line_open_ = false;
    }
    if (!usage_.empty()) {
        std::string const line = "[usage] " + usage_ + "\n";
        if (options_.color && err_tty_) write_err(std::string(kDim) + line + kReset);
        else
            write_err(line);
    }
    return error_;
}

} // namespace prompt::ai
