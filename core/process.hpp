#pragma once

#include "strings.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dana::process {

    struct [[nodiscard]] command_result {
        std::string stdout_data;
        std::string stderr_data;
        int         exit_code = 0;
        bool        timed_out = false;
    };

    // How a child's stderr is handled.
    enum struct [[nodiscard]] stderr_mode : std::uint8_t {
        inherit,   // the child writes straight to our stderr — what a `$(...)`
                   // capture does (the tool's diagnostics appear as they run).
        capture,   // piped into command_result::stderr_data.
        to_stdout, // `2>&1` — merged into command_result::stdout_data.
        discard,   // `2>/dev/null` — thrown away.
    };

    // A subprocess, built fluently:
    //
    //     process::run_command({"git", "status"}, stdin_data, timeout);
    //     process::command{"bash", "-c", "echo hi"}.capture_stderr().run();
    //
    // `argv` is executed directly (execvp — no shell, no quoting games: an
    // argument containing spaces, quotes or newlines reaches the child intact).
    // Use command::shell() when the work genuinely needs /bin/bash semantics
    // (pipes, globs, $(...) substitution).
    struct [[nodiscard]] command {
        command() = default;

        command(text::string_views const argv) : m_argv(argv.begin(), argv.end()) {}

        explicit command(std::vector<std::string> argv) : m_argv(std::move(argv)) {}

        command& arg(std::string_view str);
        command& args(text::string_views as);
        command& stdin_data(std::string data);
        command& env(text::string_views extra); // "KEY=VALUE" entries
        command& timeout(std::chrono::milliseconds timeout);
        command& stderr(stderr_mode mode);
        command& capture_stderr();              // stderr_mode::capture
        command& stderr_to_stdout();            // stderr_mode::to_stdout
        command& inherit_stderr();              // stderr_mode::inherit (default)
        command& discard_stderr();              // stderr_mode::discard (`2>/dev/null`)

        // Tee callbacks: the matching stream is piped, each chunk is passed to the
        // callback as it arrives, and the complete text is still accumulated in
        // command_result. Used by sinks to stream to the terminal without losing
        // the answer they need for the next sink.
        using chunk_callback = std::function<void(std::string_view)>;
        command& on_stdout(chunk_callback callback);
        command& on_stderr(chunk_callback callback);

        command_result run();

      private:
        std::vector<std::string>  m_argv;
        std::string               m_stdin_data;
        std::vector<std::string>  m_env;
        std::chrono::milliseconds m_timeout{0};
        stderr_mode               m_stderr_mode{stderr_mode::inherit};
        chunk_callback            m_stdout_callback;
        chunk_callback            m_stderr_callback;
    };

    // Runs a bash script snippet (`/bin/bash -c <script>`), with "extras..." passed
    // as $0 $1 .... This is the escape hatch: prefer argv construction above unless
    // the command needs a real shell.
    command_result run_script(
      std::string_view        script,
      text::string_views        extras     = {},
      std::string_view          stdin_data = {},
      std::chrono::milliseconds timeout    = std::chrono::seconds(30));

    // command_result run_command(
    //   std::span<char const* const> argv,
    //   std::string_view             stdin_data = {},
    //   std::span<char const* const> env        = {},
    //   std::chrono::milliseconds    timeout    = std::chrono::seconds(30));

    command_result run_command(
      text::string_views        argv,
      std::string_view          stdin_data = {},
      text::string_views        env        = {},
      std::chrono::milliseconds timeout    = std::chrono::seconds(30));

    struct [[nodiscard]] snippet_result {
        // stdout and stderr merged, exactly like `$(... 2>&1)`.
        std::string output;
        int         exit_code = 0;
    };

    // `$(...)` capture semantics: every trailing newline is dropped and the script
    // re-adds exactly one when it prints the value.
    std::string captured(std::string_view text);

    snippet_result run_snippet(std::string_view script, std::string_view stdin_data);


    // See if this tool exists or not
    bool tool_exists(std::string_view name) noexcept;

    // xargs: every whitespace-separated token becomes one subtitle argument (an
    // empty token list still runs the command once, like GNU xargs without -r).
    std::vector<std::string_view> xargs_tokens(std::string_view text);
} // namespace dana::process
