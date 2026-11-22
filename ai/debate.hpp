#pragma once

// `ai.debate`: ask two or more AIs the same question, then relay each answer to
// the next participant as "Another AI said: …" with a verification request.
// Every participant keeps its own conversation, so a later turn only carries the
// other AI's newest answer — the same thing two chat tabs do by hand.
//
// The loop is deliberately independent of the network: `run_debate_panel` drives
// plain `debate_participant` callbacks (which is what the offline test uses),
// while `run_debate_cli` wires the real providers, the context-file writer and
// the saved-debate state around it.

#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dana::ai {

    //: The default verification request appended to every relay turn.
    inline constexpr std::string_view kDebateDefaultInstruction =
      "Verify the other AI's answer against the question above. Point out anything wrong, unsupported or missing, and "
      "say where you agree. Then give your own corrected answer; if the other answer is already right, say so and keep "
      "it.";

    //: Added to the instruction by `--until-agree` so the relay can stop early.
    inline constexpr std::string_view kDebateVerdictSuffix =
      "End your answer with one line: `VERDICT: AGREE` if you agree with the other answer's conclusion, or "
      "`VERDICT: DISAGREE` if you still disagree.";

    struct debate_turn {
        int         round = 0;
        std::string provider;
        std::string answer;
    };

    struct debate_result {
        int                      status = 0;
        std::vector<debate_turn> transcript;
        std::string              stop_reason = "rounds"; // rounds | agreement | failed
        std::string              failed_provider;
        int                      rounds_run = 0;
    };

    // The last `VERDICT: AGREE|DISAGREE` line of an answer, or "" when absent.
    std::string debate_parse_verdict(std::string_view answer);

    // One turn's prompt.  The very first turn is the question verbatim; a
    // participant's first turn (when it is not the very first turn) also gets the
    // question, because its conversation has not seen it yet; later turns carry
    // only the newest answer.
    std::string debate_build_prompt(
      std::string_view                                          question,
      std::optional<std::pair<std::string, std::string>> const& previous,
      bool                                                      first,
      std::string_view                                          instruction);

    // A participant the panel loop can drive without a network.
    struct [[nodiscard]] debate_participant {
        std::string                                                                         name;
        std::function<std::pair<int, std::string>(std::string_view, nlohmann::json const&)> ask;
    };

    using debate_writer = std::function<void(std::string_view provider, int round, std::string_view answer)>;
    using debate_turn_observer =
      std::function<void(int                               turn_no,
                         int                               total,
                         int                               round_no,
                         std::string_view                  provider,
                         std::optional<std::string> const& previous_provider)>;

    // Run the relay and return its result.  *writer* receives each completed answer;
    // *on_turn* (optional) is called before each turn.
    debate_result run_debate_panel(
      std::vector<debate_participant> const& participants,
      std::string_view                       question,
      int                                    rounds,
      bool                                   until_agree,
      std::string_view                       instruction,
      debate_writer const&                   writer,
      debate_turn_observer const&            on_turn);

} // namespace dana::ai
