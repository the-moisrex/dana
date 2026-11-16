#include "sink_session.hpp"

#include "../sdk/errors.hpp"
#include "providers/registry.hpp"
#include "state.hpp"

#include <optional>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace dana::ai {

    namespace {

        std::string terminal_scope_or_default(cli_options const& options) {
            return options.terminal_scope.empty() ? state::terminal_scope() : options.terminal_scope;
        }

    } // namespace

    sink_setup prepare_sink(std::string_view provider_name, std::span<std::string_view const> args) {
        sink_setup setup;
        setup.label          = std::string(provider_name);
        auto const canonical = canonical_provider(provider_name);
        if (!canonical) {
            throw soft_error{"ai." + setup.label + ": unknown provider\n"};
        }
        setup.canonical = *canonical;
        setup.instance  = find_provider(*canonical);
        if (setup.instance == nullptr) {
            throw soft_error{"ai." + setup.label + ": unknown provider\n"};
        }

        std::vector<std::string> argv;
        argv.reserve(args.size());
        for (std::string_view const arg : args) {
            argv.emplace_back(arg);
        }

        try {
            setup.options = parse_cli_options(*setup.instance, argv);
        } catch (std::exception const& failure) {
            throw usage_error{"ai." + setup.label + ": " + failure.what() + "\n"};
        }
        if (setup.options.interactive) {
            throw usage_error{
              "ai." + setup.label + ": -i/--interactive cannot be used in a prompt chain; run it directly\n"};
        }
        if (!setup.options.prompt.empty()) {
            throw usage_error{"ai." + setup.label +
                              ": positional prompt text is not supported in a chain; put instructions in "
                              "`.note \"...\"` before the sink or use --system\n"};
        }
        return setup;
    }

    sink_session open_sink_session(sink_setup setup) {
        sink_session session;
        session.label       = std::move(setup.label);
        session.canonical   = std::move(setup.canonical);
        session.instance    = setup.instance;
        session.options     = std::move(setup.options);
        session.scope       = terminal_scope_or_default(session.options);
        auto const& options = session.options;

        state::conversation_record resume_record;
        bool                       has_resume = false;
        if (options.new_) {
            state::clear(session.canonical, session.scope);
        } else if (options.reply) {
            auto loaded = state::load_terminal(session.canonical, session.scope);
            if (!loaded) {
                throw soft_error{"ai." + session.label + ": no chat in this terminal to reply to; start one with ai." +
                                 session.canonical + "\n"};
            }
            resume_record = *loaded;
            has_resume    = true;
        } else if (options.continue_) {
            if (auto loaded = state::load(session.canonical)) {
                resume_record = *loaded;
                has_resume    = true;
            }
        }

        session::match match;
        if (session.instance->info().needs_session) {
            session::options session_options;
            session_options.browser   = options.browser;
            session_options.profile   = options.profile;
            session_options.family    = options.family;
            session_options.open_only = options.open_only;
            session_options.exact     = options.exact;
            try {
                auto found = session::load(session.instance->info().session_target, session_options);
                if (!found) {
                    throw soft_error{"ai." + session.label + ": no signed-in session found for " +
                                     session.instance->info().session_target + "; open " +
                                     session.instance->info().origin + " in a browser once\n"};
                }
                match = *found;
            } catch (soft_error const&) {
                throw;
            } catch (std::exception const& failure) {
                throw soft_error{"ai." + session.label + ": " + failure.what() + "\n"};
            }
        }

        std::string requested = options.model;
        if (requested.empty() && has_resume) {
            requested = resume_record.model;
        }
        std::string model;
        try {
            model = session.instance->resolve_model(requested, session.instance->info().default_base_url);
        } catch (std::exception const& failure) {
            throw soft_error{"ai." + session.label + ": " + failure.what() + "\n"};
        }

        open_options open;
        open.system   = options.system;
        open.model    = options.model;
        open.debug    = options.debug;
        open.timeout  = options.timeout;
        open.base_url = options.base_url;
        open.api_key  = options.api_key;
        open.params   = options.params;
        if (has_resume) {
            open.conversation_id = resume_record.conversation_id;
            open.history         = resume_record.history;
            open.resume          = resume_record.resume;
        }

        try {
            session.chat = session.instance->open(match, model, open);
        } catch (std::exception const& failure) {
            throw soft_error{"ai." + session.label + ": " + failure.what() + "\n"};
        }
        return session;
    }

    turn_options make_turn_options(cli_options const& options, conversation const& chat) {
        turn_options turn;
        turn.json         = options.json;
        turn.raw          = options.raw;
        turn.tty          = ::isatty(STDOUT_FILENO) != 0;
        turn.no_color     = options.no_color;
        turn.debug        = options.debug;
        turn.thinking     = options.thinking;
        turn.prompt_file  = options.prompt_file;
        turn.prompt_limit = options.prompt_limit;
        turn.files        = chat.turns() == 0 ? options.files : std::vector<std::string>{};
        turn.width        = options.width;
        turn.height       = options.height;
        if (turn.tty) {
            winsize size{};
            if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0) {
                if (turn.width == 0 && size.ws_col != 0) {
                    turn.width = size.ws_col;
                }
                if (turn.height == 0 && size.ws_row != 0) {
                    turn.height = size.ws_row;
                }
            }
        }
        return turn;
    }

    void save_sink_state(sink_session const& session) {
        try {
            state::conversation_record record;
            record.conversation_id = session.chat->conversation_id();
            record.model           = session.chat->model();
            record.history         = session.chat->history();
            record.resume          = session.chat->resume();
            state::save(session.canonical, record, session.scope);
        } catch (std::exception const&) {
            // A state write can never fail the turn.
            (void) 0;
        }
    }

} // namespace dana::ai
