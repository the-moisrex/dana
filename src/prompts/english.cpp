#include "../sdk/prompt_registry.hpp"

#include <string>
using prompt::prompts::result;
using prompt::prompts::view;

static void execute_english([[maybe_unused]] view const &inp, result &res) {
    res.out =
      "Translate to English; no explanations, no comments, no extra text, "
      "just the translation.\n\n";
}

static void render_help_english(std::ostream &os) {
    os << R"EOF(Usage: prompt english [--head N]
       some-command | prompt english [--head N]

Translate to English.

Options:
  --head N   Keep only the first N lines of the embedded context
)EOF";
}

// This file is the prompt: it implements it and registers it at static-init
// time (no central list — see sdk/prompt_registry.hpp).
bool const registered =
  prompt::register_prompt("english", "Translate to English.", &execute_english, &render_help_english);
