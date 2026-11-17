#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace dana {

    // A failing prompt throws this; the dispatcher catches it once and turns it
    // back into the process result: `message` on stderr (verbatim, unsanitized,
    // including its trailing newline), `partial_output` prepended to stdout (some
    // prompts print their prompt text *and* fail), and `exit_code` as the process
    // status.
    //
    // The chain keeps running after a failure — the first non-zero status wins.
    struct [[nodiscard]] prompt_error : std::runtime_error {
        // NOLINTNEXTLINE(performance-unnecessary-value-param) — by value on purpose: callers std::move the message in.
        prompt_error(int         exit_code,
                     std::string message,
                     std::string partial_output = {},
                     bool        no_clipboard   = false,
                     bool        stdin_consumed = false)
          // runtime_error's ctor takes a const& and copies; the message is
          // consumed there, partial_ still moves. NOLINT: no way to move into it.
          : std::runtime_error(std::move(message)), // NOLINT(performance-move-const-arg)
            m_exit_code(exit_code),
            m_partial(std::move(partial_output)),
            m_no_clipboard(no_clipboard),
            m_stdin_consumed(stdin_consumed) {}

        [[nodiscard]] int exit_code() const {
            return m_exit_code;
        }

        [[nodiscard]] std::string const& partial_output() const {
            return m_partial;
        }

        [[nodiscard]] bool no_clipboard() const {
            return m_no_clipboard;
        }

        [[nodiscard]] bool stdin_consumed() const {
            return m_stdin_consumed;
        }

      private:
        int         m_exit_code;
        std::string m_partial;
        bool        m_no_clipboard;
        bool        m_stdin_consumed;
    };

    // Argument/usage failures (bash `exit 2`).
    struct [[nodiscard]] usage_error : prompt_error {
        explicit usage_error(std::string message,
                             std::string partial_output = {},
                             bool        no_clipboard   = false,
                             bool        stdin_consumed = false)
          : prompt_error(2, std::move(message), std::move(partial_output), no_clipboard, stdin_consumed) {}
    };

    // Runtime failures (bash `exit 1`).
    struct [[nodiscard]] soft_error : prompt_error {
        explicit soft_error(std::string message,
                            std::string partial_output = {},
                            bool        no_clipboard   = false,
                            bool        stdin_consumed = false)
          : prompt_error(1, std::move(message), std::move(partial_output), no_clipboard, stdin_consumed) {}
    };

} // namespace dana
