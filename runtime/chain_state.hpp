#pragma once

#include "../core/strings.hpp"
#include "facts.hpp"

#include <string>
#include <string_view>

namespace dana {

    // The dispatcher's single accumulator. `append` is the one text-merge rule
    // (`text::append_block`): trim the block on both sides, skip it when empty, and
    // separate consecutive blocks with one blank line. `known` holds the artifacts
    // of completed segments (last writer wins: the nearest successful segment).
    struct [[nodiscard]] chain_state {
        std::string text;
        facts       known;
        bool        redacted   = false;
        bool        incomplete = false;
    };

    void append(chain_state& state, std::string_view block, bool block_redacted);

    void clear(chain_state& state);

} // namespace dana
