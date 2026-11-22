#pragma once

// Streaming renderer for the normalised events.  On a terminal the answer is
// rendered as Markdown while it streams and reasoning is dimmed on stderr; when
// piped (or with --raw) the visible text is written untouched so
// `ai.grok … > answer.md` stays clean.  The chain context always receives the
// plain answer, never the styled bytes.

#include "events.hpp"

#include <functional>
#include <string>
#include <string_view>

namespace dana::ai {

    struct render_options {
        bool show_thinking = true;
        bool raw           = false;
        bool color         = true;
        bool debug         = false;
        bool force_tty     = false;
        int  width         = 0;
        int  height        = 0;
    };

    class renderer {
      public:
        using writer = std::function<void(std::string_view)>;

        explicit renderer(render_options const& options);
        renderer(render_options const& options, writer out, writer err);

        void handle(stream_event const& event);
        void write_debug(stream_event const& event);

        // Flush the pending line and report token usage; returns the error text
        // (empty when the turn succeeded).
        std::string finish();

        [[nodiscard]] std::string_view text() const {
            return text_;
        }

        [[nodiscard]] std::string_view thinking() const {
            return thinking_;
        }

        [[nodiscard]] std::string_view error() const {
            return error_;
        }

      private:
        void        emit_text(std::string_view chunk);
        void        emit_thinking(std::string_view chunk);
        void        emit_tool(stream_event const& event);
        void        write_out(std::string_view chunk);
        void        write_err(std::string_view chunk);
        void        flush_line();
        std::string style_line(std::string_view line);

        render_options options_;
        writer         out_;
        writer         err_;
        std::string    text_;
        std::string    thinking_;
        std::string    tool_notes_;
        std::string    usage_;
        std::string    error_;
        std::string    line_buffer_;
        bool           in_code_fence_      = false;
        bool           tty_                = false;
        bool           err_tty_            = false;
        bool           emitted_thinking_   = false;
        bool           thinking_line_open_ = false;
        bool           thinking_dim_open_  = false;
        bool           emitted_tool_       = false;
        bool           started_answer_     = false;
    };

} // namespace dana::ai
