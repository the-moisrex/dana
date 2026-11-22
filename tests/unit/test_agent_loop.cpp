// The provider-independent agentic loop behind `.ai.loop`: turn order, the
// tool feedback prompt, the no-call stop, exhaustion, failure and the
// per-result truncation. Every side effect is injected, so no provider or
// network is involved.

#include "../../ai/agent_loop.hpp"

#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

    int failures = 0;

    void check(bool condition, std::string_view what) {
        if (!condition) {
            std::cerr << "FAIL: " << what << '\n';
            ++failures;
        }
    }

    void check_equal(std::string_view actual, std::string_view expected, std::string_view what) {
        if (actual != expected) {
            std::cerr << "FAIL: " << what << "\n  expected: " << expected << "\n  actual:   " << actual << '\n';
            ++failures;
        }
    }

    void check_equal(std::size_t actual, std::size_t expected, std::string_view what) {
        if (actual != expected) {
            std::cerr << "FAIL: " << what << "\n  expected: " << expected << "\n  actual:   " << actual << '\n';
            ++failures;
        }
    }

    dana::ai::turn_result answered(std::string text) {
        dana::ai::turn_result result;
        result.answer = std::move(text);
        return result;
    }

    void test_stops_without_calls() {
        std::vector<std::string>     prompts;
        dana::ai::agent_loop_options options;
        dana::ai::agent_loop_io      io;
        io.turn = [&](std::string_view prompt) {
            prompts.emplace_back(prompt);
            return answered("done");
        };
        io.run_tools = [](std::string_view) {
            return std::optional<std::string>{};
        };

        auto const result = dana::ai::run_agent_loop("task", options, io);
        check_equal(result.turns, std::size_t{1}, "one turn");
        check_equal(result.answer, "done", "the answer is the context");
        check_equal(result.transcript, "done\n", "the transcript holds the answer");
        check(!result.exhausted, "not exhausted");
        check(result.status == 0, "success");
        check_equal(prompts.size(), std::size_t{1}, "one prompt");
        check_equal(prompts.front(), "task", "the initial prompt is the task");
    }

    void test_feeds_tool_results_back() {
        std::vector<std::string>     prompts;
        std::vector<std::string>     seen;
        dana::ai::agent_loop_options options;
        dana::ai::agent_loop_io      io;
        io.turn = [&](std::string_view prompt) {
            prompts.emplace_back(prompt);
            return answered(prompts.size() == 1 ? "calling" : "finished");
        };
        io.run_tools = [&](std::string_view answer) -> std::optional<std::string> {
            seen.emplace_back(answer);
            if (answer == "calling") {
                return "### Tool: read\nbody";
            }
            return std::nullopt;
        };

        auto const result = dana::ai::run_agent_loop("task", options, io);
        check_equal(result.turns, std::size_t{2}, "two turns");
        check_equal(result.answer, "finished", "the last answer wins");
        check_equal(prompts.size(), std::size_t{2}, "two prompts");
        check_equal(prompts[1], "### Tool: read\nbody", "the tool block is the next prompt");
        check_equal(seen.size(), std::size_t{2}, "every answer is inspected");
        check_equal(result.transcript,
                    "calling\n\n### Tool: read\nbody\n\nfinished\n",
                    "the transcript folds every turn");
    }

    void test_exhaustion() {
        int                          turns = 0;
        dana::ai::agent_loop_options options;
        options.max_turns = 3;
        dana::ai::agent_loop_io io;
        io.turn = [&](std::string_view) {
            ++turns;
            return answered("still calling");
        };
        io.run_tools = [](std::string_view) {
            return std::optional<std::string>{"### Tool: cli\nok"};
        };

        auto const result = dana::ai::run_agent_loop("task", options, io);
        check_equal(turns, 3, "the turn budget is honoured");
        check(result.exhausted, "exhausted");
        check_equal(result.answer, "still calling", "the last answer is kept");
        check(result.status == 0, "exhaustion is not a failure");
    }

    void test_failed_turn() {
        dana::ai::agent_loop_options options;
        dana::ai::agent_loop_io      io;
        io.turn = [](std::string_view) {
            dana::ai::turn_result result;
            result.status = 1;
            result.answer = "partial";
            return result;
        };
        io.run_tools = [](std::string_view) {
            return std::optional<std::string>{"unused"};
        };

        auto const result = dana::ai::run_agent_loop("task", options, io);
        check(result.status != 0, "the failure is reported");
        check_equal(result.answer, "partial", "the partial answer is kept");
        check_equal(result.turns, std::size_t{1}, "the loop stops at the failure");
        check_equal(result.transcript, "partial\n", "the partial answer is in the transcript");
    }

    void test_status_callback() {
        std::string                  streamed;
        dana::ai::agent_loop_options options;
        options.show_tools = true;
        dana::ai::agent_loop_io io;
        io.turn = [](std::string_view) {
            return answered("answer");
        };
        io.run_tools = [](std::string_view) {
            return std::optional<std::string>{"### Tool: cli\nok"};
        };
        io.status = [&](std::string_view text) {
            streamed += text;
        };

        // max_turns 1 still calls run_tools once, so the status callback fires.
        options.max_turns = 1;
        (void) dana::ai::run_agent_loop("task", options, io);
        check_equal(streamed, "### Tool: cli\nok", "the tool block is streamed when asked");
    }

    void test_truncate() {
        check_equal(dana::ai::truncate_tool_result("short", 400, 64 * 1024), "short", "short text is untouched");

        std::string many;
        for (int i = 0; i < 10; ++i) {
            many += "line ";
            many += std::to_string(i);
            many += '\n';
        }
        auto const head = dana::ai::truncate_tool_result(many, 3, 0);
        check(head.starts_with("line 0\nline 1\nline 2\n"), "the head lines are kept");
        check(head.find("[truncated: ") != std::string::npos, "a truncation note is appended");
        check(head.find("first 3 lines kept") != std::string::npos, "the note names the line cap");

        auto const bytes = dana::ai::truncate_tool_result(many, 0, 10);
        check(bytes.starts_with("line 0\nlin"), "the byte cap is honoured");
        check(bytes.find("[truncated: ") != std::string::npos, "a byte truncation is noted");

        check_equal(dana::ai::truncate_tool_result(many, 0, 0), many, "no limits means no change");
    }

} // namespace

int main() {
    test_stops_without_calls();
    test_feeds_tool_results_back();
    test_exhaustion();
    test_failed_turn();
    test_status_callback();
    test_truncate();

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "agent_loop: all checks passed\n";
    return 0;
}
