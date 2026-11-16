#include "agent_loop.hpp"

#include "../core/strings.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace prompt::ai {

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
                result.answer = std::move(answered.answer);
                return result;
            }

            result.answer = answered.answer;
            if (options.show_tools && io.status) {
                io.status(*blocks);
            }

            text::append_block(result.transcript, *blocks);
            prompt = truncate_tool_result(*blocks, options.tool_head_lines, options.tool_max_bytes);
        }

        result.exhausted = true;
        return result;
    }

} // namespace prompt::ai
