// The offline `ai.debate` relay: turn order, the "Another AI said:" framing,
// --rounds/--until-agree stopping, the verdict parser and failure handling.
// The panel only needs `name` + an `ask` callback, so no network is involved.

#include "ai/debate.hpp"

#include <exception>
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

    void check_equal(std::string const& actual, std::string const& expected, std::string_view what) {
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

    struct fake_participant {
        std::string                              name;
        std::vector<std::pair<int, std::string>> answers;
        std::vector<std::string>                 prompts;
        std::vector<nlohmann::json>              fields;

        dana::ai::debate_participant make() {
            return {
              .name = name,
              .ask =
                [this](std::string const& prompt, nlohmann::json const& turn_fields) {
                    prompts.push_back(prompt);
                    fields.push_back(turn_fields);
                    auto const answer = answers.front();
                    answers.erase(answers.begin());
                    return answer;
                },
            };
        }
    };

    fake_participant make_fake(std::string name, std::vector<std::pair<int, std::string>> answers) {
        fake_participant value;
        value.name    = std::move(name);
        value.answers = std::move(answers);
        return value;
    }

    struct collected {
        std::vector<std::pair<std::string, int>> providers;
        std::vector<std::string>                 answers;
    };

    dana::ai::debate_result run(
      std::vector<fake_participant>& fakes,
      std::string const&             question,
      int                            rounds,
      bool                           until_agree,
      std::string const&             instruction,
      collected&                     writer) {
        std::vector<dana::ai::debate_participant> participants;
        participants.reserve(fakes.size());
        for (auto& fake : fakes) {
            participants.push_back(fake.make());
        }
        return dana::ai::run_debate_panel(
          participants,
          question,
          rounds,
          until_agree,
          instruction,
          [&writer](std::string const& provider, int round, std::string const& answer) {
              writer.providers.emplace_back(provider, round);
              writer.answers.push_back(answer);
          },
          {});
    }

    void test_build_prompt() {
        using dana::ai::debate_build_prompt;
        check_equal(debate_build_prompt("Q?", std::nullopt, true, ""), "Q?", "first turn is the question");
        check_equal(debate_build_prompt("Q?", std::nullopt, true, "I"),
                    "Q?",
                    "the opening question carries no instruction");
        std::optional<std::pair<std::string, std::string>> const previous{
          {"grok", "A1"}
        };
        std::string const first = debate_build_prompt("Q?", previous, true, "I");
        check(first.starts_with("Q?\n\n"), "a participant's first turn gets the question");
        check(first.contains("Another AI said:\n\nA1"), "framing");
        check(first.ends_with("\n\nI"), "instruction appended");
        std::string const later = debate_build_prompt("Q?", previous, false, "I");
        check(!later.contains("Q?"), "a later turn omits the question");
        check(later.contains("Another AI said:\n\nA1"), "later framing");
        check(later.ends_with("\n\nI"), "later instruction");
    }

    void test_verdict() {
        using dana::ai::debate_parse_verdict;
        check_equal(debate_parse_verdict("some text\nVERDICT: DISAGREE\nmore\nverdict: agree\n"),
                    "AGREE",
                    "last verdict wins");
        check_equal(debate_parse_verdict("VERDICT: DISAGREE"), "DISAGREE", "disagree");
        check_equal(debate_parse_verdict("no verdict here"), "", "no verdict");
        check_equal(debate_parse_verdict("  VERDICT: DISAGREE  "), "DISAGREE", "disagree is not agree");
    }

    void test_ring_order_and_rounds() {
        std::vector<fake_participant> fakes{
          make_fake("grok", {{0, "a1"}, {0, "a2"}}
           ),
          make_fake("dseek", {{0, "b1"}, {0, "b2"}}
           ),
        };
        collected  writer;
        auto const result = run(fakes, "Q?", 2, false, "I", writer);
        check(result.status == 0, "status");
        check_equal(result.stop_reason, "rounds", "stop reason");
        check_equal(result.transcript.size(), std::size_t{4}, "transcript length");
        check_equal(result.transcript[0].provider, "grok", "turn 0 provider");
        check_equal(result.transcript[1].provider, "dseek", "turn 1 provider");
        check_equal(result.transcript[2].provider, "grok", "turn 2 provider");
        check_equal(result.transcript[3].provider, "dseek", "turn 3 provider");
        check(result.transcript[2].round == 2, "round 2");
        check_equal(writer.providers.size(), std::size_t{4}, "writer turns");
        check_equal(fakes[0].prompts[0], "Q?", "the very first turn is the bare question");
        check(fakes[1].prompts[0].contains("Q?"), "dseek's first turn knows the question");
        check(fakes[1].prompts[0].contains("a1"), "dseek sees grok's answer");
        check(!fakes[0].prompts[1].contains("Q?"), "grok's second turn omits the question");
        check(fakes[0].prompts[1].contains("b1"), "grok sees dseek's answer");
        check(fakes[0].fields[0].value("turn", 0) == 1, "first turn number");
        check_equal(fakes[0].fields[0].value("provider", ""), "grok", "first turn provider");
        check_equal(fakes[1].fields[0].value("reply_to", ""), "grok", "reply_to");
    }

    void test_three_providers() {
        std::vector<fake_participant> fakes{
          make_fake("grok", {{0, "a1"}, {0, "a2"}}
           ),
          make_fake("dseek", {{0, "b1"}, {0, "b2"}}
           ),
          make_fake("kimi", {{0, "c1"}, {0, "c2"}}
           ),
        };
        collected  writer;
        auto const result = run(fakes, "Q?", 2, false, "", writer);
        check_equal(result.transcript.size(), std::size_t{6}, "six turns");
        check_equal(result.transcript[2].provider, "kimi", "third turn is kimi");
        check(fakes[2].prompts[0].contains("Q?"), "kimi's first turn knows the question");
        check(fakes[2].prompts[0].contains("b1"), "kimi sees dseek's answer");
    }

    void test_until_agree_stops() {
        std::vector<fake_participant> fakes{
          make_fake("grok", {                {0, "a1"}, {0, "a2\nVERDICT: AGREE"}}
           ),
          make_fake("dseek", {{0, "b1\nVERDICT: AGREE"}, {0, "b2\nVERDICT: AGREE"}}
           ),
        };
        collected  writer;
        auto const result = run(fakes, "Q?", 5, true, "I", writer);
        check(result.status == 0, "status");
        check_equal(result.stop_reason, "agreement", "agreement stop");
        check(result.rounds_run == 2, "two rounds");
        check_equal(result.transcript.size(), std::size_t{4}, "four turns");
    }

    void test_until_agree_keeps_going() {
        std::vector<fake_participant> fakes{
          make_fake("grok", {                {0, "a1"}, {0, "a2\nVERDICT: DISAGREE"}}
           ),
          make_fake("dseek", {{0, "b1\nVERDICT: AGREE"},    {0, "b2\nVERDICT: AGREE"}}
           ),
        };
        collected  writer;
        auto const result = run(fakes, "Q?", 2, true, "I", writer);
        check_equal(result.stop_reason, "rounds", "rounds stop");
        check(result.rounds_run == 2, "two rounds");
    }

    void test_failed_turn_stops() {
        std::vector<fake_participant> fakes{
          make_fake("grok", {     {0, "a1"}}
           ),
          make_fake("dseek", {{1, "partial"}}
           ),
        };
        collected  writer;
        auto const result = run(fakes, "Q?", 2, false, "I", writer);
        check(result.status == 1, "failure status");
        check_equal(result.stop_reason, "failed", "failure stop");
        check_equal(result.failed_provider, "dseek", "failed provider");
        check_equal(writer.providers.size(), std::size_t{1}, "only grok's turn was written");
    }

    void test_empty_answer_fails() {
        std::vector<fake_participant> fakes{
          make_fake("grok", {{0, "  "}}
           ),
          make_fake("dseek", {{0, "b1"}}
           ),
        };
        collected  writer;
        auto const result = run(fakes, "Q?", 1, false, "", writer);
        check(result.status == 1, "empty answer status");
        check_equal(result.failed_provider, "grok", "empty answer provider");
    }

} // namespace

int main() {
    try {
        test_build_prompt();
        test_verdict();
        test_ring_order_and_rounds();
        test_three_providers();
        test_until_agree_stops();
        test_until_agree_keeps_going();
        test_failed_turn_stops();
        test_empty_answer_fails();
    } catch (std::exception const& failure) {
        std::cerr << "test_ai_debate: " << failure.what() << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "test_ai_debate: ok\n";
    return 0;
}
