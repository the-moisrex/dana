#include "../sdk/prompt_registry.hpp"

#include <ostream>
#include <string>

namespace dana {

    static void render_help_bigo(std::ostream& os);

    // The Big O instructions. Deliberately data-free: it takes no arguments and
    // embeds nothing, so `prompt .files a.cpp .bigo` adds the code exactly once
    // (from `.files`) and then these instructions.
    static void execute_bigo(view const& v, result& r) {
        if (!v.args.empty()) {
            throw usage_error{
              "prompt bigo: takes no arguments — it only prints the Big O instructions.\n"
              "Chain it after a file prompt, e.g. `prompt .files a.cpp .bigo`.\n" +
              help_text(render_help_bigo)};
        }

        r.out =
          R"PROMPT(Calculate the Big O notation for the time and space complexity of the algorithms in the prompt.
Provide a clear step-by-step breakdown of your reasoning.
Explicitly define what any variables (e.g., N, M, V, E) represent in the context of the inputs.
If there are multiple functions or methods, analyze each one individually.
ALWAYS format your Big O notations using math delimiters, such as \mathcal{O}(N) or \mathcal{O}(N \log N).
)PROMPT";
    }

    static void render_help_bigo(std::ostream& os) {
        os << R"EOF(Usage: prompt bigo

Asks the AI to calculate the Big O time and space complexity of the algorithms in the prompt. This prompt carries no data and takes no arguments: chain it after a file prompt, e.g. `prompt .files a.cpp .bigo`.
)EOF";
    }

    // This file is the prompt: it implements it and registers it at static-init time
    // (no central list — see sdk/prompt_registry.hpp).
    [[maybe_unused]] bool const registered = dana::register_prompt(
      "bigo",
      "Asks the AI to calculate the Big O time and space complexity of the algorithms in "
      "the prompt. This prompt carries no data and takes no arguments: chain it after a "
      "file prompt, e.g. `prompt .files a.cpp .bigo`.",
      &execute_bigo,
      &render_help_bigo);

} // namespace dana
