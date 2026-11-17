#pragma once

// The shared command line behind `ai.lms`, `ai.openai`, `ai.openrouter` (and,
// as the web providers land, the rest of the family): read the prompt, open the
// conversation, stream one turn or a REPL, and persist the chat for -c/--reply.

#include "events.hpp"
#include "providers/provider.hpp"
#include "render.hpp"

#include <optional>
#include <string>
#include <vector>

namespace dana::ai {

    struct cli_options {
        std::vector<std::string> prompt;
        std::vector<std::string> files;
        std::optional<bool>      prompt_file;
        std::optional<std::int64_t> prompt_limit;
        std::string              model;
        std::string              system;
        bool                     interactive = false;
        bool                     continue_   = false;
        bool                     new_        = false;
        std::optional<bool>      thinking;
        bool                     json     = false;
        bool                     raw      = false;
        bool                     tty      = false;
        bool                     copy     = false;
        bool                     no_color = false;
        bool                     debug    = false;
        long                     timeout  = 600;
        std::string              base_url;
        std::string              api_key;
        std::vector<std::string> params;
        std::vector<std::string> browser;
        std::string              profile;
        std::string              family;
        bool                     open_only     = false;
        bool                     exact         = false;
        bool                     print_session = false;
        bool                     print_prompt  = false;
        bool                     stdin_only    = false;
        std::string              context_file;
        bool                     reply = false;
        std::string              terminal_scope;
        int                      width  = 0;
        int                      height = 0;
        bool                     list   = false;
        bool                     all    = false;
    };

    // Parse one provider's arguments (the same flags every `ai.*` command takes).
    cli_options parse_cli_options(provider const& instance, std::vector<std::string> const& argv);

    // `ai.<provider>`: the whole command.
    int run_provider_cli(std::string const& provider_name, std::vector<std::string> const& argv);

    // `ai.keys`: where each API provider's key would come from.
    int run_keys_cli(std::vector<std::string> const& argv);

    // `ai.debate`: the relay (implemented once the web providers land).
    int run_debate_cli(std::vector<std::string> const& argv);

    // One turn, shared by the CLI and the `.ai.*` sinks.  `out`/`err` receive the
    // display bytes; the plain answer is returned in `answer`.
    struct turn_options {
        bool                     json     = false;
        bool                     raw      = false;
        bool                     tty      = false;
        bool                     no_color = false;
        bool                     debug    = false;
        std::optional<bool>      thinking;
        std::optional<bool>      prompt_file;
        std::optional<std::int64_t> prompt_limit;
        std::vector<std::string> files;
        std::string              context_file;
        int                      width  = 0;
        int                      height = 0;
        // Extra fields merged into every --json event (the debate tags provider/round).
        nlohmann::json json_fields      = nlohmann::json::object();
    };

    struct turn_result {
        int         status = 0;
        std::string answer;
    };

    turn_result run_turn(
      conversation&           chat,
      provider const&         instance,
      std::string const&      prompt,
      turn_options const&     options,
      renderer::writer        out,
      renderer::writer const& err);

    // The text a prompt argument list denotes: joined, or stdin when empty.
    std::string read_prompt(std::vector<std::string> const& parts);

    // Copy `text` to the system clipboard (best effort; false when no tool exists).
    bool copy_answer(std::string const& text);

    // The `--list` and `--print-session` reports as text, for the in-process sinks
    // (which must write into the chain's stdout buffer, not the process's).
    std::string list_models_text(provider& instance, cli_options const& options);
    std::string session_report_text(provider& instance, cli_options const& options);

} // namespace dana::ai
