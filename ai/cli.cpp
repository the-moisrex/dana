#include "cli.hpp"

#include "../core/clipboard.hpp"
#include "diagnostics.hpp"
#include "files.hpp"
#include "http.hpp"
#include "keys.hpp"
#include "promptfile.hpp"
#include "providers/registry.hpp"
#include "state.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <pwd.h>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace dana::ai {

    namespace {

        using nlohmann::json;

        constexpr std::size_t model_column_width       = 10U;
        constexpr std::size_t model_list_timeout       = 5U;
        constexpr std::size_t key_source_column_width  = 32U;
        constexpr std::size_t minimum_provider_width   = 12U;
        constexpr double      price_per_million_tokens = 1'000'000.0;
        constexpr std::size_t default_password_buffer  = std::size_t{16} * 1024U;

        struct [[nodiscard]] help_requested final : std::exception {
            using std::exception::exception;
        };

        std::string trim(std::string_view value) {
            std::size_t const first = value.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos) {
                return {};
            }

            std::size_t const last = value.find_last_not_of(" \t\r\n");
            return std::string(value.substr(first, last - first + 1));
        }

        std::string read_all_stdin() {
            std::ostringstream buffer;
            buffer << std::cin.rdbuf();
            return buffer.str();
        }

        void usage(provider const& instance, std::ostream& out) {
            out << "Usage: ai." << instance.info().name << " [OPTIONS] [PROMPT...]\n";
            out << "Chat with " << instance.info().label << " from a prompt or interactively.\n";
        }

        void emit_json(renderer::writer const& out, stream_event const& event, json const& fields = json::object()) {
            json object{
              {"kind", std::string(kind_name(event.kind))},
            };

            if (!event.text.empty()) {
                object.emplace("text", event.text);
            }
            if (!event.name.empty()) {
                object.emplace("name", event.name);
            }
            if (!event.status.empty()) {
                object.emplace("status", event.status);
            }
            if (!event.args.empty()) {
                object.emplace("args", event.args);
            }
            if (!event.data.empty()) {
                object.emplace("data", event.data);
            }

            for (auto const& [key, value] : fields.items()) {
                object.erase(key);
                object.emplace(key, value);
            }

            std::string line  = object.dump(0);
            line             += '\n';

            if (out) {
                out(line);
            }
        }

        std::string_view base_name(std::string_view path) {
            auto const slash = path.find_last_of('/');
            return slash == std::string_view::npos ? path : path.substr(slash + 1);
        }

        bool copy_answer_text(std::string_view const text) {
            if (!clipboard::copy(text)) [[unlikely]] {
                warn("--copy: " + clipboard::last_error());
                return false;
            }
            return true;
        }

        renderer make_renderer(cli_options const& options, renderer::writer out, renderer::writer err) {
            bool const thinking = options.thinking.value_or(options.tty || ::isatty(STDERR_FILENO) != 0);

            render_options render;
            render.show_thinking = thinking;
            render.raw           = options.raw;
            render.color         = !options.no_color;
            render.debug         = options.debug;
            render.force_tty     = options.tty;
            render.width         = options.width;
            render.height        = options.height;

            return {render, std::move(out), std::move(err)};
        }

        std::string chosen_model(provider&                                        instance,
                                 cli_options const&                               options,
                                 std::optional<state::conversation_record> const& resume) {
            std::string requested = options.model;

            if (requested.empty() && resume) {
                requested = resume->model;
            }

            try {
                return instance.resolve_model(requested, instance.info().default_base_url);
            } catch (std::exception const&) {
                // The open() call reports the actionable error with the endpoint's hint.
                return requested;
            }
        }

        open_options make_open_options(cli_options const&                               options,
                                       std::optional<state::conversation_record> const& resume) {
            open_options open;
            open.system   = options.system;
            open.model    = options.model;
            open.debug    = options.debug;
            open.timeout  = options.timeout;
            open.base_url = options.base_url;
            open.api_key  = options.api_key;
            open.params   = options.params;

            if (resume) {
                open.conversation_id = resume->conversation_id;
                open.history         = resume->history;
                open.resume          = resume->resume;
            }

            return open;
        }

        void save_conversation(std::string_view provider_name, conversation const& chat, std::string_view scope) {
            state::conversation_record record;
            record.conversation_id = chat.conversation_id();
            record.model           = chat.model();
            record.history         = chat.history();
            record.resume          = chat.resume();

            std::ignore = state::save(provider_name, record, scope);
        }

        int print_models(provider& instance, cli_options const& options, renderer::writer const& out) {
            auto const& info = instance.info();

            if (options.all) {
                json catalogue = json::object();

                for (provider* entry : all_providers()) {
                    std::vector<std::string> models;

                    if (entry->info().api) {
                        models = entry->available_models();
                    } else {
                        models = entry->info().models;
                    }

                    catalogue.emplace(entry->info().name, models);

                    if (!options.json) {
                        std::string line = entry->info().name;

                        line +=
                          std::string(model_column_width > line.size() ? model_column_width - line.size() : 1U, ' ');

                        line += entry->info().label;

                        line += std::string(model_column_width > entry->info().label.size()
                                              ? model_column_width - entry->info().label.size()
                                              : 1U,
                                            ' ');

                        bool first_model = true;
                        for (std::string const& model : models) {
                            if (!first_model) {
                                line += ", ";
                            }

                            line        += model;
                            first_model  = false;
                        }

                        line += '\n';
                        out(line);
                    }
                }

                if (options.json) {
                    out(catalogue.dump(2) + "\n");
                }

                return 0;
            }

            if (!info.api) {
                for (std::string const& model : info.models) {
                    out(model + "\n");
                }

                return 0;
            }

            std::string base;
            try {
                base = instance.base_url(options.base_url);
            } catch (std::exception const& error) {
                warn(std::string("ai.") + info.name + ": " + error.what(), "ai");
                return 1;
            }

            std::vector<model_record> records;
            try {
                records = instance.chat_models_with_key(base, model_list_timeout, options.api_key);
            } catch (std::exception const& error) {
                warn(std::string("ai.") + info.name + ": could not list models: " + error.what(), "ai");
                return 1;
            }

            if (options.json) {
                json array = json::array();

                for (model_record const& record : records) {
                    array.push_back({
                      {     "id",record.id                                 },
                      {   "name",    record.name},
                      {   "type",    record.type},
                      {  "state",   record.state},
                      {"context", record.context},
                      {"pricing",
                       {
                       {"prompt", record.prompt_price},
                       {"completion", record.completion_price},
                       }                        },
                    });
                }

                out(array.dump(2) + "\n");
                return 0;
            }

            auto const price = [](std::string_view value) {
                try {
                    double const number = std::stod(std::string(value));

                    if (number <= 0) {
                        return std::string("free");
                    }

                    return std::format("${:.2f}/M", number * price_per_million_tokens);
                } catch (std::exception const&) {
                    return std::string{};
                }
            };

            for (model_record const& record : records) {
                std::string              line = record.id;
                std::vector<std::string> details;

                if (!record.name.empty() && record.name != record.id) {
                    details.push_back(record.name);
                }

                if (record.context > 0) {
                    details.push_back("ctx " + std::to_string(record.context));
                }

                std::string const prompt     = price(record.prompt_price);
                std::string const completion = price(record.completion_price);

                if (!prompt.empty() || !completion.empty()) {
                    details.push_back((prompt.empty() ? "?" : prompt) + "/" + (completion.empty() ? "?" : completion));
                }

                for (std::string const& detail : details) {
                    line += "  " + detail;
                }

                out(line + "\n");
            }

            return 0;
        }

        std::optional<std::filesystem::path> home_directory() {
            if (char const* home = std::getenv("HOME"); home != nullptr) [[unlikely]] {
                return std::filesystem::path(home);
            }

            passwd  password_entry{};
            passwd* result = nullptr;

            auto const        configured_size = ::sysconf(_SC_GETPW_R_SIZE_MAX);
            std::size_t const buffer_size =
              configured_size > 0 ? static_cast<std::size_t>(configured_size) : default_password_buffer;

            std::vector<char> buffer(buffer_size);

            int const status = ::getpwuid_r(::getuid(), &password_entry, buffer.data(), buffer.size(), &result);

            if (status != 0 || result == nullptr || result->pw_dir == nullptr) {
                return std::nullopt;
            }

            return std::filesystem::path(result->pw_dir);
        }

    } // namespace

    bool copy_answer(std::string_view text) {
        return copy_answer_text(text);
    }

    std::string read_prompt(text::string_views parts) {
        std::string text;

        for (std::string_view const part : parts) {
            if (!text.empty()) {
                text += ' ';
            }

            text += part;
        }

        text = trim(text);

        if (text.empty() && ::isatty(STDIN_FILENO) == 0) {
            text = trim(read_all_stdin());
        }

        return text;
    }

    cli_options parse_cli_options(provider const& instance, text::string_views args) {
        cli_options options;
        auto const& info = instance.info();

        auto const value_of = [&args](std::ptrdiff_t& index, std::string_view const flag) -> std::string {
            std::string_view const token  = args.at(index);
            auto const             equals = token.find('=');

            if (equals != std::string_view::npos) {
                return std::string(token.substr(equals + 1));
            }

            if (index + 1 >= static_cast<std::ptrdiff_t>(args.size())) {
                throw std::runtime_error(std::string(flag) + " needs a value");
            }

            ++index;
            return std::string(args.at(index));
        };

        bool positional_only = false;

        for (std::ptrdiff_t i = 0; i < static_cast<std::ptrdiff_t>(args.size()); ++i) {
            std::string_view const token = args.at(i);

            if (positional_only || token.empty() || token.front() != '-' || token == "-") {
                options.prompt.emplace_back(token);
                continue;
            }

            if (token == "--") {
                positional_only = true;
                continue;
            }

            auto const name = token.substr(0, token.find('='));

            if (name == "-f" || name == "--file") {
                options.files.push_back(value_of(i, name));
            } else if (name == "--prompt-file") {
                options.prompt_file = true;
            } else if (name == "--no-prompt-file") {
                options.prompt_file = false;
            } else if (name == "--prompt-limit") {
                options.prompt_limit = std::stoll(value_of(i, name));
            } else if (name == "-m" || name == "--model") {
                options.model = value_of(i, name);
            } else if (name == "-s" || name == "--system") {
                options.system = value_of(i, name);
            } else if (name == "-i" || name == "--interactive") {
                options.interactive = true;
            } else if (name == "-c" || name == "--continue") {
                options.continue_ = true;
            } else if (name == "--new") {
                options.new_ = true;
            } else if (name == "--show-thinking") {
                options.thinking = true;
            } else if (name == "--no-thinking") {
                options.thinking = false;
            } else if (name == "--json") {
                options.json = true;
            } else if (name == "--raw") {
                options.raw = true;
            } else if (name == "--tty") {
                options.tty = true;
            } else if (name == "--copy") {
                options.copy = true;
            } else if (name == "--no-color") {
                options.no_color = true;
            } else if (name == "--debug") {
                options.debug = true;
            } else if (name == "--timeout") {
                options.timeout = std::stol(value_of(i, name));
            } else if (name == "--base-url") {
                options.base_url = value_of(i, name);
            } else if (name == "--api-key") {
                options.api_key = value_of(i, name);
            } else if (name == "--param") {
                options.params.push_back(value_of(i, name));
            } else if (name == "--browser") {
                options.browser.push_back(value_of(i, name));
            } else if (name == "--profile") {
                options.profile = value_of(i, name);
            } else if (name == "--family") {
                options.family = value_of(i, name);
            } else if (name == "--open-only") {
                options.open_only = true;
            } else if (name == "--exact") {
                options.exact = true;
            } else if (name == "--print-session") {
                options.print_session = true;
            } else if (name == "--print-prompt") {
                options.print_prompt = true;
            } else if (name == "--stdin-only") {
                options.stdin_only = true;
            } else if (name == "--context-file") {
                options.context_file = value_of(i, name);
            } else if (name == "--reply") {
                options.reply = true;
            } else if (name == "--terminal-scope") {
                options.terminal_scope = value_of(i, name);
            } else if (name == "--width") {
                options.width = std::stoi(value_of(i, name));
            } else if (name == "--height") {
                options.height = std::stoi(value_of(i, name));
            } else if (name == "--list" || name == "--models") {
                options.list = true;
            } else if (name == "--all") {
                options.all = true;
            } else if (name == "-h" || name == "--help") {
                usage(instance, std::cout);
                throw help_requested{};
            } else [[unlikely]] {
                throw std::runtime_error(std::format("unknown option {} (see ai.{} --help)", token, info.name));
            }
        }

        return options;
    }

    turn_result run_turn(
      conversation&           chat,
      provider const&         instance,
      std::string_view        prompt,
      turn_options const&     options,
      renderer::writer        out,
      renderer::writer const& err) {
        auto const& info = instance.info();
        turn_result result;

        std::vector<std::string> files = options.files;
        std::optional<bool>      mode  = options.prompt_file;

        if (!info.prompt_file_upload) {
            mode = false;
        }

        bool const auto_size = info.auto_attach_by_size;
        bool       attach    = false;

        if (mode && *mode) {
            attach = true;
        } else if (!mode || *mode) {
            if (mode || options.prompt_limit || auto_size) {
                attach = should_attach(prompt, info.inline_limit, mode, options.prompt_limit);
            }
        }

        if (mode && !*mode) {
            attach = false;
        }

        std::filesystem::path temporary;
        std::string           message = std::string(prompt);

        if (attach) {
            std::size_t const count = prompt_size(prompt);

            try {
                temporary = write_prompt_file(prompt);
            } catch (std::exception const& failure) {
                stream_event const problem = error(std::string("could not write the prompt file: ") + failure.what());

                if (options.json) {
                    emit_json(out, problem, options.json_fields);
                } else {
                    cli_options problem_options;
                    problem_options.json = options.json;

                    renderer view = make_renderer(problem_options, out, err);
                    view.handle(problem);
                    view.finish();
                }

                return {.status = 1, .answer = {}};
            }

            message = prompt_file_reference(temporary, count);
            files.push_back(temporary.string());

            if (!options.json) {
                std::string detail;

                if (mode && *mode) {
                    detail = human_size(count) + ", forced by --prompt-file";
                } else {
                    detail = human_size(count) + ", above the " +
                             human_size(limit_for(info.inline_limit, options.prompt_limit)) + " inline budget";
                }

                err("[ai] prompt " + detail + "; uploading it as " + base_name(temporary.string()) + "\n");
            }
        }

        std::ofstream context;

        if (!options.context_file.empty()) {
            context.open(options.context_file, std::ios::binary | std::ios::trunc);

            if (!context) {
                stream_event const problem = dana::ai::error("could not open --context-file " + options.context_file);

                if (options.json) {
                    emit_json(out, problem, options.json_fields);
                } else {
                    cli_options problem_options;
                    problem_options.json = options.json;

                    renderer view = make_renderer(problem_options, out, err);
                    view.handle(problem);
                    view.finish();
                }

                remove_prompt_file(temporary);
                return {.status = 1, .answer = {}};
            }
        }

        cli_options render_options;
        render_options.json     = options.json;
        render_options.raw      = options.raw;
        render_options.tty      = options.tty;
        render_options.no_color = options.no_color;
        render_options.debug    = options.debug;
        render_options.thinking = options.thinking;
        render_options.width    = options.width;
        render_options.height   = options.height;

        renderer view = make_renderer(render_options, out, err);

        bool        terminal = false;
        bool        failed   = false;
        std::string answer;

        auto const handle = [&](stream_event const& event) {
            stream_event current = event;

            if (current.kind == event_kind::error) {
                bool const used_file            = attach;
                bool const prompt_file_disabled = mode && !*mode;
                bool const has_upload           = info.prompt_file_upload;
                bool const auto_attach          = auto_size;

                current.text = annotate_error(
                  current.text,
                  info.name,
                  info.label,
                  prompt,
                  used_file,
                  prompt_file_disabled,
                  has_upload,
                  auto_attach,
                  options.prompt_limit);
            }

            if (options.json) {
                emit_json(out, current, options.json_fields);
            } else {
                view.handle(current);
                view.write_debug(current);
            }

            if (current.kind == event_kind::text) {
                answer += current.text;

                if (context) {
                    context.write(current.text.data(), static_cast<std::streamsize>(current.text.size()));
                    context.flush();
                }
            }

            if (current.kind == event_kind::error) {
                failed   = true;
                terminal = true;
            } else if (current.kind == event_kind::done) {
                terminal = true;
            }
        };

        try {
            chat.send(message, files, handle);
        } catch (std::exception const& failure) {
            handle(dana::ai::error(std::string("error: ") + failure.what()));
        }

        if (context) {
            context.close();
        }

        remove_prompt_file(temporary);

        if (!options.json) {
            std::string const finish_error = view.finish();

            if (!finish_error.empty() && !failed) {
                failed = true;
            }
        }

        if (!failed && !terminal) {
            stream_event const problem = dana::ai::error("stream ended before the model produced a reply");

            if (options.json) {
                emit_json(out, problem, options.json_fields);
            } else {
                view.handle(problem);
            }

            failed = true;
        }

        result.status = failed ? 1 : 0;
        result.answer = std::move(answer);
        return result;
    }

    namespace {

        int run_once(conversation&      chat,
                     provider const&    instance,
                     cli_options const& options,
                     std::string_view   provider_name,
                     std::string_view   scope) {
            renderer::writer const out = [](std::string_view chunk) {
                std::cout.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                std::cout.flush();
            };

            renderer::writer const err = [](std::string_view chunk) {
                std::cerr.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                std::cerr.flush();
            };

            turn_options turn;
            turn.json         = options.json;
            turn.raw          = options.raw;
            turn.tty          = options.tty;
            turn.no_color     = options.no_color;
            turn.debug        = options.debug;
            turn.thinking     = options.thinking;
            turn.prompt_file  = options.prompt_file;
            turn.prompt_limit = options.prompt_limit;
            turn.files        = chat.turns() == 0 ? options.files : std::vector<std::string>{};
            turn.context_file = options.context_file;
            turn.width        = options.width;
            turn.height       = options.height;

            std::string const prompt = read_prompt(options.prompt);
            auto const        result = run_turn(chat, instance, prompt, turn, out, err);

            if (options.copy) {
                copy_answer(result.answer);
            }

            if (result.status == 0) {
                try {
                    save_conversation(provider_name, chat, scope);
                } catch (std::exception const& failure) {
                    warn(std::string("ai.") + provider_name + ": could not save conversation: " + failure.what(), "ai");
                }
            }

            return result.status;
        }

        template <typename SessionMatch>
        int run_repl(conversation&       initial,
                     provider&           instance,
                     SessionMatch const& match,
                     cli_options const&  options,
                     std::string_view    provider_name,
                     std::string_view    scope) {
            conversation* chat = &initial;

            auto const  model = chat->model();
            std::string where = std::format("ai.{}: {}", provider_name, model);

            if (!chat->history().empty() && chat->history().is_array()) {
                where += " (continuing, " + std::to_string(chat->history().size()) + " message(s) in context)";
            }

            std::cerr << where << '\n';
            std::cerr << ":new starts a new chat, :q (or Ctrl-D) quits.\n";

            std::string first  = read_prompt(options.prompt);
            int         status = 0;

            while (true) {
                std::string text;

                if (!first.empty()) {
                    text = first;
                    first.clear();
                } else {
                    std::cerr << provider_name << "> ";
                    std::cerr.flush();

                    std::string line;
                    if (!std::getline(std::cin, line)) {
                        std::cerr << '\n';
                        break;
                    }

                    text = trim(line);

                    if (text.empty()) {
                        continue;
                    }
                }

                if (text == ":q" || text == ":quit" || text == ":exit" || text == "exit" || text == "quit") {
                    break;
                }

                if (text == ":new") {
                    state::clear(provider_name, scope);
                    first.clear();

                    try {
                        auto fresh = instance.open(match, model, make_open_options(options, std::nullopt));

                        chat->close();

                        // The fresh conversation is owned by the unique_ptr below.
                        static std::unique_ptr<conversation> replacement;
                        replacement = std::move(fresh);
                        chat        = replacement.get();
                    } catch (std::exception const& failure) {
                        warn(std::string("ai.") + provider_name + ": " + failure.what(), "ai");
                    }

                    continue;
                }

                renderer::writer const out = [](std::string_view chunk) {
                    std::cout.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                    std::cout.flush();
                };

                renderer::writer const err = [](std::string_view chunk) {
                    std::cerr.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                    std::cerr.flush();
                };

                turn_options turn;
                turn.json         = options.json;
                turn.raw          = options.raw;
                turn.tty          = options.tty;
                turn.no_color     = options.no_color;
                turn.debug        = options.debug;
                turn.thinking     = options.thinking;
                turn.prompt_file  = options.prompt_file;
                turn.prompt_limit = options.prompt_limit;
                turn.files        = chat->turns() == 0 ? options.files : std::vector<std::string>{};
                turn.context_file = options.context_file;
                turn.width        = options.width;
                turn.height       = options.height;

                auto const result = run_turn(*chat, instance, text, turn, out, err);

                if (options.copy) {
                    copy_answer(result.answer);
                }

                if (result.status == 0) {
                    try {
                        save_conversation(provider_name, *chat, scope);
                    } catch (std::exception const& failure) {
                        warn(std::string("ai.") + provider_name + ": could not save conversation: " + failure.what(),
                             "ai");
                    }
                } else {
                    status = status != 0 ? status : result.status;
                }
            }

            chat->close();
            return status;
        }

    } // namespace

    int run_provider_cli(std::string_view provider_name, text::string_views args) {
        auto const canonical = canonical_provider(provider_name);

        if (!canonical) [[unlikely]] {
            warn(std::format("ai: unknown provider '{}'", provider_name), "ai");
            return 2;
        }

        provider* instance = find_provider(*canonical);

        if (instance == nullptr) [[unlikely]] {
            warn(std::format("ai: unknown provider '{}'", provider_name), "ai");
            return 2;
        }

        cli_options options;

        try {
            options = parse_cli_options(*instance, args);
        } catch (help_requested const&) {
            return 0;
        } catch (std::exception const& failure) {
            warn(std::string("ai.") + *canonical + ": " + failure.what(), "ai");
            return 2;
        }

        renderer::writer const out = [](std::string_view chunk) {
            std::cout.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            std::cout.flush();
        };

        if (options.list) {
            return print_models(*instance, options, out);
        }

        if (options.prompt_file && *options.prompt_file && !instance->info().prompt_file_upload) [[unlikely]] {
            warn(std::string("ai.") + *canonical + ": --prompt-file is not supported: " + instance->info().label +
                   " has no upload endpoint, so the prompt is always sent inline; "
                   "raise the model's context length instead",
                 "ai");
            return 2;
        }

        if (options.stdin_only && !options.prompt.empty()) [[unlikely]] {
            warn(std::string("ai.") + *canonical +
                   ": positional prompt text is not supported in a chain; "
                   "put instructions in `.note \"...\"` before the sink or use --system",
                 "ai");
            return 2;
        }

        if (options.print_prompt) {
            std::string const prompt = read_prompt(options.prompt);

            if (prompt.empty()) {
                usage(*instance, std::cerr);
                return 2;
            }

            std::cout << prompt << '\n';
            return 0;
        }

        if (options.interactive) {
            if (options.json) [[unlikely]] {
                warn(std::string("ai.") + *canonical + ": -i cannot be combined with --json", "ai");
                return 2;
            }

            if (::isatty(STDIN_FILENO) == 0) [[unlikely]] {
                warn(std::string("ai.") + *canonical + ": -i needs a terminal on stdin", "ai");
                return 2;
            }
        }

        session::match match;

        if (instance->info().needs_session) {
            session::options session_options;
            session_options.browser   = options.browser;
            session_options.profile   = options.profile;
            session_options.family    = options.family;
            session_options.open_only = options.open_only;
            session_options.exact     = options.exact;

            try {
                auto found = session::load(instance->info().session_target, session_options);

                if (!found) [[unlikely]] {
                    warn(std::string("ai.") + *canonical + ": no signed-in session found for " +
                           instance->info().session_target + "; open " + instance->info().origin + " in a browser once",
                         "ai");
                    return 1;
                }

                match = *found;
            } catch (std::exception const& failure) {
                warn(std::string("ai.") + *canonical + ": " + failure.what(), "ai");
                return 1;
            }
        }

        if (options.print_session) {
            open_options open;
            open.model    = options.model;
            open.base_url = options.base_url;
            open.api_key  = options.api_key;
            open.params   = options.params;

            std::cout << instance->session_descriptor(open).dump(2) << '\n';
            return 0;
        }

        for (std::string& path : options.files) {
            std::filesystem::path full(path);

            if (full.string().starts_with("~/")) {
                if (auto const home = home_directory()) {
                    full = *home / full.string().substr(2);
                }
            }

            std::error_code error_code;

            if (!exists(full, error_code)) [[unlikely]] {
                warn(std::string("ai.") + *canonical + ": " + path + ": no such file", "ai");
                return 1;
            }

            if (!is_regular_file(full, error_code)) [[unlikely]] {
                warn(std::string("ai.") + *canonical + ": " + path + ": not a regular file", "ai");
                return 1;
            }

            // home_directory() fallback can make a ~/... file pass validation when HOME is unset. But validation
            // expands only the local full path; options.files still contains the literal ~/... path passed to run_turn.
            // Use the validated path for the upload too.
            path = full.string();
        }

        std::string const scope = options.terminal_scope.empty() ? state::terminal_scope() : options.terminal_scope;

        std::optional<state::conversation_record> resume;

        if (options.new_) {
            state::clear(*canonical, scope);
        } else if (options.reply) {
            resume = state::load_terminal(*canonical, scope);

            if (!resume) [[unlikely]] {
                warn(std::string("ai.") + *canonical + ": no chat in this terminal to reply to; start one with ai." +
                       *canonical + " (or run it without --reply)",
                     "ai");
                return 1;
            }
        } else if (options.continue_) {
            resume = state::load(*canonical);

            if (!resume) [[unlikely]] {
                warn(std::string("ai.") + *canonical + ": no saved conversation to continue; starting a new one", "ai");
            }
        }

        std::string const             model = chosen_model(*instance, options, resume);
        std::unique_ptr<conversation> chat;

        try {
            chat = instance->open(match, model, make_open_options(options, resume));
        } catch (std::exception const& failure) {
            warn(std::string("ai.") + *canonical + ": " + failure.what(), "ai");
            return 1;
        }

        if (options.interactive) {
            return run_repl(*chat, *instance, match, options, *canonical, scope);
        }

        std::string const prompt = read_prompt(options.prompt);

        if (prompt.empty()) {
            usage(*instance, std::cerr);
            return 2;
        }

        // run_once re-reads stdin; the prompt was already consumed above.
        cli_options with_prompt = options;
        with_prompt.prompt      = {prompt};

        return run_once(*chat, *instance, with_prompt, *canonical, scope);
    }

    std::string list_models_text(provider& instance, cli_options const& options) {
        std::string text;

        renderer::writer const out = [&text](std::string_view chunk) {
            text.append(chunk);
        };

        cli_options copy = options;
        copy.json        = options.json;

        print_models(instance, copy, out);
        return text;
    }

    std::string session_report_text(provider& instance, cli_options const& options) {
        open_options open;
        open.model    = options.model;
        open.base_url = options.base_url;
        open.api_key  = options.api_key;
        open.params   = options.params;

        return instance.session_descriptor(open).dump(2) + "\n";
    }

    int run_keys_cli(text::string_views args) {
        std::vector<std::string_view> names;

        for (std::string_view const token : args) {
            if (!token.empty() && token.front() != '-') {
                names.emplace_back(token);
            }
        }

        if (names.empty()) {
            for (provider const* instance : all_providers()) {
                if (instance->info().api) {
                    names.emplace_back(instance->info().name);
                }
            }
        }

        std::size_t width = minimum_provider_width;

        for (std::string_view const name : names) {
            width = std::max(width, name.size());
        }

        for (std::string_view const name : names) {
            auto const      canonical = canonical_provider(name);
            provider const* instance  = canonical ? find_provider(*canonical) : nullptr;

            if (instance == nullptr) [[unlikely]] {
                [[unlikely]] {
                    warn(std::string(name) + ": unknown provider", "ai");
                    continue;
                }
            }

            std::array<std::string_view, 1> const environment_names{
              instance->info().key_env,
            };

            auto const [source, masked] = keys::describe(instance->info().name, environment_names);

            std::string line = std::string(name);

            line += std::string(width - name.size() + 2U, ' ');
            line += source;

            line +=
              std::string(source.size() < key_source_column_width ? key_source_column_width - source.size() + 2U : 2U,
                          ' ');

            line += masked;
            std::cout << line << '\n';
        }

        return 0;
    }

} // namespace dana::ai
