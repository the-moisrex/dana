#include "../sdk/agent.hpp"
#include "../sdk/errors.hpp"
#include "../sdk/prompt_registry.hpp"
#include "../sdk/sink.hpp"
#include "../sdk/stage.hpp"
#include "../sdk/tool_call.hpp"
#include "../sdk/tool_exec.hpp"

#include <cstddef>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace dana::prompts {

    namespace {

        struct run_options {
            bool print_only     = false;
            bool list_only      = false;
            bool json           = false;
            bool yes            = false;
            bool abort_on_error = false;
        };

        run_options parse_run_args(view const& v) {
            run_options opts;
            for (std::size_t i = 0; i < v.args.size(); ++i) {
                std::string_view const arg = v.args[i];
                if (arg == "--print") {
                    opts.print_only = true;
                } else if (arg == "--list") {
                    opts.list_only = true;
                } else if (arg == "--json") {
                    opts.json = true;
                } else if (arg == "--yes") {
                    opts.yes = true;
                } else if (arg == "--on-call-error") {
                    if (++i >= v.args.size()) {
                        throw usage_error{"tools.run: --on-call-error needs abort or continue\n"};
                    }
                    std::string_view const value = v.args[i];
                    if (value == "abort") {
                        opts.abort_on_error = true;
                    } else if (value == "continue") {
                        opts.abort_on_error = false;
                    } else {
                        throw usage_error{"tools.run: --on-call-error needs abort or continue\n"};
                    }
                } else if (arg == "--on-call-error=continue") {
                    opts.abort_on_error = false;
                } else if (arg == "--on-call-error=abort") {
                    opts.abort_on_error = true;
                } else {
                    throw usage_error{"tools.run: unknown option " + std::string(arg) + "\n"};
                }
            }
            return opts;
        }

    } // namespace

    static void execute_tools_run(view const& v, result& r) {
        run_options const opts = parse_run_args(v);

        auto const calls = tool_call::extract(v.text);
        if (calls.empty()) {
            r.err = "prompt tools.run: no tool calls in the context\n";
            return;
        }

        if (opts.list_only) {
            for (auto const& call : calls) {
                r.err += "tool: " + call.name + " " + call.input.dump() + "\n";
            }
            return;
        }

        if (opts.print_only) {
            for (auto const& call : calls) {
                std::vector<std::string> argv = tool_call::to_argv(call);
                std::vector<std::string> chain{"prompt", call.name};
                chain.insert(chain.end(), argv.begin(), argv.end());
                r.out += agent::join_argv(chain);
                r.out += '\n';
            }
            return;
        }

        tool_exec::options exec_options;
        exec_options.yes            = opts.yes;
        exec_options.abort_on_error = opts.abort_on_error;
        exec_options.json           = opts.json;
        auto const outcome          = tool_exec::run(v, v.text, exec_options);
        r.out                       = opts.json ? outcome.json_report : outcome.blocks;
    }

    static void render_help_tools_run(std::ostream& os) {
        os << R"EOF(Usage: prompt <prompts...> .tools.run [--print] [--list] [--json]
                                          [--on-call-error abort|continue] [--yes]

Run the tool calls in the accumulated context and replace it with their results.
A call is a fenced ```json block emitted by an AI answer:

  {"tool": "grep", "input": {"pattern": "TODO", "path": "src"}}

Inline markup is accepted too, so a DeepSeek-style answer works unchanged:

  <｜126｜ calls>
  <｜126｜ invoke name="grep">
  <｜126｜ parameter name="pattern" string="true">TODO</｜126｜ parameter>
  </｜126｜ invoke>
  </｜126｜ calls>

Each call is resolved to a registered prompt that declares an input schema, its
input is parsed into argv, the command is checked by the policy (the same
`prompt ...` chain approval `.exec` uses), and the prompt runs in-process. The
result is fed back as a `### Tool: <name>` block, so a following `.ai.reply`
continues the conversation with the results.

No calls in the context is not an error: the sink leaves the context unchanged
and notes it on stderr, so a plain answer still flows to the next segment.

)EOF";
        render_sink_help(os);
        os << R"EOF(
Tool options:
  --print                  Print the resolved `prompt ...` commands without running
  --list                   List the calls on stderr and keep the context
  --json                   Emit a JSON array of {tool, input, ok, exit, output}
  --on-call-error MODE     abort or continue after a failed call (default: continue)
  --yes                    Turn policy asks into allows (never overrides a deny)

Examples:
  prompt .note "find the TODOs" .tools --for chat --category core .ai.dseek .tools.run
  prompt .note "read main.cpp" .tools --for chat .ai.gap .tools.run --print
  prompt .note "..." .tools --for chat .ai.dseek .tools.run .ai.reply

See also: `.tools` (the catalog), `.exec` (run an argv command), `.extract`.

)EOF";
    }

    bool const registered = dana::register_sink(
      "tools.run",
      "Run the tool calls in the chain context (from an AI answer) and continue with their results.",
      &execute_tools_run,
      &render_help_tools_run,
      nullptr,
      true,
      nullptr,
      nullptr,
      false,
      true);

} // namespace dana::prompts
