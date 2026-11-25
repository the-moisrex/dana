#include "../ai/agent_loop.hpp"
#include "../ai/cli.hpp"
#include "../ai/models.hpp"
#include "../ai/providers/registry.hpp"
#include "../ai/sink_session.hpp"
#include "../ai/state.hpp"
#include "../core/fs.hpp"
#include "../core/process.hpp"
#include "../core/strings.hpp"
#include "../runtime/alias.hpp"
#include "../runtime/completion.hpp"
#include "../runtime/embed.hpp"
#include "../runtime/errors.hpp"
#include "../runtime/facts.hpp"
#include "../runtime/prompt_registry.hpp"
#include "../runtime/sink.hpp"
#include "../runtime/stage.hpp"
#include "../runtime/terminal.hpp"
#include "../runtime/tool_call.hpp"
#include "../runtime/tool_exec.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <system_error>
#include <unistd.h>
#include <unordered_set>
#include <vector>

namespace {

    using namespace dana;

    struct ai_provider {
        std::string_view name;
        std::string_view label;
        /// Speaks the OpenAI-compatible HTTP API instead of replaying a web session.
        bool api   = false;
        /// A local server the tool discovers on its own (LM Studio).
        bool local = false;
        // GCC's -Wmissing-field-initializers wants a member initializer so the
        // designators above stay warning-free; clang-tidy calls the empty ones
        // redundant. Keep both quiet, as `stage.hpp` does.
        // NOLINTBEGIN(readability-redundant-member-init)
        std::string_view base_url{}; // default API root, empty = discovered
        std::string_view base_url_env{};
        std::string_view key_env{};
        std::string_view model_env{};
        std::string_view default_model{}; // used when no -m/env model is set
        // NOLINTEND(readability-redundant-member-init)
    };

    // The provider writes to a pipe when it runs as a sink, so its stdout is
    // display-only: rich Markdown frames (or --json events) must never reach the
    // clipboard or the next prompt. `bin/ai --context-file` carries the plain
    // answer out of band; the file is removed on every path, success or failure.
    class answer_file {
      public:
        answer_file() : path_(make_path()) {}

        ~answer_file() {
            remove();
        }

        answer_file(answer_file const&)            = delete;
        answer_file& operator=(answer_file const&) = delete;

        [[nodiscard]] std::filesystem::path const& path() const {
            return path_;
        }

        // The answer written so far, or `nullopt` when the tool never created the
        // file (a pre-flight failure, --print-prompt, …). Followed by a best-effort
        // unlink so no answer is left behind.
        [[nodiscard]] std::optional<std::string> take() {
            std::error_code ec;
            bool const      present = std::filesystem::is_regular_file(path_, ec);
            std::string     text    = present ? read_file(path_) : std::string{};
            remove();
            if (!present) {
                return std::nullopt;
            }
            return text;
        }

      private:
        static std::filesystem::path make_path() {
            static std::atomic<unsigned> counter{0};
            std::error_code              ec;
            auto                         dir = std::filesystem::temp_directory_path(ec);
            if (ec) {
                dir = std::filesystem::current_path(ec);
            }
            std::string const name = "prompt-ai-" + std::to_string(static_cast<std::int64_t>(::getpid())) + "-" +
                                     std::to_string(counter.fetch_add(1, std::memory_order_relaxed)) + ".txt";
            return dir / name;
        }

        void remove() {
            std::error_code ec;
            std::filesystem::remove(path_, ec);
        }

        std::filesystem::path path_;
    };

    // The child's stdout/stderr are pipes, so a renderer cannot measure the
    // terminal prompt forwards to. Pass the size read from the real one.
    void add_terminal_size(std::vector<std::string>& argv) {
        winsize size{};
        if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0) {
            return;
        }
        if (size.ws_col != 0) {
            argv.emplace_back("--width=" + std::to_string(size.ws_col));
        }
        if (size.ws_row != 0) {
            argv.emplace_back("--height=" + std::to_string(size.ws_row));
        }
    }

    // --print-prompt, --print-session and --list answer on stdout without an
    // assistant turn, so there is no plain answer to read back.
    bool inspect_only(text::string_views args) {
        return std::ranges::any_of(args, [](std::string_view arg) {
            return arg == "--print-prompt" || arg == "--print-session" || arg == "--list" || arg == "--models";
        });
    }

    void render_help_ai_debate(std::ostream& out) {
        out << "Usage: prompt <other prompts...> .ai.debate PROVIDER PROVIDER [PROVIDER...] [DEBATE-OPTIONS]\n"
            << "       prompt .ai.debate PROVIDER PROVIDER [PROVIDER...] [DEBATE-OPTIONS]\n"
            << "\n"
            << "Ask two or more AIs the same question, then let each verify the previous answer:\n"
            << "the first answer is passed to the next AI as \"Another AI said: …\" with a\n"
            << "verification request, and every later turn carries the newest answer around the\n"
            << "ring. Each AI keeps its own conversation, so later turns only carry the other\n"
            << "AI's latest answer.\n"
            << "\n"
            << "The prompt is a sink: it waits until every prompt before it has run, sends their\n"
            << "combined text to the panel as the question, and (by default) replaces the\n"
            << "running context with the last answer. Sinks may appear anywhere in a chain.\n"
            << "\n"
            << "Providers: chatgpt, claude, grok, deepseek (dseek), gap, kimi, perplexity (pplx),\n"
            << "gemini — plus the API providers lms (lmstudio, a local LM Studio server),\n"
            << "openai (oai, OPENAI_API_KEY) and openrouter (OPENROUTER_API_KEY) — in the\n"
            << "order they should speak.\n"
            << "\n";
        render_sink_help(out);
        out << "\n"
            << "Debate options (passed to ai.debate):\n"
            << "  --rounds N                  Full passes through the providers (default: 2)\n"
            << "  --until-agree               Stop after a pass in which every AI votes AGREE\n"
            << "  --instruction TEXT          Verification request appended to every turn\n"
            << "  --no-instruction            Relay the answer with no verification request\n"
            << "  --context last|all          Last answer (default) or the labelled transcript\n"
            << "  --transcript                Same as --context all\n"
            << "  -c, --continue              Continue each provider's last debate chat\n"
            << "  --new                       Start fresh chats and forget the saved debate\n"
            << "  -m, --model PROVIDER=MODEL  Pick one provider's model (repeatable)\n"
            << "  -s, --system TEXT           System / custom instructions for every provider\n"
            << "  -f, --file PATH             Attach a local file to each first turn (repeatable)\n"
            << "  --prompt-file               Force uploading the prompt as a text file\n"
            << "  --no-prompt-file            Never upload the prompt as a text file\n"
            << "  --prompt-limit BYTES        Override each provider's prompt-file threshold\n"
            << "  --base-url URL              API root for the lms/openai providers\n"
            << "  --api-key KEY               API key for the lms/openai providers\n"
            << "  --param KEY=VALUE           Extra request parameter for them (repeatable)\n"
            << "  --show-thinking             Show the models' reasoning on stderr\n"
            << "  --no-thinking               Hide the models' reasoning\n"
            << "  --json                      Emit one JSON event per line (provider/round tags)\n"
            << "  --raw                       Print only the answers\n"
            << "  --tty                       Render as if stdout/stderr were terminals\n"
            << "  --copy                      Copy the chosen context to the clipboard\n"
            << "  --timeout SECONDS           Per-request timeout (default: 600)\n"
            << "  --print-prompt              Print the first turn and template, then exit\n"
            << "  --print-session             Show each provider's session and exit\n"
            << "  --list                      List the available providers and exit\n"
            << "  -h, --help                  Show this help message\n"
            << "\n"
            << "Notes:\n"
            << "  - Use a preceding .note (or --system) for instructions; positional prompt text\n"
            << "    is rejected because stdin carries the question.\n"
            << "  - Every web provider needs a signed-in browser session; the API providers\n"
            << "    (lms, openai) need their local server / API key instead. Sessions are loaded\n"
            << "    up front, before the first turn.\n"
            << "  - The final answer (or --context all) becomes the chain context and clipboard.\n"
            << "    Chain .save after the sink to keep it, or .ai.kimi to summarise the debate.\n"
            << "  - Every debate saves the panel's chats under $XDG_STATE_HOME/ai/; -c resumes\n"
            << "    the last debate between the same providers and --new discards it.\n";
    }

    // The model candidates for one provider, offered after `-m`/`--model` (and for
    // the `--model=` form). Returns true when the token is a model position, so a
    // caller can fall back to its other candidates otherwise.
    bool complete_ai_model(dana::completion_request const& request, std::ostream& out, std::string_view provider) {
        constexpr std::string_view kModelFlag  = "--model=";
        bool const                 equals_form = request.prefix.starts_with(kModelFlag);
        bool const                 value_position =
          !request.prior_args.empty() && (request.prior_args.back() == "-m" || request.prior_args.back() == "--model");
        if (!equals_form && !value_position) {
            return false;
        }

        auto const             models        = ai::models::list(provider);
        std::string_view const default_model = ai::models::default_model(provider);
        auto const             description   = [&](std::string_view model) -> std::string_view {
            return std::string_view{model} == default_model ? "default model" : std::string_view{};
        };

        if (equals_form) {
            std::string_view const typed = request.prefix.substr(kModelFlag.size());
            for (auto const& model : models) {
                if (!dana::completion::matches(typed, model)) {
                    continue;
                }
                std::string value{kModelFlag};
                value += model;
                dana::completion::write_item(out, value, description(model));
            }
            return true;
        }
        for (auto const& model : models) {
            if (dana::completion::matches(request.prefix, model)) {
                dana::completion::write_item(out, model, description(model));
            }
        }
        return true;
    }

    void complete_ai_chatgpt(dana::completion_request const& request, std::ostream& out) {
        complete_ai_model(request, out, "chatgpt");
    }

    void complete_ai_claude(dana::completion_request const& request, std::ostream& out) {
        complete_ai_model(request, out, "claude");
    }

    void complete_ai_dseek(dana::completion_request const& request, std::ostream& out) {
        complete_ai_model(request, out, "dseek");
    }

    void complete_ai_gap(dana::completion_request const& request, std::ostream& out) {
        complete_ai_model(request, out, "gap");
    }

    void complete_ai_gemini(dana::completion_request const& request, std::ostream& out) {
        complete_ai_model(request, out, "gemini");
    }

    void complete_ai_grok(dana::completion_request const& request, std::ostream& out) {
        complete_ai_model(request, out, "grok");
    }

    void complete_ai_kimi(dana::completion_request const& request, std::ostream& out) {
        complete_ai_model(request, out, "kimi");
    }

    void complete_ai_perplexity(dana::completion_request const& request, std::ostream& out) {
        complete_ai_model(request, out, "perplexity");
    }

    void complete_ai_lms(dana::completion_request const& request, std::ostream& out) {
        complete_ai_model(request, out, "lms");
    }

    void complete_ai_openai(dana::completion_request const& request, std::ostream& out) {
        complete_ai_model(request, out, "openai");
    }

    void complete_ai_openrouter(dana::completion_request const& request, std::ostream& out) {
        complete_ai_model(request, out, "openrouter");
    }

    void complete_ai_debate(dana::completion_request const& request, std::ostream& out) {
        using dana::completion::item;
        static constexpr std::array<item, 15> kProviders = {
          {
           {.value = "chatgpt", .description = "ChatGPT"},
           {.value = "claude", .description = "Claude"},
           {.value = "grok", .description = "Grok"},
           {.value = "deepseek", .description = "DeepSeek"},
           {.value = "dseek", .description = "DeepSeek (alias)"},
           {.value = "gap", .description = "GapGPT"},
           {.value = "gemini", .description = "Gemini"},
           {.value = "kimi", .description = "Kimi"},
           {.value = "perplexity", .description = "Perplexity"},
           {.value = "pplx", .description = "Perplexity (alias)"},
           {.value = "lms", .description = "LM Studio (local)"},
           {.value = "lmstudio", .description = "LM Studio (alias)"},
           {.value = "openai", .description = "OpenAI API"},
           {.value = "oai", .description = "OpenAI API (alias)"},
           {.value = "openrouter", .description = "OpenRouter"},
           },
        };
        // -m/--model takes PROVIDER=MODEL, so complete the provider half there,
        // and the model half once a provider is named.
        if (
          !request.prior_args.empty() && (request.prior_args.back() == "-m" || request.prior_args.back() == "--model"))
        {
            auto const equals = request.prefix.find('=');
            if (equals != std::string_view::npos) {
                std::string_view const provider = request.prefix.substr(0, equals);
                std::string_view const typed    = request.prefix.substr(equals + 1);
                if (auto const* entry = ai::models::find(provider)) {
                    for (auto const& model : ai::models::list(provider)) {
                        if (!dana::completion::matches(typed, model)) {
                            continue;
                        }
                        std::string value{provider};
                        value += '=';
                        value += model;
                        dana::completion::write_item(
                          out,
                          value,
                          std::string_view{model} == entry->default_model ? "default model" : "");
                    }
                    return;
                }
            }
            for (auto const& provider : kProviders) {
                std::string value{provider.value};
                value += '=';
                if (dana::completion::matches(request.prefix, value)) {
                    dana::completion::write_item(out, value, provider.description);
                }
            }
            return;
        }
        dana::completion::write_items(out, request.prefix, kProviders);
    }

    // The state directory `bin/ai/state.py` uses, so `.ai.reply` finds the same
    // per-terminal marker the provider just wrote.
    std::filesystem::path ai_state_directory() {
        if (char const* xdg = std::getenv("XDG_STATE_HOME"); xdg != nullptr && *xdg != '\0') {
            return std::filesystem::path(xdg) / "ai";
        }
        if (char const* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
            return std::filesystem::path(home) / ".local" / "state" / "ai";
        }
        return {};
    }

    // The provider that talked most recently (this terminal, or globally).
    std::optional<std::string> last_ai_provider(bool global) {
        auto dir = ai_state_directory();
        if (dir.empty()) {
            return std::nullopt;
        }
        if (!global) {
            dir /= "terminals";
            dir /= terminal::scope(terminal::current());
        }
        std::string text = read_file(dir / "last");
        while (
          !text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '\t'))
        {
            text.pop_back();
        }
        if (text.empty()) {
            return std::nullopt;
        }
        return text;
    }

    constexpr std::array<std::string_view, 11> kReplyProviders{
      "chatgpt",
      "claude",
      "dseek",
      "gap",
      "gemini",
      "grok",
      "kimi",
      "perplexity",
      "lms",
      "openai",
      "openrouter",
    };

    std::optional<std::string> canonical_provider(std::string_view name) {
        if (name.starts_with("ai.")) {
            name.remove_prefix(3);
        }
        if (name == "deepseek") {
            return std::string{"dseek"};
        }
        if (name == "lmstudio") {
            return std::string{"lms"};
        }
        if (name == "oai") {
            return std::string{"openai"};
        }
        if (name == "pplx") {
            return std::string{"perplexity"};
        }
        for (std::string_view const provider : kReplyProviders) {
            if (name == provider) {
                return std::string(provider);
            }
        }
        return std::nullopt;
    }

    bool native_ai_provider(std::string_view name) {
        // `PROMPT_AI_TOOL=1` keeps the legacy external `bin/ai.<provider>` path
        // available (the offline sink/reply tests use it to inject stubs).
        if (char const* forced = std::getenv("PROMPT_AI_TOOL");
            forced != nullptr && *forced != '\0' && std::string_view(forced) != "0")
        {
            return false;
        }
        auto const canonical = ai::canonical_provider(name);
        return canonical &&
               (*canonical == "lms" || *canonical == "openai" || *canonical == "openrouter" || *canonical == "claude" ||
                *canonical == "kimi" || *canonical == "perplexity" || *canonical == "gemini" || *canonical == "dseek" ||
                *canonical == "chatgpt" || *canonical == "grok" || *canonical == "gap");
    }

    // The API providers run in-process: no child process, no context-file side
    // channel. The styled/JSON stream still goes to the terminal through
    // write_stdout/write_stderr, and the plain answer replaces r.out afterwards, so
    // the chain and clipboard never see frames.
    void execute_ai_native(
      view const&        v,
      result&            r,
      std::string_view   provider,
      text::string_views args,
      std::string_view   context) {
        auto setup = ai::prepare_sink(provider, args);
        if (setup.options.print_prompt) {
            r.out = std::string(context);
            return;
        }
        if (setup.options.list) {
            r.out = ai::list_models_text(*setup.instance, setup.options);
            return;
        }
        if (setup.options.print_session) {
            r.out = ai::session_report_text(*setup.instance, setup.options);
            return;
        }

        auto             session = ai::open_sink_session(std::move(setup));
        ai::turn_options turn    = ai::make_turn_options(session.options, *session.chat);

        auto out = [&v, &r](std::string_view chunk) {
            write_stdout(v, r, chunk);
        };
        auto err = [&v, &r](std::string_view chunk) {
            write_stderr(v, r, chunk);
        };
        auto const outcome = ai::run_turn(*session.chat, *session.instance, std::string(context), turn, out, err);
        r.out              = outcome.answer;

        if (outcome.status == 0) {
            ai::save_sink_state(session);
            r.published.set(keys::ai_provider, provider);
            if (!r.out.empty()) {
                r.published.set(keys::ai_answer, r.out);
            }
            return;
        }
        throw prompt_error{1,
                           "ai." + std::string(provider) + ": the turn failed\n",
                           r.out,
                           r.no_clipboard,
                           v.in.piped()};
    }

    // ---- `.ai.loop`: the agentic tool loop ------------------------------------


    // A loop flag is either `--flag value` or `--flag=value`. `value` is empty for
    // the first form, which means "read the next argument".
    struct flag_match {
        bool             hit = false;
        std::string_view value;
    };

    flag_match match_flag(std::string_view arg, std::string_view flag) {
        if (arg == flag) {
            return {.hit = true, .value = {}};
        }
        if (arg.size() > flag.size() && arg.starts_with(flag) && arg[flag.size()] == '=') {
            return {.hit = true, .value = arg.substr(flag.size() + 1)};
        }
        return {};
    }

    struct loop_cli {
        ai::agent_loop_options        loop;
        bool                          yes          = false;
        bool                          print_prompt = false;
        bool                          read_only    = false;
        bool                          agents       = false;
        bool                          real_paths   = false;
        bool                          any          = false;
        std::string                   tools;
        std::string                   provider;
        std::string                   fallback_model;
        std::vector<std::string_view> forward;
    };

    std::size_t parse_positive(std::string_view flag, std::string_view text) {
        std::size_t value    = 0;
        auto const [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (ec != std::errc{} || ptr != text.data() + text.size() || value == 0) {
            throw usage_error{"ai.loop: " + std::string(flag) + " needs a positive number\n"};
        }
        return value;
    }

    // `.ai.loop [PROVIDER]` and the `.ai.<provider>.loop` aliases: the provider is
    // the name baked into the registration, or the first bare argument.
    std::string provider_from_name(std::string_view name) {
        constexpr std::string_view prefix = "ai.";
        constexpr std::string_view suffix = ".loop";
        if (name.starts_with(prefix) && name.ends_with(suffix) && name.size() > prefix.size() + suffix.size()) {
            return std::string(name.substr(prefix.size(), name.size() - prefix.size() - suffix.size()));
        }
        return {};
    }

    loop_cli parse_loop_args(view const& v) {
        loop_cli cli;
        cli.provider = provider_from_name(v.name);

        std::size_t i = 0;
        if (cli.provider.empty() && !v.args.empty()) {
            std::string_view const first = v.args[0];
            if (!first.empty() && first.front() != '-') {
                if (auto const canonical = canonical_provider(first)) {
                    cli.provider = *canonical;
                    i            = 1;
                }
            }
        }

        auto take_value = [&](std::string_view flag) -> std::string_view {
            if (i + 1 >= v.args.size()) {
                throw usage_error{"ai.loop: " + std::string(flag) + " needs a value\n"};
            }
            return v.args[++i];
        };

        for (; i < v.args.size(); ++i) {
            std::string_view const arg = v.args[i];
            // Separate `if`s with `continue` rather than one `else if` chain: an
            // init-statement per branch would shadow the previous one (-Wshadow),
            // and assigning inside the condition is bugprone.
            auto match_value           = [&](std::string_view flag) -> std::optional<std::string_view> {
                auto const m = match_flag(arg, flag);
                if (!m.hit) {
                    return std::nullopt;
                }
                return m.value.empty() ? take_value(flag) : m.value;
            };

            if (auto value = match_value("--max-turns")) {
                cli.loop.max_turns = static_cast<int>(parse_positive("--max-turns", *value));
                continue;
            }
            if (auto value = match_value("--max-calls")) {
                cli.loop.max_calls_per_turn = parse_positive("--max-calls", *value);
                continue;
            }
            if (auto value = match_value("--tool-head")) {
                cli.loop.tool_head_lines = parse_positive("--tool-head", *value);
                continue;
            }
            if (auto value = match_value("--tool-bytes")) {
                cli.loop.tool_max_bytes = parse_positive("--tool-bytes", *value);
                continue;
            }
            if (auto value = match_value("--tools")) {
                cli.tools = std::string(*value);
                continue;
            }
            if (auto value = match_value("--provider")) {
                cli.provider = std::string(*value);
                continue;
            }
            if (auto value = match_value("--fallback-model")) {
                cli.fallback_model = std::string(*value);
                continue;
            }
            if (arg == "--read-only") {
                cli.read_only = true;
            } else if (arg == "--agents") {
                cli.agents = true;
            } else if (arg == "--real-paths") {
                cli.real_paths = true;
            } else if (arg == "--show-tools") {
                cli.loop.show_tools = true;
            } else if (arg == "--transcript") {
                cli.loop.transcript = true;
            } else if (arg == "--yes") {
                cli.yes = true;
            } else if (arg == "--print-prompt") {
                cli.print_prompt = true;
            } else if (arg == "--any" || arg == "--global") {
                cli.any = true;
            } else if (arg != "--here") { // --here is the default
                cli.forward.push_back(arg);
            }
        }
        return cli;
    }

    // Whether the catalog built for this loop includes a file-editing tool.
    // `--read-only` removes them; `--tools` is a comma-separated allow-list.
    bool loop_can_edit(loop_cli const& cli) {
        if (cli.read_only) {
            return false;
        }
        if (cli.tools.empty()) {
            return true;
        }
        std::string_view list = cli.tools;
        while (!list.empty()) {
            auto const comma = list.find(',');
            auto       name  = list.substr(0, comma);
            while (!name.empty() && name.front() == ' ') {
                name.remove_prefix(1);
            }
            while (!name.empty() && name.back() == ' ') {
                name.remove_suffix(1);
            }
            if (name == "edit" || name == "write") {
                return true;
            }
            if (comma == std::string_view::npos) {
                break;
            }
            list.remove_prefix(comma + 1);
        }
        return false;
    }

    std::string build_agent_message(view const& v, loop_cli const& cli, alias::table const& aliases) {
        std::string out;
        out += "# Agent\n\n";
        out += "You are the planner of a coding agent. You do NOT run anything yourself and\n";
        out += "you have no terminal of your own. Instead you emit a JSON tool call; an\n";
        out += "external harness runs it in the user's workspace and sends you the output as a\n";
        out += "`### Tool: <name>` message. So never say a tool is unavailable or that you\n";
        out += "cannot access the workspace: just emit the call and the harness will run it.\n";
        out += "Work until the task is complete, then answer in plain prose with NO tool block.\n";
        out += "Your first response must be a tool call (not prose, not a refusal, not a\n";
        out += "report) unless the task is purely conversational.\n\n";

        out += alias::workspace_header(v.env.working_dir, v.env.git_root, aliases);
        out += "\nRules:\n";
        out += "- Read a file before you change it; use `read`, `grep` and `glob` to find things.\n";
        out += "- Use `edit` for a targeted change and `write` to create or replace a whole file.\n";
        out += "- Use `cli` to run commands (build, tests, git).\n";
        out += "- Batch independent calls into one answer; split them only when one depends on another.\n";
        out += "- Never guess a file's contents.\n";
        if (!aliases.empty()) {
            out += "- The working directory is shown as a placeholder path (for example\n";
            out += "  `/prompt-ws-…`). It is not a real directory: the tools below translate it to\n";
            out += "  the real workspace. Pass paths exactly as shown to `read`/`grep`/`glob`/`edit`.\n";
        }
        out += "- Use ONLY the tools below. Do not use any built-in code interpreter, Python\n";
        out += "  sandbox, browser, or file tool: they run elsewhere and cannot see this\n";
        out += "  workspace, so they will always report that the files do not exist. If a tool\n";
        out += "  result surprises you, call `glob` or `read` instead of giving up.\n";
        out += "- Do not produce a report, PDF, Word document, canvas, or summary instead of\n";
        out += "  doing the work. When the task says to edit, make the edit with `edit` (or\n";
        out += "  `write`) and then say what changed.\n";
        out += "- To run a build, test, or git command, use the `cli` tool; it runs in this\n";
        out += "  workspace. Never claim a tool is unavailable before trying it. For example:\n";
        out += "  ```json\n";
        out += "  {\"tool\": \"cli\", \"input\": {\"command\": [\"git\", \"status\"]}}\n";
        out += "  ```\n";
        out += "- The files quoted in the task are a snapshot for reference. The authoritative\n";
        out += "  files are on disk: always `read` a file before you `edit` it.\n";

        std::vector<std::string> catalog_args{"--for", "chat"};
        if (!cli.tools.empty()) {
            catalog_args.emplace_back("--only");
            catalog_args.push_back(cli.tools);
        } else if (cli.read_only) {
            catalog_args.emplace_back("--exclude");
            catalog_args.emplace_back("edit,write,cli,run");
        }
        auto const catalog = run_sub_prompt(v, "tools", catalog_args);
        text::append_block(out, catalog.out);

        if (cli.agents && !v.env.git_root.empty()) {
            auto const agents = run_sub_prompt(v, "agents", {});
            text::append_block(out, agents.out);
        }

        out += "\n# Task\n\n";
        out += v.text;
        out += "\n\n";
        // Say what this session can actually do. A prompt such as `.clang-tidy`
        // must not guess whether editing tools were granted; the sink knows the
        // catalog it built above.
        if (loop_can_edit(cli)) {
            out += "This session HAS the edit, write and cli tools. Apply the requested\n";
            out += "changes directly to the files named in the task; do not answer with only\n";
            out += "an explanation, a patch, or a report.\n";
        } else {
            out += "This session has NO editing tools. Explain the issues and provide patches\n";
            out += "instead of trying to edit files.\n";
        }
        out += "To act, reply with a fenced ```json tool call using ONLY the tools listed\n";
        out += "above. Do not use a built-in interpreter or sandbox.\n";
        return aliases.hide(out);
    }

    void execute_ai_loop(view const& v, result& r) {
        loop_cli const cli = parse_loop_args(v);

        std::string provider = cli.provider;
        if (provider.empty()) {
            if (auto const prior = v.prior.get(keys::ai_provider); prior && *prior != "debate") {
                if (auto const canonical = canonical_provider(*prior)) {
                    provider = *canonical;
                }
            }
        }
        if (provider.empty()) {
            if (auto const last = last_ai_provider(cli.any); last && *last != "debate") {
                if (auto const canonical = canonical_provider(*last)) {
                    provider = *canonical;
                }
            }
        }
        if (provider.empty()) {
            throw usage_error{"ai.loop: no provider given and no chat found; name one, e.g. `.ai.loop gap`\n"};
        }

        alias::table const aliases = cli.real_paths ? alias::table{} : alias::for_environment(v.env);
        std::string const  initial = build_agent_message(v, cli, aliases);
        if (cli.print_prompt) {
            r.out = initial;
            r.err = aliases.describe();
            return;
        }

        auto setup = ai::prepare_sink(provider, cli.forward);
        if (setup.options.list) {
            r.out = ai::list_models_text(*setup.instance, setup.options);
            return;
        }
        if (setup.options.print_session) {
            r.out = ai::session_report_text(*setup.instance, setup.options);
            return;
        }
        auto session = ai::open_sink_session(std::move(setup));

        // Web providers can route a task to their own site agent (GapGPT's
        // `code-run`/`canvas`), which ignores the JSON tool protocol and repeats
        // itself. A different model usually follows the protocol; fall back to
        // one after the first site-tool turn unless the user chose a model.
        std::string fallback_model = cli.fallback_model;
        bool const  explicit_model = std::ranges::any_of(cli.forward, [](std::string_view arg) {
            return arg == "-m" || arg == "--model" || arg.starts_with("--model=");
        });
        if (fallback_model.empty() && !explicit_model && session.canonical == "gap") {
            fallback_model = "A-GPT-4.1";
        }

        int                    turn_number       = 0;
        bool                   switched_model    = false;
        bool                   send_initial_next = false;
        bool                   saw_no_action     = false;
        ai::agent_loop_io      io;
        ai::agent_loop_options loop = cli.loop;
        loop.tool_result_prefix =
          "The tool results are below. Continue the task: emit the next fenced ```json\n"
          "call, or answer without a tool block when done. Do not repeat a call whose\n"
          "result is already shown, and do not use a built-in sandbox.\n\n";
        // Web models sometimes fall back to their site's own agent and refuse
        // ("I couldn't inspect or edit f.cpp in the workspace") when the task
        // arrives with embedded file content. One corrective turn usually gets
        // them back to emitting a tool call.
        loop.refusal_retries = 3;
        loop.refusal_nudge =
          "STOP. Do not use any built-in or site-side Python sandbox, `code-run`,\n"
          "`canvas`, file upload, or other built-in tool. They run on a different\n"
          "machine and cannot see this workspace; using them is the bug. You are the\n"
          "planner of an external coding harness. The file content in the previous\n"
          "message is only a reference snapshot; the real file is on disk and the\n"
          "harness will run your call against it. Emit exactly one fenced ```json\n"
          "tool call now, using the tools from the first message, for example\n"
          "```json\n"
          "{\"tool\": \"edit\", \"input\": {\"path\": \"path/to/file\", \"old\": \"exact text\", \"new\": "
          "\"replacement\"}}\n"
          "```\n"
          "Do not explain, do not paste a patch, and do not reply DONE until a tool has\n"
          "actually run. If the task is already complete, reply with exactly: DONE\n";
        // A planner that answers with a patch instead of acting has not done
        // the task. Only worth nudging when this session can actually edit.
        loop.nudge_on_no_action = loop_can_edit(cli);
        loop.completion_marker  = "DONE";
        io.turn                 = [&](std::string_view prompt) {
            ++turn_number;
            write_stderr(
              v,
              r,
              "ai.loop: turn " + std::to_string(turn_number) + "/" + std::to_string(cli.loop.max_turns) + "\n");
            std::string_view const actual = send_initial_next ? std::string_view{initial} : prompt;
            send_initial_next             = false;
            auto turn_options             = ai::make_turn_options(session.options, *session.chat);
            auto out                      = [&v, &r](std::string_view chunk) {
                write_stdout(v, r, chunk);
            };
            auto err = [&v, &r](std::string_view chunk) {
                write_stderr(v, r, chunk);
            };
            auto result = ai::run_turn(*session.chat, *session.instance, actual, turn_options, out, err);

            bool const no_action = !result.used_site_tools && tool_call::extract(result.answer).empty();
            if (!switched_model && !fallback_model.empty() &&
                (result.used_site_tools || (saw_no_action && no_action && loop.nudge_on_no_action)))
            {
                switched_model = true;
                std::vector<std::string> args;
                for (std::size_t j = 0; j < cli.forward.size(); ++j) {
                    std::string_view const arg = cli.forward[j];
                    if (arg == "-m" || arg == "--model") {
                        ++j;
                        continue;
                    }
                    if (arg.starts_with("--model=")) {
                        continue;
                    }
                    args.emplace_back(arg);
                }
                args.emplace_back("-m");
                args.push_back(fallback_model);
                text::string_views const fallback_args(args);
                try {
                    auto fallback     = ai::open_sink_session(ai::prepare_sink(provider, fallback_args));
                    session           = std::move(fallback);
                    send_initial_next = true;
                    write_stderr(v,
                                 r,
                                 "ai.loop: the model did not use the tools; retrying with " + fallback_model + "\n");
                } catch (std::exception const& failure) {
                    write_stderr(v, r, "ai.loop: could not switch to " + fallback_model + ": " + failure.what() + "\n");
                }
            }
            saw_no_action = no_action;
            return result;
        };
        std::unordered_set<std::size_t> seen_results;
        int                             repeated_results = 0;
        bool                            last_edit_failed = false;
        io.tools_failed                                  = [&last_edit_failed] {
            return last_edit_failed;
        };
        io.run_tools = [&v, &cli, &aliases, &seen_results, &repeated_results, &last_edit_failed](
                         std::string_view answer) -> std::optional<std::string> {
            tool_exec::options exec_options;
            exec_options.yes       = cli.yes;
            exec_options.max_calls = cli.loop.max_calls_per_turn;
            auto const outcome     = tool_exec::run(v, aliases.reveal(answer), exec_options);
            if (outcome.calls.empty()) {
                return std::nullopt;
            }
            // A failed edit/write/read means the task is not done, so a later
            // "DONE" must not end the loop; let the agent loop nudge instead.
            last_edit_failed    = std::ranges::any_of(outcome.failed_calls, [](std::string const& name) {
                return name == "edit" || name == "write" || name == "read";
            });
            std::string blocks  = aliases.hide(outcome.blocks);
            // A web model can get stuck re-emitting a call whose result it has
            // already seen. Feeding the same block back repeats our own text
            // and burns turns, so skip it and tell the model plainly to move on.
            auto const inserted = seen_results.insert(std::hash<std::string>{}(blocks));
            if (!inserted.second) {
                ++repeated_results;
                std::string note  = "### Tool: repeat\nStatus: skipped (";
                note             += std::to_string(repeated_results);
                note             += ")\nThis exact call already ran and its result is above. Do not call it again. ";
                note             += "Make a different call or answer without a tool block.\n";
                return aliases.hide(note);
            }
            return blocks;
        };
        io.status = [&v, &r](std::string_view text) {
            write_stderr(v, r, text);
        };

        auto const result = ai::run_agent_loop(initial, loop, io);

        if (result.status != 0) {
            throw prompt_error{result.status, "ai.loop: " + result.error + "\n", r.out, r.no_clipboard, v.in.piped()};
        }
        if (result.exhausted) {
            write_stderr(v,
                         r,
                         "ai.loop: stopped after " + std::to_string(result.turns) +
                           " turns while the model was still calling tools\n");
        }

        ai::save_sink_state(session);
        r.out = aliases.reveal(cli.loop.transcript ? result.transcript : result.answer);
        r.published.set(keys::ai_provider, provider);
        if (!aliases.empty()) {
            r.published.set(keys::path_alias, alias::encode(aliases));
        }
        if (!r.out.empty()) {
            r.published.set(keys::ai_answer, r.out);
        }
    }

    void render_help_ai_loop(std::ostream& out) {
        out << "Usage: prompt <other prompts...> .ai.loop [PROVIDER] [LOOP-OPTIONS] [SINK-OPTIONS] [PROVIDER-OPTIONS]\n"
            << "       prompt .ai.<provider>.loop [LOOP-OPTIONS] [SINK-OPTIONS] [PROVIDER-OPTIONS]\n"
            << "\n"
            << "Run an agentic tool loop: send the task to the AI, run the tool calls in its\n"
            << "answer, feed the results back, and repeat until it answers without a call.\n"
            << "\n"
            << "  prompt .note \"add a --dry-run flag to the save prompt\" .ai.gap.loop\n"
            << "\n"
            << "The first message carries the agent preamble, the `.tools --for chat` catalog\n"
            << "(read, grep, glob, files, edit, write, cli, run), the project's AGENTS.md\n"
            << "(`--agents`) and the chain context. Absolute paths are replaced with a stable\n"
            << "placeholder workspace (`/prompt-ws-…`) and translated back on every tool call;\n"
            << "`--real-paths` sends the real ones. Each later turn carries only the\n"
            << "`### Tool:` results, so the conversation stays coherent. The final answer\n"
            << "becomes the chain context and the clipboard; the streamed turns are\n"
            << "display-only. The chat is saved, so a later `.ai.reply` continues it.\n"
            << "\n"
            << "The provider is the one baked into the alias (`.ai.gap.loop`), else a leading\n"
            << "PROVIDER or --provider, else the AI sink earlier in the chain, else the chat\n"
            << "this terminal used last. `--any` reads the globally newest chat.\n"
            << "\n";
        render_sink_help(out);
        out << "\n"
            << "Loop options:\n"
            << "  --max-turns N            Assistant turns before giving up (default 25)\n"
            << "  --max-calls N            Tool calls per turn (default 16)\n"
            << "  --tool-head N            Lines kept per tool result (default 400)\n"
            << "  --tool-bytes N           Bytes kept per tool result (default 65536)\n"
            << "  --tools NAME[,NAME...]   Only these tools in the catalog\n"
            << "  --read-only              Drop edit/write/cli/run from the catalog\n"
            << "  --agents                 Embed the project's AGENTS.md and friends\n"
            << "  --real-paths             Send real absolute paths, not a placeholder workspace\n"
            << "  --show-tools             Print each tool result on stderr\n"
            << "  --transcript             Final context is the whole loop, not the last answer\n"
            << "  --yes                    Turn policy asks into allows (never a deny)\n"
            << "  --print-prompt           Print the assembled first message and exit\n"
            << "  --provider NAME          Use this provider\n"
            << "  --fallback-model NAME    Switch to NAME if the model uses its site tools\n"
            << "                           or refuses to act (GapGPT defaults to A-GPT-4.1\n"
            << "                           when no -m is given)\n"
            << "  --any, --global          Infer the provider from the globally newest chat\n"
            << "  --here                   Infer it from this terminal (the default)\n"
            << "\n"
            << "Notes:\n"
            << "  - Read-only tools run freely; edit/write/cli/run go through the policy\n"
            << "    (~/.config/prompt/policy, <git-root>/.prompt/policy), which asks by default.\n"
            << "  - A failed tool call is fed back as `Status: failed`, so the model can retry.\n"
            << "  - Every provider option (`-m`, `-s`, `-c`, `--new`, `--timeout`, …) is forwarded.\n"
            << "\n"
            << "Examples:\n"
            << "  prompt .note \"fix the failing test\" .ai.gap.loop\n"
            << "  prompt .note \"review app/prompt/main.cpp\" .ai.dseek.loop --read-only\n"
            << "  prompt .note \"...\" .ai.loop gap --max-turns 10 --yes\n"
            << "  prompt .note \"...\" .ai.gap.loop --print-prompt\n";
    }

    void complete_ai_loop(dana::completion_request const& request, std::ostream& out) {
        using dana::completion::item;
        static constexpr std::array<item, 15> kProviders = {
          {
           {.value = "chatgpt", .description = "ChatGPT"},
           {.value = "claude", .description = "Claude"},
           {.value = "dseek", .description = "DeepSeek"},
           {.value = "deepseek", .description = "DeepSeek (alias)"},
           {.value = "gap", .description = "GapGPT"},
           {.value = "gemini", .description = "Gemini"},
           {.value = "grok", .description = "Grok"},
           {.value = "kimi", .description = "Kimi"},
           {.value = "perplexity", .description = "Perplexity"},
           {.value = "pplx", .description = "Perplexity (alias)"},
           {.value = "lms", .description = "LM Studio (local)"},
           {.value = "lmstudio", .description = "LM Studio (alias)"},
           {.value = "openai", .description = "OpenAI API"},
           {.value = "oai", .description = "OpenAI API (alias)"},
           {.value = "openrouter", .description = "OpenRouter"},
           },
        };
        if (dana::completion::write_values_after_flag(out, request, "--provider", kReplyProviders)) {
            return;
        }
        dana::completion::write_items(out, request.prefix, kProviders);
    }

    void execute_ai_argv(
      view const&        v,
      result&            r,
      std::string_view   provider,
      text::string_views args,
      std::string_view   context) {
        // A `.paths.hide` earlier in the chain published the mapping: hide what the
        // model reads, reveal what the user gets back. `--print-prompt` must show
        // exactly what would be sent, so it keeps the hidden form.
        alias::table aliases;
        if (auto const fact = v.prior.get(keys::path_alias)) {
            if (auto decoded = alias::decode(*fact)) {
                aliases = std::move(*decoded);
            }
        }
        bool const        inspect        = inspect_only(args);
        std::string const hidden_context = inspect ? std::string(context) : aliases.hide(context);

        if (native_ai_provider(provider)) {
            execute_ai_native(v, r, provider, args, hidden_context);
            if (!inspect && !aliases.empty()) {
                r.out = aliases.reveal(r.out);
                if (!r.out.empty()) {
                    r.published.set(keys::ai_answer, r.out);
                }
            }
            return;
        }
        std::string const tool_name = "ai." + std::string(provider);
        auto const        tool      = fs::bin_tool(tool_name, v.env.exe_path, v.env.git_root);
        if (tool.empty() || !std::filesystem::exists(tool)) {
            throw soft_error{"ai." + std::string(provider) + ": " + tool_name + " not found\n"};
        }

        std::vector<std::string> argv;
        argv.reserve(args.size() + 5);
        argv.push_back(tool.string());
        // The chain context is stdin. This flag makes bin/ai reject positional
        // prompt text instead of silently ignoring the context.
        argv.emplace_back("--stdin-only");
        // We forward the bytes to the real terminal, so render for one.
        if (::isatty(STDOUT_FILENO) != 0) {
            argv.emplace_back("--tty");
            add_terminal_size(argv);
        }
        bool const  chainable = !inspect_only(args);
        answer_file answer;
        if (chainable) {
            argv.emplace_back("--context-file=" + answer.path().string());
        }
        // Mirror every prompt-driven chat under its terminal, so `.ai.reply` can
        // continue exactly the one this terminal used. The debate tool mirrors its
        // participants itself and has no such flag.
        if (provider != "debate") {
            argv.emplace_back("--terminal-scope=" + terminal::scope(terminal::current()));
        }
        for (std::string_view const arg : args) {
            if (arg == "-i" || arg == "--interactive") {
                throw usage_error{"ai." + std::string(provider) +
                                  ": -i/--interactive cannot be used in a prompt chain; run it directly\n"};
            }
            argv.emplace_back(arg);
        }

        auto const result = run_sink_tool(v, r, std::move(argv), hidden_context, std::chrono::milliseconds{0});

        if (chainable) {
            // The styled stream already reached the terminal; the chain context and
            // the clipboard take the plain answer instead. An empty answer still
            // replaces the (styled) stream, so no frames leak onward.
            if (auto plain = answer.take()) {
                r.out = std::move(*plain);
            }
        }

        if (result.exit_code != 0 || result.timed_out) {
            int code = 1;
            if (!result.timed_out && result.exit_code > 0) {
                code = result.exit_code;
            }
            std::string message = "ai." + std::string(provider) + ": ";
            if (result.timed_out) {
                message += "timed out\n";
            } else {
                message += "exited with status " + std::to_string(result.exit_code) + "\n";
            }
            throw prompt_error{code, std::move(message), r.out, r.no_clipboard, v.in.piped()};
        }

        if (chainable) {
            // Facts for a later `.ai.reply` in the same chain: which provider just
            // answered, and the answer it already holds. The answer is revealed so
            // it matches the (revealed) chain context `.ai.reply` compares against.
            if (!aliases.empty()) {
                r.out = aliases.reveal(r.out);
            }
            r.published.set(keys::ai_provider, provider);
            if (!r.out.empty()) {
                r.published.set(keys::ai_answer, r.out);
            }
        }
    }

    void execute_ai(view const& v, result& r, std::string_view provider) {
        execute_ai_argv(v, r, provider, v.args, v.text);
    }

    // `.ai.reply`: continue the chat this terminal talked to last (or the one the
    // previous AI sink in this chain started), sending only the newly appended
    // text. Provider options are forwarded unchanged; `-c` and `--reply` are ours.
    void execute_ai_reply(view const& v, result& r) {
        std::string const             prefix = "ai.reply: ";
        std::string                   provider;
        bool                          global = false;
        std::vector<std::string_view> forward;

        for (std::size_t i = 0; i < v.args.size(); ++i) {
            std::string_view const arg = v.args[i];
            if (arg == "--provider") {
                if (i + 1 >= v.args.size()) {
                    throw usage_error{prefix + "--provider needs a name\n"};
                }
                provider = std::string(v.args[++i]);
                continue;
            }
            if (arg.starts_with("--provider=")) {
                provider = std::string(arg.substr(std::string_view{"--provider="}.size()));
                continue;
            }
            if (arg == "--any" || arg == "--global") {
                global = true;
                continue;
            }
            if (arg == "--here") {
                continue; // default
            }
            forward.push_back(arg);
        }

        // A leading bare provider (`prompt .ai.reply gap`) is friendlier than the
        // explicit flag; anything else stays a provider option.
        if (provider.empty() && !forward.empty()) {
            if (auto const canonical = canonical_provider(forward.front())) {
                provider = *canonical;
                forward.erase(forward.begin());
            }
        }

        if (provider.empty()) {
            if (auto const prior = v.prior.get(keys::ai_provider)) {
                if (*prior == "debate") {
                    throw usage_error{prefix + "the last AI here was a debate; name a provider, e.g. .ai.reply grok\n"};
                }
                if (auto const canonical = canonical_provider(*prior)) {
                    provider = *canonical;
                }
            }
        }
        if (provider.empty()) {
            if (auto const last = last_ai_provider(global)) {
                if (*last == "debate") {
                    throw usage_error{
                      prefix + "the last AI chat here was a debate; name a provider, e.g. .ai.reply grok\n"};
                }
                if (auto const canonical = canonical_provider(*last)) {
                    provider = *canonical;
                } else {
                    throw soft_error{prefix + "unknown provider in the saved terminal state: " + *last + "\n"};
                }
            }
        }
        if (provider.empty()) {
            throw soft_error{prefix + "no AI chat found" + (global ? " yet" : " in this terminal yet") +
                             "; start one with .ai.<provider>, or name a provider\n"};
        }

        // The chat already holds the previous answer, so sending it again would
        // duplicate it. Drop it when it is exactly the context prefix (the usual
        // `<answer> + blank line + new instruction` shape) and send only the rest.
        std::string_view context = v.text;
        if (auto const previous = v.prior.get(keys::ai_answer)) {
            std::string_view answer = *previous;
            while (!answer.empty() && (answer.back() == '\n' || answer.back() == '\r')) {
                answer.remove_suffix(1);
            }
            auto const is_space = [](char c) {
                return c == '\n' || c == '\r' || c == ' ' || c == '\t';
            };
            if (!answer.empty() && context.starts_with(answer)) {
                std::string_view rest = context.substr(answer.size());
                if (rest.empty() || is_space(rest.front())) {
                    while (!rest.empty() && is_space(rest.front())) {
                        rest.remove_prefix(1);
                    }
                    context = rest;
                }
            }
        }
        if (context.empty()) {
            throw usage_error{prefix + "nothing new to send; put the reply in a .note before .ai.reply\n"};
        }

        std::vector<std::string_view> args;
        args.reserve(forward.size() + 2);
        args.emplace_back("-c");
        // `--any` continues the globally newest chat, which a terminal mirror may
        // not hold; the default continues this terminal's own chat.
        if (!global) {
            args.emplace_back("--reply");
        }
        args.insert(args.end(), forward.begin(), forward.end());
        execute_ai_argv(v, r, provider, args, context);
    }

    // The API providers speak an OpenAI-compatible HTTP API, so they have no
    // browser session, no upload endpoint and no prompt-file fallback: the flags
    // are the endpoint, the key and the raw inference parameters.
    void render_api_provider_options(std::ostream& out, ai_provider const& provider) {
        out << "Provider options (passed to ai." << provider.name << "):\n"
            << "  -f, --file PATH             Attach a local file to the first message: text is\n"
            << "                              inlined, images become data URLs (repeatable)\n"
            << "  -m, --model NAME            Model id (default: $" << provider.model_env;
        if (!provider.default_model.empty()) {
            out << ", else " << provider.default_model;
        }
        if (provider.local) {
            out << ", else the first chat model the server lists";
        }
        out << ")\n"
            << "  -s, --system TEXT           System / custom instructions\n"
            << "  -c, --continue              Continue the last conversation with this provider\n"
            << "  --new                       Start a fresh conversation\n"
            << "  --base-url URL              API root (default: $" << provider.base_url_env;
        if (provider.local) {
            out << ", then the cached\n"
                << "                              endpoint, then the usual local ports)\n";
        } else {
            out << ", then\n"
                << "                              " << provider.base_url << ")\n";
        }
        out << "  --api-key KEY               API key (default: $" << provider.key_env << ")\n"
            << "  --param KEY=VALUE           Extra request parameter, e.g. temperature=0.2 (repeatable)\n"
            << "  --show-thinking             Show the model's reasoning on stderr\n"
            << "  --no-thinking               Hide the model's reasoning\n"
            << "  --json                      Emit one JSON event per line\n"
            << "  --raw                       Print only the answer text\n"
            << "  --tty                       Render as if stdout/stderr were terminals (pipe-friendly)\n"
            << "  --copy                      Copy the answer to the clipboard\n"
            << "  --timeout SECONDS           Request timeout (default: 600)\n"
            << "  --print-prompt              Print the prompt that would be sent and exit\n"
            << "  --print-session             Show the endpoint, model and reachability, then exit\n"
            << "  --list                      List the available providers and models, then exit\n"
            << "  -h, --help                  Show this help message\n"
            << "\n"
            << "Notes:\n"
            << "  - Use a preceding .note (or --system) for instructions; positional prompt text\n"
            << "    is rejected because stdin carries the chain context.\n"
            << "  - The model's reasoning stays on stderr and is never part of the chain context.\n"
            << "  - There is no browser session and no upload endpoint: -f files are inlined\n"
            << "    into the message, --prompt-file is refused, and an oversized prompt is sent\n"
            << "    inline (raise the model's context length instead).\n";
        if (provider.local) {
            out << "  - LM Studio must be running its local server (`lms server start`, or\n"
                << "    Developer → Local Server). The endpoint comes from --base-url, $" << provider.base_url_env
                << ", the\n"
                << "    cached one, then the usual local ports, then `lms status`.\n";
        } else {
            out << "  - Any OpenAI-compatible endpoint works (OpenRouter, Groq, vLLM): set $" << provider.base_url_env
                << "\n"
                << "    to its API root and $OPENAI_MODEL to its model id.\n";
        }
        out << "  - --show-context prints the source prompts in the final output; --print-prompt\n"
            << "    prints exactly what this sink receives.\n";
    }

    void render_help_ai_reply(std::ostream& out) {
        out << "Usage: prompt <other prompts...> .ai.reply [PROVIDER] [--provider NAME] [--any] [SINK-OPTIONS] "
               "[PROVIDER-OPTIONS]\n"
            << "       prompt .ai.reply [PROVIDER] [--provider NAME] [--any] [SINK-OPTIONS] [PROVIDER-OPTIONS]\n"
            << "\n"
            << "Reply in the AI chat this terminal talked to last and use its answer as the new\n"
            << "chain context, e.g. after `.ai.gap`:\n"
            << "  prompt .note \"implement it; switch to build mode\" .ai.reply\n"
            << "\n"
            << "The provider is inferred: an explicit PROVIDER/--provider, else the AI sink\n"
            << "earlier in the same chain, else the chat this terminal sent last (read from the\n"
            << "per-terminal state bin/ai keeps). `--any` reads the globally newest chat. Every\n"
            << "provider option is forwarded as `.ai.<provider>` would receive it; the `-c` and\n"
            << "`--reply` state flags are supplied for you.\n"
            << "\n";
        render_sink_help(out);
        out << "\n"
            << "Reply options:\n"
            << "  PROVIDER, --provider NAME  Use this provider: chatgpt, claude, dseek, gap, gemini, grok,\n"
            << "                             kimi, perplexity, lms, openai, openrouter\n"
            << "  --any, --global            Use the globally newest chat instead of this terminal's\n"
            << "  --here                     Require this terminal (the default)\n"
            << "  -h, --help                 Show this help message\n"
            << "\n"
            << "Notes:\n"
            << "  - Chained right after an AI sink, the previous answer is dropped from the\n"
            << "    message: only the text appended after it (usually a `.note`) is sent.\n"
            << "  - All provider options are accepted (`-m`, `-s`, `-f`, `--new`, `--show-thinking`,\n"
            << "    `--print-session`, …); see `.ai.<provider> --help`.\n"
            << "  - This continues the single-provider chats. When the last chat here was a\n"
            << "    debate, name a provider (`.ai.reply grok`) to continue its side.\n"
            << "\n"
            << "Examples:\n"
            << "  prompt .note \"implement it\" .ai.reply\n"
            << "  prompt ... .plan .ai.gap .note \"impl it; switch to build mode\" .ai.reply\n"
            << "  prompt .note \"continue on a smaller model\" .ai.reply --provider dseek -m deepseek-chat\n"
            << "  prompt .last.ai .note \"compare with the above\" .ai.reply\n";
    }

    // The provider `.ai.reply` will continue, as far as the typed arguments say (an
    // explicit `--provider NAME` or a leading PROVIDER). Used only to pick the model
    // list after `-m`; the runtime resolution also consults the chain facts and the
    // terminal marker.
    std::optional<std::string> reply_provider_from_args(text::string_views args) {
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (args[i] == "--provider" && i + 1 < args.size()) {
                return canonical_provider(args[i + 1]);
            }
            if (args[i].starts_with("--provider=")) {
                return canonical_provider(args[i].substr(11));
            }
        }
        for (auto const& arg : args) {
            if (arg.empty() || arg.front() == '-') {
                continue;
            }
            if (auto const provider = canonical_provider(arg)) {
                return provider;
            }
        }
        return std::nullopt;
    }

    void complete_ai_reply(dana::completion_request const& request, std::ostream& out) {
        using dana::completion::item;
        static constexpr std::array<item, 15> kProviders = {
          {
           {.value = "chatgpt", .description = "ChatGPT"},
           {.value = "claude", .description = "Claude"},
           {.value = "dseek", .description = "DeepSeek"},
           {.value = "deepseek", .description = "DeepSeek (alias)"},
           {.value = "gap", .description = "GapGPT"},
           {.value = "gemini", .description = "Gemini"},
           {.value = "grok", .description = "Grok"},
           {.value = "kimi", .description = "Kimi"},
           {.value = "perplexity", .description = "Perplexity"},
           {.value = "pplx", .description = "Perplexity (alias)"},
           {.value = "lms", .description = "LM Studio (local)"},
           {.value = "lmstudio", .description = "LM Studio (alias)"},
           {.value = "openai", .description = "OpenAI API"},
           {.value = "oai", .description = "OpenAI API (alias)"},
           {.value = "openrouter", .description = "OpenRouter"},
           },
        };
        if (dana::completion::write_values_after_flag(out, request, "--provider", kReplyProviders)) {
            return;
        }
        if (auto const provider = reply_provider_from_args(request.prior_args)) {
            if (complete_ai_model(request, out, *provider)) {
                return;
            }
        } else if (
          !request.prior_args.empty() && (request.prior_args.back() == "-m" || request.prior_args.back() == "--model"))
        {
            // No provider named yet, so there is no model list to offer.
            return;
        }
        dana::completion::write_items(out, request.prefix, kProviders);
    }

    void render_help_ai(std::ostream& out, ai_provider const& provider) {
        out << "Usage: prompt <other prompts...> .ai." << provider.name << " [SINK-OPTIONS] [PROVIDER-OPTIONS]\n"
            << "       prompt .ai." << provider.name << " [SINK-OPTIONS] [PROVIDER-OPTIONS]\n"
            << "\n"
            << "Send the accumulated chain context to " << provider.label
            << " and use the answer as the new chain context.\n"
            << "\n"
            << "The prompt is a sink: it waits until every prompt before it has run, sends their\n"
            << "combined text to " << provider.label << ", and (by default) replaces the running\n"
            << "context with its answer. Sinks may appear anywhere in a chain.\n"
            << "\n";
        render_sink_help(out);
        out << "\n";
        if (provider.api) {
            render_api_provider_options(out, provider);
            return;
        }
        out << "Provider options (passed to ai." << provider.name << "):\n"
            << "  -f, --file PATH             Upload and attach a local file (repeatable)\n"
            << "  --prompt-file               Force uploading the prompt as a text file\n"
            << "  --no-prompt-file            Never upload the prompt as a text file\n"
            << "  --prompt-limit BYTES        Override the automatic prompt-file threshold\n"
            << "  -m, --model NAME            Provider model\n"
            << "  -s, --system TEXT           System / custom instructions\n"
            << "  -c, --continue              Continue the last conversation with this provider\n"
            << "  --new                       Start a fresh conversation\n"
            << "  --show-thinking             Show the model's reasoning on stderr\n"
            << "  --no-thinking               Hide the model's reasoning\n"
            << "  --json                      Emit one JSON event per line\n"
            << "  --raw                       Print only the answer text\n"
            << "  --tty                       Render as if stdout/stderr were terminals (pipe-friendly)\n"
            << "  --copy                      Copy the answer to the clipboard\n"
            << "  --timeout SECONDS           Request timeout (default: 600)\n"
            << "  --print-prompt              Print the prompt that would be sent and exit\n"
            << "  --list                      List available providers and exit\n"
            << "  --print-session             Show the browser session that would be used\n"
            << "  -h, --help                  Show this help message\n"
            << "\n"
            << "Notes:\n"
            << "  - Use a preceding .note (or --system) for instructions; positional prompt text\n"
            << "    is rejected because stdin carries the chain context.\n"
            << "  - The model's reasoning stays on stderr and is never part of the chain context.\n"
            << "  - --show-context prints the source prompts in the final output; --print-prompt\n"
            << "    prints exactly what this sink receives.\n";
    }

    constexpr ai_provider kChatgpt{.name = "chatgpt", .label = "ChatGPT"};
    constexpr ai_provider kClaude{.name = "claude", .label = "Claude"};
    constexpr ai_provider kDseek{.name = "dseek", .label = "DeepSeek"};
    constexpr ai_provider kGap{.name = "gap", .label = "GapGPT"};
    constexpr ai_provider kGemini{.name = "gemini", .label = "Gemini"};
    constexpr ai_provider kGrok{.name = "grok", .label = "Grok"};
    constexpr ai_provider kKimi{.name = "kimi", .label = "Kimi"};
    constexpr ai_provider kPerplexity{.name = "perplexity", .label = "Perplexity"};
    constexpr ai_provider kLms{
      .name         = "lms",
      .label        = "a local LM Studio server",
      .api          = true,
      .local        = true,
      .base_url_env = "LMS_BASE_URL",
      .key_env      = "LMS_API_KEY",
      .model_env    = "LMS_MODEL",
    };
    constexpr ai_provider kOpenai{
      .name         = "openai",
      .label        = "the OpenAI API",
      .api          = true,
      .base_url     = "https://api.openai.com/v1",
      .base_url_env = "OPENAI_BASE_URL",
      .key_env      = "OPENAI_API_KEY",
      .model_env    = "OPENAI_MODEL",
    };
    constexpr ai_provider kOpenrouter{
      .name          = "openrouter",
      .label         = "OpenRouter",
      .api           = true,
      .base_url      = "https://openrouter.ai/api/v1",
      .base_url_env  = "OPENROUTER_BASE_URL",
      .key_env       = "OPENROUTER_API_KEY",
      .model_env     = "OPENROUTER_MODEL",
      .default_model = "openrouter/auto-code",
    };
    constexpr ai_provider kDebate{.name = "debate", .label = "a panel of AIs"};

    void execute_ai_chatgpt(view const& v, result& r) {
        execute_ai(v, r, kChatgpt.name);
    }

    void execute_ai_claude(view const& v, result& r) {
        execute_ai(v, r, kClaude.name);
    }

    void execute_ai_dseek(view const& v, result& r) {
        execute_ai(v, r, kDseek.name);
    }

    void execute_ai_gap(view const& v, result& r) {
        execute_ai(v, r, kGap.name);
    }

    void execute_ai_gemini(view const& v, result& r) {
        execute_ai(v, r, kGemini.name);
    }

    void execute_ai_grok(view const& v, result& r) {
        execute_ai(v, r, kGrok.name);
    }

    void execute_ai_kimi(view const& v, result& r) {
        execute_ai(v, r, kKimi.name);
    }

    void execute_ai_perplexity(view const& v, result& r) {
        execute_ai(v, r, kPerplexity.name);
    }

    void execute_ai_lms(view const& v, result& r) {
        execute_ai(v, r, kLms.name);
    }

    void execute_ai_openai(view const& v, result& r) {
        execute_ai(v, r, kOpenai.name);
    }

    void execute_ai_openrouter(view const& v, result& r) {
        execute_ai(v, r, kOpenrouter.name);
    }

    void execute_ai_debate(view const& v, result& r) {
        execute_ai(v, r, kDebate.name);
    }

    void render_help_ai_chatgpt(std::ostream& out) {
        render_help_ai(out, kChatgpt);
    }

    void render_help_ai_claude(std::ostream& out) {
        render_help_ai(out, kClaude);
    }

    void render_help_ai_dseek(std::ostream& out) {
        render_help_ai(out, kDseek);
    }

    void render_help_ai_gap(std::ostream& out) {
        render_help_ai(out, kGap);
    }

    void render_help_ai_gemini(std::ostream& out) {
        render_help_ai(out, kGemini);
    }

    void render_help_ai_grok(std::ostream& out) {
        render_help_ai(out, kGrok);
    }

    void render_help_ai_kimi(std::ostream& out) {
        render_help_ai(out, kKimi);
    }

    void render_help_ai_perplexity(std::ostream& out) {
        render_help_ai(out, kPerplexity);
    }

    void render_help_ai_lms(std::ostream& out) {
        render_help_ai(out, kLms);
    }

    void render_help_ai_openai(std::ostream& out) {
        render_help_ai(out, kOpenai);
    }

    void render_help_ai_openrouter(std::ostream& out) {
        render_help_ai(out, kOpenrouter);
    }

} // namespace

namespace {

    [[maybe_unused]] bool const registered_ai_chatgpt = dana::register_sink(
      "ai.chatgpt",
      "Send the accumulated chain context to ChatGPT and use its answer as the next context.",
      &execute_ai_chatgpt,
      &render_help_ai_chatgpt,
      &complete_ai_chatgpt);

    [[maybe_unused]] bool const registered_ai_claude = dana::register_sink(
      "ai.claude",
      "Send the accumulated chain context to Claude and use its answer as the next context.",
      &execute_ai_claude,
      &render_help_ai_claude,
      &complete_ai_claude);

    [[maybe_unused]] bool const registered_ai_dseek = dana::register_sink(
      "ai.dseek",
      "Send the accumulated chain context to DeepSeek and use its answer as the next context.",
      &execute_ai_dseek,
      &render_help_ai_dseek,
      &complete_ai_dseek);

    [[maybe_unused]] bool const registered_ai_gap = dana::register_sink(
      "ai.gap",
      "Send the accumulated chain context to GapGPT and use its answer as the next context.",
      &execute_ai_gap,
      &render_help_ai_gap,
      &complete_ai_gap);

    [[maybe_unused]] bool const registered_ai_gemini = dana::register_sink(
      "ai.gemini",
      "Send the accumulated chain context to Gemini and use its answer as the next context.",
      &execute_ai_gemini,
      &render_help_ai_gemini,
      &complete_ai_gemini);

    [[maybe_unused]] bool const registered_ai_grok = dana::register_sink(
      "ai.grok",
      "Send the accumulated chain context to Grok and use its answer as the next context.",
      &execute_ai_grok,
      &render_help_ai_grok,
      &complete_ai_grok);

    [[maybe_unused]] bool const registered_ai_kimi = dana::register_sink(
      "ai.kimi",
      "Send the accumulated chain context to Kimi and use its answer as the next context.",
      &execute_ai_kimi,
      &render_help_ai_kimi,
      &complete_ai_kimi);

    [[maybe_unused]] bool const registered_ai_perplexity = dana::register_sink(
      "ai.perplexity",
      "Send the accumulated chain context to Perplexity and use its answer as the next context.",
      &execute_ai_perplexity,
      &render_help_ai_perplexity,
      &complete_ai_perplexity);

    [[maybe_unused]] bool const registered_ai_lms = dana::register_sink(
      "ai.lms",
      "Send the accumulated chain context to a local LM Studio model and use its answer as the next context.",
      &execute_ai_lms,
      &render_help_ai_lms,
      &complete_ai_lms);

    [[maybe_unused]] bool const registered_ai_openai = dana::register_sink(
      "ai.openai",
      "Send the accumulated chain context to the OpenAI API (or any OpenAI-compatible endpoint) and use its answer as "
      "the next context.",
      &execute_ai_openai,
      &render_help_ai_openai,
      &complete_ai_openai);

    [[maybe_unused]] bool const registered_ai_openrouter = dana::register_sink(
      "ai.openrouter",
      "Send the accumulated chain context to OpenRouter and use its answer as the next context.",
      &execute_ai_openrouter,
      &render_help_ai_openrouter,
      &complete_ai_openrouter);

    [[maybe_unused]] bool const registered_ai_debate = dana::register_sink(
      "ai.debate",
      "Ask two or more AIs the same question and let them verify each other's answers, round after round.",
      &execute_ai_debate,
      &render_help_ai_debate,
      &complete_ai_debate);

    [[maybe_unused]] bool const registered_ai_reply = dana::register_sink(
      "ai.reply",
      "Reply in the last AI chat from this terminal and use its answer as the next context.",
      &execute_ai_reply,
      &render_help_ai_reply,
      &complete_ai_reply);

    // The agentic loop: `.ai.loop` plus one alias per provider, so the provider is
    // part of the prompt name and the model sees the same tools either way.
    [[maybe_unused]] bool const registered_ai_loop = dana::register_sink(
      "ai.loop",
      "Let the AI read and edit files and run commands, turn after turn, until the task is done.",
      &execute_ai_loop,
      &render_help_ai_loop,
      &complete_ai_loop);

    [[maybe_unused]] bool const registered_ai_chatgpt_loop = dana::register_sink(
      "ai.chatgpt.loop",
      "Agentic tool loop on ChatGPT: it reads and edits files and runs commands until done.",
      &execute_ai_loop,
      &render_help_ai_loop,
      &complete_ai_loop);

    [[maybe_unused]] bool const registered_ai_claude_loop = dana::register_sink(
      "ai.claude.loop",
      "Agentic tool loop on Claude: it reads and edits files and runs commands until done.",
      &execute_ai_loop,
      &render_help_ai_loop,
      &complete_ai_loop);

    [[maybe_unused]] bool const registered_ai_dseek_loop = dana::register_sink(
      "ai.dseek.loop",
      "Agentic tool loop on DeepSeek: it reads and edits files and runs commands until done.",
      &execute_ai_loop,
      &render_help_ai_loop,
      &complete_ai_loop);

    [[maybe_unused]] bool const registered_ai_gap_loop = dana::register_sink(
      "ai.gap.loop",
      "Agentic tool loop on GapGPT: it reads and edits files and runs commands until done.",
      &execute_ai_loop,
      &render_help_ai_loop,
      &complete_ai_loop);

    [[maybe_unused]] bool const registered_ai_gemini_loop = dana::register_sink(
      "ai.gemini.loop",
      "Agentic tool loop on Gemini: it reads and edits files and runs commands until done.",
      &execute_ai_loop,
      &render_help_ai_loop,
      &complete_ai_loop);

    [[maybe_unused]] bool const registered_ai_grok_loop = dana::register_sink(
      "ai.grok.loop",
      "Agentic tool loop on Grok: it reads and edits files and runs commands until done.",
      &execute_ai_loop,
      &render_help_ai_loop,
      &complete_ai_loop);

    [[maybe_unused]] bool const registered_ai_kimi_loop = dana::register_sink(
      "ai.kimi.loop",
      "Agentic tool loop on Kimi: it reads and edits files and runs commands until done.",
      &execute_ai_loop,
      &render_help_ai_loop,
      &complete_ai_loop);

    [[maybe_unused]] bool const registered_ai_perplexity_loop = dana::register_sink(
      "ai.perplexity.loop",
      "Agentic tool loop on Perplexity: it reads and edits files and runs commands until done.",
      &execute_ai_loop,
      &render_help_ai_loop,
      &complete_ai_loop);

    [[maybe_unused]] bool const registered_ai_lms_loop = dana::register_sink(
      "ai.lms.loop",
      "Agentic tool loop on a local LM Studio model: it reads and edits files and runs commands.",
      &execute_ai_loop,
      &render_help_ai_loop,
      &complete_ai_loop);

    [[maybe_unused]] bool const registered_ai_openai_loop = dana::register_sink(
      "ai.openai.loop",
      "Agentic tool loop on the OpenAI API: it reads and edits files and runs commands until done.",
      &execute_ai_loop,
      &render_help_ai_loop,
      &complete_ai_loop);

    [[maybe_unused]] bool const registered_ai_openrouter_loop = dana::register_sink(
      "ai.openrouter.loop",
      "Agentic tool loop on OpenRouter: it reads and edits files and runs commands until done.",
      &execute_ai_loop,
      &render_help_ai_loop,
      &complete_ai_loop);

} // namespace
