#include "../runtime/args.hpp"
#include "../runtime/embed.hpp"
#include "../runtime/prompt_registry.hpp"
#include "../tools/whatwg.hpp"

#include <format>
#include <stdexcept>
#include <string>
#include <vector>

namespace dana {

    static void execute_whatwg_url_specs(view const& v, result& r) {
        auto head_lines = head_lines_or_throw(v);

        auto sections = positional_args(v);

        std::string output = R"PROMPT(Review the following sections from the WHATWG URL specification.
Ensure that any code, fixes, or analysis strictly adhere to these standard algorithms and definitions.

)PROMPT";

        if (sections.empty()) {
            // No queries: the help is streamed verbatim inside a code fence (no
            // --head applied here), preceded by the stderr warning.
            output += "```text\n";
            output += tools::whatwg_help();
            output += "```\n\n";
            r.out   = std::move(output);
            r.err   = "Warning: No queries provided. Use 'list' to see sections or provide specific keywords.\n";
            return;
        }

        std::string joined;
        for (std::size_t i = 0; i < sections.size(); ++i) {
            if (i != 0U) {
                joined += ' ';
            }
            joined += sections[i];
        }
        output += std::format("Querying WHATWG URL specification for: {}\n\n", joined);
        output += "```markdown\n";

        std::vector<std::string> queries;
        queries.reserve(sections.size());
        for (auto const& section : sections) {
            queries.emplace_back(section);
        }

        try {
            output += trim_context_nl(tools::whatwg_query(queries), head_lines);
        } catch (std::exception const& error) {
            r.err = std::format("whatwg-url-specs: {}\n", error.what());
        }
        output += "```\n\n";

        r.out = std::move(output);
    }

    static void render_help_whatwg_url_specs(std::ostream& os) {
        os << R"EOF(Usage: prompt whatwg-url-specs [--head N] [SECTIONS...]
       some-command | prompt whatwg-url-specs [--head N] [SECTIONS...]

Fetches relevant sections of the WHATWG URL specification and adds them to the prompt context.

Several sections can be requested in a single call:
       prompt whatwg-url-specs path-state authority-state host-state path-start-state

Options:
  --head N   Keep only the first N lines of the embedded context
)EOF";
    }

    // This file is the prompt: it implements it and registers it at static-init time
    // (no central list — see sdk/prompt_registry.hpp).
    [[maybe_unused]] bool const registered = dana::register_prompt(
      "whatwg-url-specs",
      "Fetches relevant sections of the WHATWG URL specification and adds them to the prompt context.",
      &execute_whatwg_url_specs,
      &render_help_whatwg_url_specs);

} // namespace dana
