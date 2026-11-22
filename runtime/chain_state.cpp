// Created by moisrex on 11/18/26.

#include "./chain_state.hpp"

void dana::append(chain_state& state, std::string_view const block, bool const block_redacted) {
    if (!text::append_block(state.text, block)) {
        return;
    }
    state.redacted = state.redacted || block_redacted;
}

void dana::clear(chain_state& state) {
    state.text.clear();
    state.known.clear();
    state.redacted   = false;
    state.incomplete = false;
}
