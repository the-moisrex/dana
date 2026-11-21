#pragma once

// The normalised stream-event model shared by every provider adapter.  Each
// adapter translates its site's wire format into this flat sequence; the
// renderer, the --json emitter and the sinks only ever see this model.

#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace dana::ai {

    enum struct [[nodiscard]] event_kind : unsigned char {
        thinking,  // reasoning text (DeepSeek reasoning_content, ChatGPT thoughts, …)
        text,      // the visible answer
        tool_call, // a tool the *site* is running; display only
        usage,     // token accounting, when the provider reports it
        done,      // the stream finished normally
        error,     // a fatal problem
        raw,       // an unrecognised line (kept for --debug)
    };

    struct stream_event {
        event_kind     kind = event_kind::text;
        std::string    text;
        std::string    name;   // tool name
        std::string    status; // tool phase: start | delta | done
        std::string    args;   // tool arguments (raw JSON text)
        nlohmann::json data = nlohmann::json::object();
    };

    inline stream_event thinking(std::string_view const value) {
        stream_event event;
        event.kind = event_kind::thinking;
        event.text = std::string(value);
        return event;
    }

    inline stream_event text(std::string_view const value) {
        stream_event event;
        event.kind = event_kind::text;
        event.text = std::string(value);
        return event;
    }

    inline stream_event tool_call(
      std::string    name   = {},
      std::string    status = "delta",
      std::string    args   = {},
      nlohmann::json data   = nlohmann::json::object()) {
        stream_event event;
        event.kind   = event_kind::tool_call;
        event.name   = std::move(name);
        event.status = std::move(status);
        event.args   = std::move(args);
        event.data   = std::move(data);
        return event;
    }

    inline stream_event usage(nlohmann::json data) {
        stream_event event;
        event.kind = event_kind::usage;
        event.data = std::move(data);
        return event;
    }

    inline stream_event done() {
        stream_event event;
        event.kind = event_kind::done;
        return event;
    }

    inline stream_event error(std::string_view const message) {
        stream_event event;
        event.kind = event_kind::error;
        event.text = std::string(message);
        return event;
    }

    inline stream_event raw(std::string_view const line) {
        stream_event event;
        event.kind = event_kind::raw;
        event.text = std::string(line);
        return event;
    }

    inline std::string_view kind_name(event_kind const kind) {
        switch (kind) {
            case event_kind::thinking: return "thinking";
            case event_kind::text: return "text";
            case event_kind::tool_call: return "tool_call";
            case event_kind::usage: return "usage";
            case event_kind::done: return "done";
            case event_kind::error: return "error";
            case event_kind::raw: return "raw";
        }
        return "text";
    }

    using event_sink = std::function<void(stream_event const&)>;

} // namespace dana::ai
