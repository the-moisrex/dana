#include "tool_exec.hpp"

#include "errors.hpp"
#include "policy.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dana::tool_exec {

    namespace {

        std::string body_of(result const& res) {
            std::string body = res.out;
            if (!res.err.empty()) {
                if (!body.empty() && !body.ends_with('\n')) {
                    body += '\n';
                }
                body += res.err;
            }
            return body;
        }

        std::string body_of(prompt_error const& err) {
            std::string            body    = std::string(err.partial_output());
            std::string_view const message = err.what();
            if (!message.empty()) {
                if (!body.empty() && !body.ends_with('\n')) {
                    body += '\n';
                }
                body += message;
            }
            return body;
        }

        void record(nlohmann::ordered_json& report,
                    tool_call::call const&  call,
                    std::string_view        body,
                    int                     exit_code,
                    bool                    ok) {
            report.push_back(nlohmann::ordered_json{
              {  "tool",  call.name},
              { "input", call.input},
              {    "ok",         ok},
              {  "exit",  exit_code},
              {"output",       body},
            });
        }

        // Tools that only read the workspace. They need no approval, so an agent loop
        // can explore freely; everything else (edit/write/cli/run) goes through the
        // policy. This is the "read-only tools run freely" rule the loop documents.
        bool is_read_only_tool(std::string_view name) {
            return name == "read" || name == "grep" || name == "glob" || name == "files";
        }

    } // namespace

    outcome run(view const& v, std::string_view text, options const& opts) {
        outcome out;
        out.calls = tool_call::extract(text);
        if (out.calls.empty()) {
            return out;
        }

        std::string dropped_note;
        if (opts.max_calls != 0 && out.calls.size() > opts.max_calls) {
            std::size_t const dropped = out.calls.size() - opts.max_calls;
            out.calls.resize(opts.max_calls);
            dropped_note = "[dropped " + std::to_string(dropped) + " more tool call(s): the per-turn limit is " +
                           std::to_string(opts.max_calls) + "]\n";
        }

        policy::engine policy_engine(v.env);
        policy_engine.set_yes(opts.yes);

        nlohmann::ordered_json report = nlohmann::ordered_json::array();
        std::string            blocks;

        for (auto const& call : out.calls) {
            std::vector<std::string> argv;
            try {
                argv = tool_call::to_argv(call);
            } catch (prompt_error const& err) {
                std::string const body  = body_of(err);
                blocks                 += tool_call::format_result(call, body, err.exit_code(), false);
                out.failed_calls.push_back(call.name);
                record(report, call, body, err.exit_code(), false);
                if (opts.abort_on_error) {
                    throw prompt_error{err.exit_code(), err.what(), blocks};
                }
                continue;
            }

            std::vector<std::string> chain{call.name};
            chain.insert(chain.end(), argv.begin(), argv.end());
            // A policy deny or a cancelled approval is the user saying no: it always
            // aborts, whatever --on-call-error says. Read-only tools skip the ask.
            if (!is_read_only_tool(call.name)) {
                policy_engine.require_chain(chain);
            }

            try {
                auto const        res   = run_sub_prompt(v, call.name, argv, std::nullopt, nullptr, false);
                std::string const body  = body_of(res);
                blocks                 += tool_call::format_result(call, body, 0, true);
                record(report, call, body, 0, true);
            } catch (prompt_error const& err) {
                std::string const body  = body_of(err);
                blocks                 += tool_call::format_result(call, body, err.exit_code(), false);
                out.failed_calls.push_back(call.name);
                record(report, call, body, err.exit_code(), false);
                if (opts.abort_on_error) {
                    throw prompt_error{err.exit_code(), err.what(), blocks};
                }
            }
        }

        out.blocks = std::move(blocks);
        if (!dropped_note.empty()) {
            out.blocks += dropped_note;
        }
        if (opts.json) {
            out.json_report = report.dump(2) + "\n";
        }
        return out;
    }

} // namespace dana::tool_exec
