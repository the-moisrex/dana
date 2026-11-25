#include "agent_loop.hpp"

#include "../core/strings.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>

namespace dana::ai {

    namespace {

        // A web model sometimes falls back to its site's own agent and refuses
        // ("I can't access the workspace", "the tools aren't connected"). Detect
        // that so the loop can nudge it instead of ending on the refusal.
        bool looks_like_refusal(std::string_view answer) {
            std::string lower;
            lower.reserve(answer.size());
            for (char const character : answer) {
                lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
            }
            constexpr std::array<std::string_view, 10> negations{
              "can't",
              "cannot",
              "couldn't",
              "could not",
              "unable",
              "not able",
              "don't have",
              "do not have",
              "not available",
              "not connected"};
            constexpr std::array<std::string_view, 7>
                       subjects{"tool", "terminal", "workspace", "access", "connected", "repository", "file"};
            auto const has = [&lower](auto const& needles) {
                return std::ranges::any_of(needles, [&lower](std::string_view needle) {
                    return lower.contains(needle);
                });
            };
            return has(negations) && has(subjects);
        }

        // A fenced block in a no-call answer is a patch or a report, not a tool
        // call (the tool-call extractor already consumed any valid call). This
        // also catches refusals written in another language.
        bool contains_fenced_block(std::string_view answer) {
            return answer.contains("```");
        }

    } // namespace

    std::string truncate_tool_result(std::string_view text, std::size_t head_lines, std::size_t max_bytes) {
        std::string_view kept = text;

        if (head_lines != 0) {
            std::size_t lines = 0;
            std::size_t pos   = 0;
            while (pos < kept.size() && lines < head_lines) {
                if (kept[pos] == '\n') {
                    ++lines;
                }
                ++pos;
            }
            kept = kept.substr(0, pos);
        }

        if (max_bytes != 0 && kept.size() > max_bytes) {
            kept = kept.substr(0, max_bytes);
        }

        if (kept.size() == text.size()) {
            return std::string(text);
        }

        std::string out{kept};
        if (!out.empty() && !out.ends_with('\n')) {
            out += '\n';
        }
        out += "[truncated: ";
        out += std::to_string(text.size() - kept.size());
        out += " bytes omitted]";
        if (head_lines != 0) {
            out += " (first ";
            out += std::to_string(head_lines);
            out += " lines kept)";
        }
        out += '\n';
        return out;
    }

    agent_loop_result run_agent_loop(std::string initial, agent_loop_options const& options, agent_loop_io const& io) {
        agent_loop_result result;
        std::string       prompt = std::move(initial);
        std::string       last_answer;
        int               refusals = 0;
        bool              acted    = false;

        for (int turn = 1; turn <= options.max_turns; ++turn) {
            result.turns = turn;

            auto answered = io.turn(prompt);
            text::append_block(result.transcript, answered.answer);
            if (answered.status != 0) {
                result.status = answered.status;
                result.answer = std::move(answered.answer);
                result.error  = "the turn failed";
                return result;
            }

            auto blocks = io.run_tools(answered.answer);
            if (!blocks) {
                bool const marker = !options.completion_marker.empty() &&
                                    text::strip(text::strip_newlines(answered.answer)) == options.completion_marker;
                // A nudge can also confirm that the previous answer was the
                // finished result; keep that answer instead of the marker. Only
                // trust it when a tool actually ran and the site did not take
                // over the turn.
                if (marker && acted && !answered.used_site_tools && refusals > 0) {
                    result.answer = std::move(last_answer);
                    return result;
                }
                // A web model can fall back to its site's own agent (its Python
                // sandbox, a canvas report), refuse ("I can't access the
                // workspace", in any language) or reply with a patch instead of
                // calling a tool. Nudge it before accepting that as the answer.
                bool const retryable =
                  answered.used_site_tools || looks_like_refusal(answered.answer) ||
                  (options.nudge_on_code_answer && contains_fenced_block(answered.answer)) || (marker && !acted);
                if (refusals < options.refusal_retries && !options.refusal_nudge.empty() && retryable) {
                    ++refusals;
                    if (io.status) {
                        io.status("ai.loop: model answered without a tool call; retrying with a corrective nudge\n");
                    }
                    last_answer = std::move(answered.answer);
                    prompt      = options.refusal_nudge;
                    continue;
                }
                // The marker without any tool call is not a finished result;
                // keep the substantive answer we were nudging away from.
                if (marker && !acted && !last_answer.empty()) {
                    result.answer = std::move(last_answer);
                    return result;
                }
                result.answer = std::move(answered.answer);
                return result;
            }

            acted         = true;
            result.answer = answered.answer;
            if (options.show_tools && io.status) {
                io.status(*blocks);
            }

            text::append_block(result.transcript, *blocks);
            prompt = options.tool_result_prefix +
                     truncate_tool_result(*blocks, options.tool_head_lines, options.tool_max_bytes);
        }

        result.exhausted = true;
        return result;
    }

} // namespace dana::ai
