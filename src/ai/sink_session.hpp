#pragma once

// The setup half of an AI sink, shared by `.ai.<provider>` and `.ai.loop`:
// parse the provider's arguments, resolve the browser session and model, open
// the conversation and persist it afterwards. The plain sink runs one turn; the
// loop runs many on the same conversation.

#include "cli.hpp"
#include "providers/provider.hpp"

#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace dana::ai {

    // Parsed but not opened: cheap, no session or network access. `--print-prompt`,
    // `--list` and `--print-session` are answered from this alone.
    struct [[nodiscard]] sink_setup {
        std::string label; // the provider name as the user wrote it ("gap")
        std::string canonical;
        provider*   instance = nullptr;
        cli_options options;
    };

    // Throws usage_error/soft_error exactly as the plain sink does.
    [[nodiscard]] sink_setup prepare_sink(std::string_view provider_name, std::span<std::string_view const> args);

    struct [[nodiscard]] sink_session {
        std::string                   label;
        std::string                   canonical;
        std::string                   scope;
        provider*                     instance = nullptr;
        std::unique_ptr<conversation> chat;
        cli_options                   options;
    };

    // Load the session, resolve the model and open the conversation.
    sink_session open_sink_session(sink_setup setup);

    // The per-turn options: terminal rendering, thinking, prompt-file handling and
    // the first-turn attachments.
    turn_options make_turn_options(cli_options const& options, conversation const& chat);

    // Persist the conversation under the provider's terminal scope, so `-c` and
    // `.ai.reply` continue it.
    void save_sink_state(sink_session const& session);

} // namespace dana::ai
