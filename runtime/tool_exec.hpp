#pragma once

// The execution half of the tool-call layer: take the `{"tool": …}` blocks an
// AI answer contains, resolve each to a registered prompt's argv, approve it
// through the policy and run it in-process, collecting the `### Tool:` feedback
// blocks the model reads on its next turn.
//
// `.tools.run` and the `.ai.loop` agent loop share this, so the two can never
// disagree about what a call is or how it runs. `sdk/tool_call.hpp` owns the
// protocol itself (extract / parse / format); this owns the side effects.

#include "stage.hpp"
#include "tool_call.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace dana::tool_exec {

    struct options {
        // Turn policy asks into allows (never overrides a deny).
        bool yes              = false;
        // Stop at the first failed call instead of feeding the error back.
        bool abort_on_error   = false;
        // Build the machine-readable `json_report` as well as the blocks.
        bool json             = false;
        // Run at most this many calls (0 = unlimited). Excess calls are dropped
        // with a note in `blocks`, so a runaway answer cannot execute forever.
        std::size_t max_calls = 0;
    };

    struct outcome {
        // The calls found in the input text (empty when there were none).
        std::vector<tool_call::call> calls;
        // The `### Tool: <name>` blocks, in call order. Empty when there were no
        // calls; a call that produced no output still contributes its block.
        std::string blocks;
        // A JSON array of {tool, input, ok, exit, output}; only when `json`.
        std::string json_report;
    };

    // Run every call in `text`. A policy deny or a cancelled approval always
    // aborts, whatever `abort_on_error` says: that is the user saying no.
    [[nodiscard]] outcome run(view const& v, std::string_view text, options const& opts);

} // namespace dana::tool_exec
