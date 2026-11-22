#pragma once

// The provider-independent agentic loop behind `.ai.loop`: send the task, read
// the answer, run the tool calls it contains, feed the results back, repeat
// until the model answers without a call (or the turn budget runs out).
//
// Everything with a side effect is injected, so the loop itself is pure logic
// and unit-testable without a provider: `turn` runs one assistant turn,
// `run_tools` executes the calls in an answer (returning nullopt when there
// were none) and `status` receives the one-line progress notes for stderr.

#include "cli.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace dana::ai {

    struct [[nodiscard]] agent_loop_options {
        // Hard stop; a model that never stops calling tools cannot run forever.
        int max_turns                  = 25;
        // Per-turn call cap (0 = unlimited). Excess calls are dropped with a note.
        std::size_t max_calls_per_turn = 16;
        // Per-result truncation, so one big file read cannot poison the context.
        std::size_t tool_head_lines    = 400;
        std::size_t tool_max_bytes     = std::size_t{64} * 1024;
        // Final context is the whole loop instead of only the last answer.
        bool transcript                = false;
        // Print each tool result on stderr as well as feeding it to the model.
        bool show_tools                = false;
    };

    struct [[nodiscard]] agent_loop_io {
        // One assistant turn. `status != 0` aborts the loop.
        std::function<turn_result(std::string_view)> turn;
        // Execute the calls in an answer. nullopt means "no calls": the loop is done.
        std::function<std::optional<std::string>(std::string_view)> run_tools;
        // Progress lines for stderr (never part of the chain context).
        std::function<void(std::string_view)> status;
    };

    struct [[nodiscard]] agent_loop_result {
        // The last assistant answer (also on exhaustion).
        std::string answer;
        // Every answer and tool block, folded with the shared text rule.
        std::string transcript;
        int         turns  = 0;
        // True when max_turns was reached while the model was still calling tools.
        bool exhausted     = false;
        // Non-zero when a turn failed; `error` then carries the message.
        int         status = 0;
        std::string error;
    };

    agent_loop_result run_agent_loop(std::string initial, agent_loop_options const& options, agent_loop_io const& io);

    // Keep the first `head_lines` lines and at most `max_bytes` bytes, appending a
    // one-line note when anything was dropped. 0 disables either limit.
    [[nodiscard]] std::string
    truncate_tool_result(std::string_view text, std::size_t head_lines, std::size_t max_bytes);

} // namespace dana::ai
