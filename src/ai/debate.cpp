#include "debate.hpp"

#include "cli.hpp"
#include "diagnostics.hpp"
#include "providers/registry.hpp"
#include "session.hpp"
#include "state.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace dana::ai {

    namespace {

        namespace fs = std::filesystem;
        using nlohmann::json;

        struct debate_usage_error : std::runtime_error {
            using std::runtime_error::runtime_error;
        };

        // A usable debate could not be started (missing session, bad file, …).
        struct debate_error : std::runtime_error {
            using std::runtime_error::runtime_error;
        };

        std::string trim(std::string_view value) {
            std::size_t first = value.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos) {
                return {};
            }
            std::size_t last = value.find_last_not_of(" \t\r\n");
            return std::string(value.substr(first, last - first + 1));
        }

        std::string join(std::vector<std::string> const& parts, std::string_view separator) {
            std::string text;
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (i != 0) {
                    text += separator;
                }
                text += parts[i];
            }
            return text;
        }

        std::string join_words(std::vector<std::string> const& words) {
            std::string text;
            for (auto const& word : words) {
                if (!text.empty()) {
                    text += ' ';
                }
                text += word;
            }
            return trim(text);
        }

        bool is_word_char(char value) {
            return std::isalnum(static_cast<unsigned char>(value)) != 0 || value == '_';
        }

        bool equals_ignore_case(std::string_view left, std::string_view right) {
            if (left.size() != right.size()) {
                return false;
            }
            for (std::size_t i = 0; i < left.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(left[i])) !=
                    std::tolower(static_cast<unsigned char>(right[i])))
                {
                    return false;
                }
            }
            return true;
        }

        std::string expand_user(std::string const& path) {
            if (path.size() >= 2 && path[0] == '~' && path[1] == '/') {
                if (char const* home = std::getenv("HOME"); home != nullptr) {
                    return std::string(home) + path.substr(1);
                }
            }
            return path;
        }

        // ---------------------------------------------------------------------------
        // The relay loop
        // ---------------------------------------------------------------------------

        // The last `VERDICT: AGREE|DISAGREE` line of an answer, or "" when absent.
        std::string parse_verdict(std::string_view answer) {
            std::string last;
            std::size_t position = 0;
            while (position <= answer.size()) {
                std::size_t end = answer.find('\n', position);
                if (end == std::string_view::npos) {
                    end = answer.size();
                }
                std::string_view line  = answer.substr(position, end - position);
                std::size_t      start = 0;
                while (start < line.size() && std::isspace(static_cast<unsigned char>(line[start])) != 0) {
                    ++start;
                }
                std::string_view           rest   = line.substr(start);
                constexpr std::string_view prefix = "verdict:";
                if (rest.size() >= prefix.size() && equals_ignore_case(rest.substr(0, prefix.size()), prefix)) {
                    std::size_t value_at = prefix.size();
                    while (value_at < rest.size() && std::isspace(static_cast<unsigned char>(rest[value_at])) != 0) {
                        ++value_at;
                    }
                    std::string_view value = rest.substr(value_at);
                    for (std::string_view candidate : {"AGREE", "DISAGREE"}) {
                        if (value.size() < candidate.size()) {
                            continue;
                        }
                        if (!equals_ignore_case(value.substr(0, candidate.size()), candidate)) {
                            continue;
                        }
                        if (value.size() == candidate.size() || !is_word_char(value[candidate.size()])) {
                            last = candidate;
                        }
                    }
                }
                if (end == answer.size()) {
                    break;
                }
                position = end + 1;
            }
            return last;
        }

        std::string build_prompt(
          std::string_view                                          question,
          std::optional<std::pair<std::string, std::string>> const& previous,
          bool                                                      first,
          std::string_view                                          instruction) {
            if (!previous) {
                return trim(question);
            }
            std::vector<std::string> parts;
            if (first) {
                parts.push_back(trim(question));
            }
            parts.push_back("Another AI said:\n\n" + trim(previous->second));
            if (!instruction.empty()) {
                parts.push_back(trim(instruction));
            }
            return join(parts, "\n\n");
        }

        debate_result run_panel(
          std::vector<debate_participant> const& participants,
          std::string const&                     question,
          int                                    rounds,
          bool                                   until_agree,
          std::string const&                     instruction,
          debate_writer const&                   writer,
          debate_turn_observer const&            on_turn) {
            debate_result                                      result;
            std::set<std::string>                              started;
            std::map<std::string, std::string>                 verdicts;
            std::optional<std::pair<std::string, std::string>> previous;
            int const                                          total   = rounds * static_cast<int>(participants.size());
            int                                                turn_no = 0;

            for (int round_no = 1; round_no <= rounds; ++round_no) {
                result.rounds_run = round_no;
                for (auto const& participant : participants) {
                    bool const        first  = !started.contains(participant.name);
                    std::string const prompt = build_prompt(question, previous, first, instruction);
                    ++turn_no;
                    if (on_turn) {
                        on_turn(turn_no,
                                total,
                                round_no,
                                participant.name,
                                previous ? std::optional<std::string>(previous->first) : std::nullopt);
                    }
                    json fields{
                      {    "turn",          turn_no},
                      {   "round",         round_no},
                      {"provider", participant.name}
                    };
                    if (previous) {
                        fields["reply_to"] = previous->first;
                    }
                    auto const [status, raw_answer] = participant.ask(prompt, fields);
                    if (status != 0) {
                        result.status          = status;
                        result.stop_reason     = "failed";
                        result.failed_provider = participant.name;
                        return result;
                    }
                    std::string const answer = trim(raw_answer);
                    if (answer.empty()) {
                        result.status          = 1;
                        result.stop_reason     = "failed";
                        result.failed_provider = participant.name;
                        return result;
                    }
                    started.insert(participant.name);
                    previous = std::make_pair(participant.name, answer);
                    result.transcript.push_back({.round = round_no, .provider = participant.name, .answer = answer});
                    if (writer) {
                        writer(participant.name, round_no, answer);
                    }
                    if (until_agree) {
                        verdicts[participant.name] = parse_verdict(answer);
                    }
                }
                if (until_agree) {
                    bool agreed = true;
                    for (auto const& participant : participants) {
                        auto const found = verdicts.find(participant.name);
                        if (found == verdicts.end() || found->second != "AGREE") {
                            agreed = false;
                            break;
                        }
                    }
                    if (agreed) {
                        result.stop_reason = "agreement";
                        break;
                    }
                }
            }
            return result;
        }

        // ---------------------------------------------------------------------------
        // The plain side channel (`--context-file`) and the clipboard text
        // ---------------------------------------------------------------------------

        struct written_turn {
            std::string provider;
            int         round = 0;
            std::string answer;
        };

        class context_writer {
          public:
            context_writer(std::string path, std::string const& mode, std::string const& question)
              : path_(std::move(path)),
                mode_(mode == "all" ? "all" : "last"),
                question_(trim(question)) {
                if (!path_.empty()) {
                    std::ofstream probe(path_, std::ios::binary | std::ios::trunc);
                    if (!probe) {
                        error_ = "cannot open the file";
                    }
                }
            }

            void add(std::string const& provider, int round_no, std::string const& answer) {
                turns_.push_back({.provider = provider, .round = round_no, .answer = trim(answer)});
                flush();
            }

            [[nodiscard]] std::string text() const {
                if (mode_ == "last") {
                    return turns_.empty() ? std::string{} : turns_.back().answer;
                }
                std::vector<std::string> blocks;
                if (!question_.empty()) {
                    blocks.push_back("## Question\n\n" + question_);
                }
                for (auto const& turn : turns_) {
                    blocks.push_back(
                      "## " + turn.provider + " · round " + std::to_string(turn.round) + "\n\n" + turn.answer);
                }
                return join(blocks, "\n\n");
            }

            [[nodiscard]] bool has_error() const {
                return !error_.empty();
            }

            [[nodiscard]] std::string const& error() const {
                return error_;
            }

            void close() {}

          private:
            void flush() {
                if (path_.empty()) {
                    return;
                }
                std::ofstream stream(path_, std::ios::binary | std::ios::trunc);
                if (!stream) {
                    return;
                }
                std::string const value = text();
                stream << value;
                if (!value.empty() && value.back() != '\n') {
                    stream << '\n';
                }
                stream.flush();
            }

            std::string               path_;
            std::string               mode_;
            std::string               question_;
            std::vector<written_turn> turns_;
            std::string               error_;
        };

        // ---------------------------------------------------------------------------
        // The command line
        // ---------------------------------------------------------------------------

        struct debate_options {
            std::vector<std::string>   words;
            int                        rounds      = 2;
            bool                       until_agree = false;
            std::optional<std::string> instruction;
            bool                       no_instruction = false;
            std::string                context        = "last";
            bool                       transcript     = false;
            bool                       continue_      = false;
            bool                       new_           = false;
            std::string                system;
            std::vector<std::string>   model_specs;
            std::vector<std::string>   files;
            std::optional<bool>        prompt_file;
            std::optional<long long>   prompt_limit;
            std::optional<bool>        thinking;
            bool                       json     = false;
            bool                       raw      = false;
            bool                       tty      = false;
            bool                       copy     = false;
            bool                       no_color = false;
            bool                       debug    = false;
            double                     timeout  = 600;
            std::string                base_url;
            std::string                api_key;
            std::vector<std::string>   params;
            std::vector<std::string>   browser;
            std::string                profile;
            std::string                family;
            bool                       open_only     = false;
            bool                       exact         = false;
            bool                       print_session = false;
            bool                       print_prompt  = false;
            bool                       list          = false;
            bool                       stdin_only    = false;
            std::string                context_file;
            int                        width  = 0;
            int                        height = 0;
            bool                       help   = false;
        };

        int parse_integer(std::string const& flag, std::string const& value) {
            try {
                std::size_t used   = 0;
                int const   number = std::stoi(value, &used);
                if (used != value.size()) {
                    throw std::invalid_argument(value);
                }
                return number;
            } catch (std::exception const&) {
                throw debate_usage_error(flag + " expects an integer, got '" + value + "'");
            }
        }

        double parse_number(std::string const& flag, std::string const& value) {
            try {
                std::size_t  used   = 0;
                double const number = std::stod(value, &used);
                if (used != value.size()) {
                    throw std::invalid_argument(value);
                }
                return number;
            } catch (std::exception const&) {
                throw debate_usage_error(flag + " expects a number, got '" + value + "'");
            }
        }

        debate_options parse_options(std::vector<std::string> const& argv) {
            debate_options options;
            auto           value_of = [&argv](std::size_t& index, std::string_view flag) -> std::string {
                std::string const& token  = argv[index];
                auto const         equals = token.find('=');
                if (equals != std::string::npos) {
                    return token.substr(equals + 1);
                }
                if (index + 1 >= argv.size()) {
                    throw debate_usage_error(std::string(flag) + " needs a value");
                }
                return argv[++index];
            };

            bool positional_only = false;
            for (std::size_t i = 0; i < argv.size(); ++i) {
                std::string const& token = argv[i];
                if (positional_only || token.empty() || token.front() != '-' || token == "-") {
                    options.words.push_back(token);
                    continue;
                }
                if (token == "--") {
                    positional_only = true;
                    continue;
                }
                std::string const name = token.substr(0, token.find('='));
                if (name == "-h" || name == "--help") {
                    options.help = true;
                } else if (name == "--rounds") {
                    options.rounds = parse_integer(name, value_of(i, name));
                } else if (name == "--until-agree") {
                    options.until_agree = true;
                } else if (name == "--instruction") {
                    options.instruction = value_of(i, name);
                } else if (name == "--no-instruction") {
                    options.no_instruction = true;
                } else if (name == "--context") {
                    std::string const value = value_of(i, name);
                    if (value != "last" && value != "all") {
                        throw debate_usage_error("--context expects last or all, got '" + value + "'");
                    }
                    options.context = value;
                } else if (name == "--transcript") {
                    options.transcript = true;
                } else if (name == "-c" || name == "--continue") {
                    options.continue_ = true;
                } else if (name == "--new") {
                    options.new_ = true;
                } else if (name == "-s" || name == "--system") {
                    options.system = value_of(i, name);
                } else if (name == "-m" || name == "--model") {
                    options.model_specs.push_back(value_of(i, name));
                } else if (name == "-f" || name == "--file") {
                    options.files.push_back(value_of(i, name));
                } else if (name == "--prompt-file") {
                    options.prompt_file = true;
                } else if (name == "--no-prompt-file") {
                    options.prompt_file = false;
                } else if (name == "--prompt-limit") {
                    options.prompt_limit = static_cast<long long>(parse_integer(name, value_of(i, name)));
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
                    options.timeout = parse_number(name, value_of(i, name));
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
                    std::string const value = value_of(i, name);
                    if (value != "chromium" && value != "firefox") {
                        throw debate_usage_error("--family expects chromium or firefox, got '" + value + "'");
                    }
                    options.family = value;
                } else if (name == "--open-only") {
                    options.open_only = true;
                } else if (name == "--exact") {
                    options.exact = true;
                } else if (name == "--print-session") {
                    options.print_session = true;
                } else if (name == "--print-prompt") {
                    options.print_prompt = true;
                } else if (name == "--list") {
                    options.list = true;
                } else if (name == "--stdin-only") {
                    options.stdin_only = true;
                } else if (name == "--context-file") {
                    options.context_file = value_of(i, name);
                } else if (name == "--width") {
                    options.width = parse_integer(name, value_of(i, name));
                } else if (name == "--height") {
                    options.height = parse_integer(name, value_of(i, name));
                } else {
                    throw debate_usage_error("unrecognized argument: " + token);
                }
            }
            return options;
        }

        // A provider name as the debate spells it: `gapgpt` is not one of the registry's
        // own aliases, and the rest resolve through `canonical_provider`.
        std::optional<std::string> debate_alias(std::string_view name) {
            if (name == "gapgpt") {
                return std::string{"gap"};
            }
            return canonical_provider(name);
        }

        std::pair<std::vector<std::string>, std::vector<std::string>> split_providers(
          std::vector<std::string> const& words) {
            std::vector<std::string> names;
            std::size_t              index = 0;
            while (index < words.size()) {
                auto const canonical = debate_alias(words[index]);
                if (!canonical) {
                    break;
                }
                names.push_back(*canonical);
                ++index;
            }
            return {
              std::move(names),
              {words.begin() + static_cast<std::ptrdiff_t>(index), words.end()}
            };
        }

        std::map<std::string, std::string> parse_models(std::vector<std::string> const& specs,
                                                        std::vector<std::string> const& names) {
            std::map<std::string, std::string> models;
            for (auto const& spec : specs) {
                auto const equals = spec.find('=');
                if (equals == std::string::npos) {
                    throw debate_usage_error("--model expects PROVIDER=MODEL, got '" + spec + "'");
                }
                std::string const raw_name  = trim(spec.substr(0, equals));
                std::string const model     = trim(spec.substr(equals + 1));
                auto const        canonical = debate_alias(raw_name);
                std::string const name      = canonical ? *canonical : raw_name;
                if (std::ranges::find(names, name) == names.end()) {
                    throw debate_usage_error(
                      "--model names '" + name + "', which is not one of this debate's providers (" +
                      join(names, ", ") + ")");
                }
                if (model.empty()) {
                    throw debate_usage_error("--model " + name + "= needs a model name");
                }
                models[name] = model;
            }
            return models;
        }

        std::string instruction_for(debate_options const& options) {
            if (options.no_instruction) {
                if (options.until_agree) {
                    throw debate_usage_error("--until-agree needs the verification instruction");
                }
                return {};
            }
            std::string text =
              options.instruction ? trim(*options.instruction) : std::string(kDebateDefaultInstruction);
            if (options.until_agree) {
                text = text + "\n\n" + std::string(kDebateVerdictSuffix);
            }
            return text;
        }

        void print_plan(std::vector<std::string> const& names,
                        std::string const&              question,
                        std::string const&              instruction) {
            std::vector<std::string> parts;
            parts.emplace_back("# turn 1 → " + names.front());
            parts.emplace_back(trim(question));
            parts.emplace_back("# later turns");
            parts.emplace_back("Another AI said:");
            parts.emplace_back("<answer>");
            if (!instruction.empty()) {
                parts.push_back(trim(instruction));
            }
            std::cout << join(parts, "\n\n") << '\n';
        }

        void print_usage(std::ostream& out) {
            out << "usage: ai.debate [-h] [--rounds N] [--until-agree] [--instruction TEXT]\n"
                   "                  [--no-instruction] [--context {last,all}] [--transcript]\n"
                   "                  [-c] [--new] [-s SYSTEM] [-m PROVIDER=MODEL] [-f PATH]\n"
                   "                  [--prompt-file | --no-prompt-file] [--prompt-limit BYTES]\n"
                   "                  [--show-thinking | --no-thinking] [--json] [--raw] [--tty]\n"
                   "                  [--copy] [--no-color] [--debug] [--timeout SECONDS]\n"
                   "                  [--base-url URL] [--api-key KEY] [--param KEY=VALUE]\n"
                   "                  [--browser BROWSER] [--profile PROFILE]\n"
                   "                  [--family {chromium,firefox}] [--open-only] [--exact]\n"
                   "                  [--print-session] [--print-prompt] [--list]\n"
                   "                  [--context-file PATH] [--width COLS] [--height ROWS]\n"
                   "                  [PROVIDER|QUESTION ...]\n";
        }

        void render_help(std::ostream& out, std::map<std::string, provider*> const& providers) {
            std::vector<std::string> names;
            names.reserve(providers.size());
            for (auto const& [name, instance] : providers) {
                (void) instance;
                names.push_back(name);
            }
            out << "Usage: ai.debate [OPTIONS] PROVIDER PROVIDER [PROVIDER...] [QUESTION...]\n"
                << "\n"
                << "Ask two or more AIs the same question, then let each verify the other's answer,\n"
                << "round after round (replays the signed-in web sessions, or calls the local/hosted\n"
                << "OpenAI-compatible API providers without a browser).\n"
                << "\n"
                << "positional arguments:\n"
                << "  PROVIDER|QUESTION          providers first (two or more), then the question;\n"
                << "                             with no question the prompt is read from stdin\n"
                << "\n"
                << "options:\n"
                << "  -h, --help                 show this help message and exit\n"
                << "  --rounds N                 full passes through the providers (default: 2)\n"
                << "  --until-agree              stop after a pass in which every AI's VERDICT is AGREE\n"
                << "  --instruction TEXT         verification request appended to each turn (default: built in)\n"
                << "  --no-instruction           relay the other answer with no verification request\n"
                << "  --context {last,all}       what --context-file and --copy receive: the last answer\n"
                << "                             (default) or the labelled transcript\n"
                << "  --transcript               same as --context all\n"
                << "  -c, --continue             continue each provider's chat from the last debate\n"
                << "                             between these providers\n"
                << "  --new                      start fresh chats and forget the saved debate\n"
                << "  -s, --system TEXT          system / custom instructions for every provider\n"
                << "  -m, --model PROVIDER=MODEL pick one provider's model (repeatable), e.g.\n"
                << "                             -m grok=expert -m deepseek=deepseek-reasoner\n"
                << "  -f, --file PATH            upload and attach a local file to each provider's first\n"
                << "                             turn (repeatable)\n"
                << "  --prompt-file              upload a prompt as a text file: automatic when it is too\n"
                << "                             large for one message (the default)\n"
                << "  --no-prompt-file           always send the prompt inline\n"
                << "  --prompt-limit BYTES       override each provider's automatic prompt-file threshold\n"
                << "  --show-thinking            show the models' reasoning (default on a TTY)\n"
                << "  --no-thinking              hide the models' reasoning\n"
                << "  --json                     emit one JSON event per line, tagged with provider/round\n"
                << "  --raw                      print only the answer text\n"
                << "  --tty                      render as if stdout/stderr were terminals, even through a pipe\n"
                << "  --copy                     copy the chosen context to the clipboard\n"
                << "  --no-color                 disable colours\n"
                << "  --debug                    print raw stream lines to stderr\n"
                << "  --timeout SECONDS          per-request timeout in seconds (default: 600)\n"
                << "  --base-url URL             API root for the API providers (lms/openai); overrides\n"
                << "                             their environment variable\n"
                << "  --api-key KEY              API key for the API providers\n"
                << "  --param KEY=VALUE          extra request parameter for the API providers (repeatable)\n"
                << "  --browser BROWSER          restrict the sessions to a browser id (repeatable)\n"
                << "  --profile PROFILE          restrict the sessions to a profile\n"
                << "  --family {chromium,firefox}\n"
                << "  --open-only                only use browsers/profiles that are currently running\n"
                << "  --exact                    match the session hosts exactly\n"
                << "  --print-session            show the session each provider would use and exit\n"
                << "  --print-prompt             print the first turn and the verification template and exit\n"
                << "  --list                     list the available providers and exit\n"
                << "  --context-file PATH        write the chosen context here (used by the .ai.debate sink)\n"
                << "  --width COLS               render width (used by the .ai.debate sink)\n"
                << "  --height ROWS              render height (used by the .ai.debate sink)\n"
                << "\n"
                << "providers: " << join(names, ", ")
                << " (dseek = deepseek, lmstudio = lms, oai = openai, pplx = perplexity)\n";
        }

        void print_provider_list(std::map<std::string, provider*> const& providers) {
            for (auto const& [name, instance] : providers) {
                std::string line = name;
                if (line.size() < 10) {
                    line.append(10 - line.size(), ' ');
                }
                line              += ' ';
                std::string label  = instance->info().label;
                line              += label;
                if (label.size() < 10) {
                    line.append(10 - label.size(), ' ');
                }
                line += ' ';
                if (instance->info().models.empty()) {
                    line += '-';
                } else {
                    line += join(instance->info().models, ", ");
                }
                std::cout << line << '\n';
            }
        }

        std::map<std::string, session::match> load_sessions(
          std::vector<std::string> const&         names,
          std::map<std::string, provider*> const& providers,
          debate_options const&                   options) {
            std::map<std::string, session::match> matches;
            for (auto const& name : names) {
                provider* instance = providers.at(name);
                if (!instance->info().needs_session) {
                    // An API provider is configured, not signed in.
                    matches[name] = session::match{};
                    continue;
                }
                session::options session_options;
                session_options.browser   = options.browser;
                session_options.profile   = options.profile;
                session_options.family    = options.family;
                session_options.open_only = options.open_only;
                session_options.exact     = options.exact;
                try {
                    auto const found = session::load(instance->info().session_target, session_options);
                    if (!found) {
                        throw debate_error("no signed-in session found for " + instance->info().session_target +
                                           "; open " + instance->info().origin + " in a browser once");
                    }
                    matches[name] = *found;
                } catch (session::session_error const& failure) {
                    throw debate_error(failure.what());
                }
            }
            return matches;
        }

        json session_summary(session::match const& value) {
            json report          = json::object();
            report["browser"]    = value.browser;
            report["profile"]    = value.profile;
            report["path"]       = value.path;
            report["is_open"]    = value.is_open;
            report["user_agent"] = value.user_agent;
            report["cookies"]    = value.cookies.is_array() ? value.cookies.size() : 0;
            auto collect         = [](json const& store, std::vector<std::string>& into) {
                if (!store.is_object()) {
                    return;
                }
                for (auto const& [key, item] : store.items()) {
                    (void) item;
                    into.push_back(key);
                }
                std::ranges::sort(into);
            };
            std::vector<std::string> local;
            std::vector<std::string> session_keys;
            collect(session::localstorage(value), local);
            collect(session::sessionstorage(value), session_keys);
            report["localstorage_keys"]   = local;
            report["sessionstorage_keys"] = session_keys;
            return report;
        }

        void check_files(std::vector<std::string> const& paths) {
            for (auto const& path : paths) {
                std::string const full = expand_user(path);
                std::error_code   ec;
                if (!fs::exists(full, ec)) {
                    throw debate_error(path + ": no such file");
                }
                if (!fs::is_regular_file(full, ec)) {
                    throw debate_error(path + ": not a regular file");
                }
                if (::access(full.c_str(), R_OK) != 0) {
                    throw debate_error(path + ": permission denied");
                }
            }
        }

        std::optional<std::map<std::string, state::conversation_record>> saved_debate(
          std::vector<std::string> const& names,
          debate_options const&           options) {
            if (options.new_) {
                state::clear_debate(names);
                return std::nullopt;
            }
            if (!options.continue_) {
                return std::nullopt;
            }
            auto saved = state::load_debate(names);
            if (!saved) {
                std::cerr << "[ai.debate] no saved debate to continue; starting a new one\n";
                return std::nullopt;
            }
            std::vector<std::string> continuing;
            std::vector<std::string> missing;
            for (auto const& name : names) {
                (saved->contains(name) ? continuing : missing).push_back(name);
            }
            std::cerr << "[ai.debate] continuing " << join(continuing, ", ") << '\n';
            if (!missing.empty()) {
                std::cerr << "[ai.debate] no saved chat for " << join(missing, ", ") << "; starting a new one\n";
            }
            return saved;
        }

        // One AI in the panel: its provider, model and open conversation.
        class participant_impl {
          public:
            participant_impl(
              std::string                   name,
              provider&                     instance,
              std::unique_ptr<conversation> chat,
              cli_options                   args,
              std::function<void()>         saver)
              : name_(std::move(name)),
                instance_(&instance),
                chat_(std::move(chat)),
                args_(std::move(args)),
                saver_(std::move(saver)) {}

            [[nodiscard]] std::string const& name() const {
                return name_;
            }

            // The saved-state entry for this participant (see `ai.state`).
            [[nodiscard]] state::conversation_record record() const {
                state::conversation_record value;
                value.conversation_id = chat_->conversation_id();
                value.model           = chat_->model();
                value.history         = chat_->history();
                value.resume          = chat_->resume();
                return value;
            }

            std::pair<int, std::string> ask(std::string const& prompt, json const& fields) {
                turn_options turn;
                turn.json         = args_.json;
                turn.raw          = args_.raw;
                turn.tty          = args_.tty;
                turn.no_color     = args_.no_color;
                turn.debug        = args_.debug;
                turn.thinking     = args_.thinking;
                turn.prompt_file  = args_.prompt_file;
                turn.prompt_limit = args_.prompt_limit;
                turn.files        = chat_->turns() == 0 ? args_.files : std::vector<std::string>{};
                turn.context_file.clear();
                turn.width           = args_.width;
                turn.height          = args_.height;
                turn.json_fields     = fields;
                renderer::writer out = [](std::string_view chunk) {
                    std::cout.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                    std::cout.flush();
                };
                renderer::writer err = [](std::string_view chunk) {
                    std::cerr.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                    std::cerr.flush();
                };
                auto result = run_turn(*chat_, *instance_, prompt, turn, out, err);
                if (result.status == 0 && !trim(result.answer).empty() && saver_) {
                    saver_();
                }
                return {result.status, std::move(result.answer)};
            }

            void close() {
                try {
                    chat_->close();
                } catch (std::exception const& failure) {
                    note("closing " + name_ + ": " + failure.what(), std::string_view{"ai.debate"});
                }
            }

          private:
            std::string                   name_;
            provider*                     instance_;
            std::unique_ptr<conversation> chat_;
            cli_options                   args_;
            std::function<void()>         saver_;
        };

        void open_participants(
          std::vector<participant_impl>&                                          participants,
          std::vector<std::string> const&                                         names,
          std::map<std::string, provider*> const&                                 providers,
          std::map<std::string, session::match> const&                            matches,
          std::map<std::string, std::string> const&                               models,
          debate_options const&                                                   options,
          std::optional<std::map<std::string, state::conversation_record>> const& saved) {
            std::function<void()> save_all = [&names, &participants] {
                std::map<std::string, state::conversation_record> records;
                for (auto const& participant : participants) {
                    records[participant.name()] = participant.record();
                }
                state::save_debate(names, records);
            };
            try {
                for (auto const& name : names) {
                    provider*                                 instance = providers.at(name);
                    std::optional<state::conversation_record> resume;
                    if (saved) {
                        auto const found = saved->find(name);
                        if (found != saved->end()) {
                            resume = found->second;
                        }
                    }
                    std::string requested;
                    auto const  chosen = models.find(name);
                    if (chosen != models.end()) {
                        requested = chosen->second;
                    } else if (resume) {
                        requested = resume->model;
                    }
                    std::string model;
                    try {
                        model = instance->resolve_model(requested, instance->info().default_base_url);
                    } catch (std::exception const&) {
                        // The open() call reports the actionable error with the endpoint's hint.
                        model = requested;
                    }
                    open_options open;
                    open.system   = options.system;
                    open.model    = model;
                    open.debug    = options.debug;
                    open.timeout  = static_cast<long>(options.timeout);
                    open.base_url = options.base_url;
                    open.api_key  = options.api_key;
                    open.params   = options.params;
                    if (resume) {
                        open.conversation_id = resume->conversation_id;
                        open.history         = resume->history;
                        open.resume          = resume->resume;
                    }
                    std::unique_ptr<conversation> chat;
                    try {
                        chat = instance->open(matches.at(name), model, open);
                    } catch (std::exception const& failure) {
                        throw debate_error(failure.what());
                    }
                    cli_options args = [&options] {
                        cli_options value;
                        value.files        = options.files;
                        value.prompt_file  = options.prompt_file;
                        value.prompt_limit = options.prompt_limit;
                        value.system       = options.system;
                        value.thinking     = options.thinking;
                        value.json         = options.json;
                        value.raw          = options.raw;
                        value.tty          = options.tty;
                        value.no_color     = options.no_color;
                        value.debug        = options.debug;
                        value.timeout      = static_cast<long>(options.timeout);
                        value.base_url     = options.base_url;
                        value.api_key      = options.api_key;
                        value.params       = options.params;
                        value.width        = options.width;
                        value.height       = options.height;
                        return value;
                    }();
                    participants.emplace_back(name, *instance, std::move(chat), std::move(args), save_all);
                }
            } catch (...) {
                for (auto& participant : participants) {
                    participant.close();
                }
                participants.clear();
                throw;
            }
        }

        debate_turn_observer header_printer(debate_options const& options) {
            return [&options](int                               turn_no,
                              int                               total,
                              int                               round_no,
                              std::string const&                name,
                              std::optional<std::string> const& previous) {
                if (turn_no > 1 && !options.json) {
                    std::cout << '\n';
                }
                std::string arrow;
                if (previous) {
                    arrow = " ← " + *previous;
                }
                std::cerr << "[ai.debate] turn " << turn_no << '/' << total << " · " << name << " · round " << round_no
                          << '/' << options.rounds << arrow << '\n';
            };
        }

    } // namespace

    // ---------------------------------------------------------------------------
    // Public pieces (declared in debate.hpp; the CLI entry point in cli.hpp)
    // ---------------------------------------------------------------------------

    std::string debate_parse_verdict(std::string_view answer) {
        return parse_verdict(answer);
    }

    std::string debate_build_prompt(
      std::string_view                                          question,
      std::optional<std::pair<std::string, std::string>> const& previous,
      bool                                                      first,
      std::string_view                                          instruction) {
        return build_prompt(question, previous, first, instruction);
    }

    debate_result run_debate_panel(
      std::vector<debate_participant> const& participants,
      std::string const&                     question,
      int                                    rounds,
      bool                                   until_agree,
      std::string const&                     instruction,
      debate_writer const&                   writer,
      debate_turn_observer const&            on_turn) {
        return run_panel(participants, question, rounds, until_agree, instruction, writer, on_turn);
    }

    int run_debate_cli(std::vector<std::string> const& argv) {
        debate_options options;
        try {
            options = parse_options(argv);
        } catch (debate_usage_error const& failure) {
            std::cerr << "ai.debate: " << failure.what() << '\n';
            print_usage(std::cerr);
            return 2;
        }

        std::map<std::string, provider*> providers;
        for (auto* instance : all_providers()) {
            providers[instance->info().name] = instance;
        }

        if (options.help) {
            render_help(std::cout, providers);
            return 0;
        }
        if (options.list) {
            print_provider_list(providers);
            return 0;
        }

        std::vector<std::string>           names;
        std::string                        question;
        std::string                        instruction;
        std::map<std::string, std::string> models;
        try {
            auto split                           = split_providers(options.words);
            names                                = std::move(split.first);
            std::vector<std::string> const words = std::move(split.second);
            if (names.size() < 2) {
                std::vector<std::string> known;
                known.reserve(providers.size());
                for (auto const& [name, instance] : providers) {
                    (void) instance;
                    known.push_back(name);
                }
                throw debate_usage_error(
                  "name at least two providers, e.g. `ai.debate grok dseek` (one of: " + join(known, ", ") + ")");
            }
            {
                std::set<std::string> seen;
                for (auto const& name : names) {
                    if (!seen.insert(name).second) {
                        throw debate_usage_error("each provider may appear only once");
                    }
                }
            }
            if (options.rounds < 1) {
                throw debate_usage_error("--rounds must be at least 1");
            }
            question = join_words(words);
            if (options.stdin_only && !question.empty()) {
                throw debate_usage_error(
                  "positional prompt text is not supported in a chain; put the question in a "
                  "`.note \"...\"` before the sink or pipe it on stdin");
            }
            if (question.empty() && ::isatty(STDIN_FILENO) == 0) {
                question = read_prompt({});
            }
            if (question.empty()) {
                throw debate_usage_error("no question given; pass it after the providers or pipe it on stdin");
            }
            models      = parse_models(options.model_specs, names);
            instruction = instruction_for(options);
            if (options.continue_ && options.new_) {
                throw debate_usage_error("-c/--continue and --new are mutually exclusive");
            }
        } catch (debate_usage_error const& failure) {
            std::cerr << "ai.debate: " << failure.what() << '\n';
            print_usage(std::cerr);
            return 2;
        }

        if (options.print_prompt) {
            print_plan(names, question, instruction);
            return 0;
        }

        std::map<std::string, session::match> matches;
        try {
            matches = load_sessions(names, providers, options);
        } catch (debate_error const& failure) {
            std::cerr << "ai.debate: " << failure.what() << '\n';
            return 1;
        }

        if (options.print_session) {
            nlohmann::ordered_json reports = nlohmann::ordered_json::object();
            for (auto const& name : names) {
                provider*    instance = providers.at(name);
                open_options open;
                open.base_url = options.base_url;
                open.api_key  = options.api_key;
                open.params   = options.params;
                if (instance->info().api) {
                    reports[name] = instance->session_descriptor(open);
                } else {
                    reports[name] = session_summary(matches.at(name));
                }
            }
            std::cout << reports.dump(2) << '\n';
            return 0;
        }

        try {
            check_files(options.files);
        } catch (debate_error const& failure) {
            std::cerr << "ai.debate: " << failure.what() << '\n';
            return 1;
        }

        auto saved = saved_debate(names, options);

        // The debate writes --context-file itself, once per completed turn.
        debate_options turn_options = options;
        turn_options.context_file.clear();

        context_writer writer(options.context_file, options.transcript ? "all" : options.context, question);
        if (writer.has_error()) {
            std::cerr << "ai.debate: --context-file " << options.context_file << ": " << writer.error() << '\n';
            return 2;
        }

        int                           status = 0;
        std::vector<participant_impl> participants;
        try {
            open_participants(participants, names, providers, matches, models, turn_options, saved);
        } catch (debate_error const& failure) {
            std::cerr << "ai.debate: " << failure.what() << '\n';
            return 1;
        }

        try {
            std::vector<debate_participant> panel;
            panel.reserve(participants.size());
            for (auto& participant : participants) {
                participant_impl* entry = &participant;
                panel.push_back({
                  .name = participant.name(),
                  .ask =
                    [entry](std::string const& prompt, json const& fields) {
                        return entry->ask(prompt, fields);
                    },
                });
            }
            auto const result = run_panel(
              panel,
              question,
              options.rounds,
              options.until_agree,
              instruction,
              [&writer](std::string const& name, int round_no, std::string const& answer) {
                  writer.add(name, round_no, answer);
              },
              header_printer(options));
            status = result.status;
            if (status == 0) {
                std::string note_text;
                if (result.stop_reason == "agreement") {
                    note_text = "agreement after " + std::to_string(result.transcript.size()) + " turns";
                } else if (options.until_agree) {
                    note_text = "no agreement after " + std::to_string(result.transcript.size()) + " turns";
                } else {
                    note_text = "done after " + std::to_string(result.transcript.size()) + " turns";
                }
                std::cerr << "[ai.debate] " << note_text << '\n';
            } else {
                std::cerr << "[ai.debate] " << result.failed_provider << " failed; stopping\n";
            }
        } catch (std::exception const& failure) {
            std::cerr << "ai.debate: " << failure.what() << '\n';
            status = 1;
        }

        for (auto& participant : participants) {
            participant.close();
        }
        if (options.copy) {
            std::string const text = writer.text();
            if (!text.empty()) {
                copy_answer(text);
            }
        }
        writer.close();
        return status;
    }

} // namespace dana::ai
