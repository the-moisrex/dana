#include "../tools/spp.hpp"

#include "../runtime/args.hpp"
#include "../runtime/embed.hpp"
#include "../runtime/prompt_registry.hpp"

#include <format>
#include <string>

namespace dana {

    static void execute_spp(view const& v, result& r) {
        auto head_lines = head_lines_or_throw(v);

        if (v.args.empty()) {
            throw usage_error{"Usage: prompt spp [--head N] <symbol> [symbol...]\n"};
        }

        std::string output;
        output += "Additional C++ symbol context:\n\n";

        std::string error;
        for (auto const& arg : positional_args(v)) {
            std::string symbol(arg);
            output += std::format("Symbol: {}\n\n", symbol);
            output += "```cpp\n";
            output += trim_context_nl(tools::expand_symbol(symbol), head_lines);
            output += "\n```\n\n";
        }

        output += "Use the symbol context above when analyzing the issue.";
        r.out   = std::move(output);
        r.err   = std::move(error);
    }

    static void render_help_spp(std::ostream& os) {
        os << R"EOF(Usage: prompt spp [--head N] <symbol> [symbol...]

Builds a C++ debugging prompt and expands the given symbols through `spp`.

Options:
  --head N   Keep only the first N lines of each symbol expansion
)EOF";
    }

    // This file is the prompt: it implements it and registers it at static-init time
    // (no central list — see sdk/prompt_registry.hpp).
    [[maybe_unused]] bool const registered = dana::register_prompt(
      "spp",
      "Builds a C++ debugging prompt and expands the given symbols through `spp`.",
      &execute_spp,
      &render_help_spp);

} // namespace dana
