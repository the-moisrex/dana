#include "../sdk/prompt_registry.hpp"

#include <ostream>
#include <string>

namespace dana {

    static void render_help_docstring(std::ostream& os);

    // The docstring instructions. Deliberately data-free: it takes no arguments and
    // embeds nothing, so `prompt .files a.cpp .docstring` adds the code exactly once
    // (from `.files`) and then these instructions.
    static void execute_docstring(view const& v, result& r) {
        if (!v.args.empty()) {
            throw usage_error{
              "prompt docstring: takes no arguments — it only prints the docstring instructions.\n"
              "Chain it after a file prompt, e.g. `prompt .files a.cpp .docstring`.\n" +
              help_text(render_help_docstring)};
        }

        r.out =
          R"PROMPT(Add comprehensive docstrings to all functions, classes, and methods in the code in the prompt.
Ensure the docstrings clearly describe:
1. The overall purpose and behavior.
2. The parameters (including their expected types).
3. The return values (and types).
4. Any exceptions or errors that might be raised.
5. Don't explain trivial or add useless information.
Adhere to standard documentation conventions for the specific language if they exist (e.g., PEP 257/Google style for Python, JSDoc for JavaScript/TypeScript, JavaDoc for Java).
Return the updated code with the newly added docstrings or write a patch file.
)PROMPT";
    }

    static void render_help_docstring(std::ostream& os) {
        os << R"EOF(Usage: prompt docstring

Asks the AI to add or improve docstrings for functions, classes, and methods in the code in the prompt. This prompt carries no data and takes no arguments: chain it after a file prompt, e.g. `prompt .files a.cpp .docstring`.
)EOF";
    }

    // This file is the prompt: it implements it and registers it at static-init time
    // (no central list — see sdk/prompt_registry.hpp).
    [[maybe_unused]] bool const registered = dana::register_prompt(
      "docstring",
      "Asks the AI to add or improve docstrings for functions, classes, and methods in "
      "the code in the prompt. This prompt carries no data and takes no arguments: chain "
      "it after a file prompt, e.g. `prompt .files a.cpp .docstring`.",
      &execute_docstring,
      &render_help_docstring);

} // namespace dana
