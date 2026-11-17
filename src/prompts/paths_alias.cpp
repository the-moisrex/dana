#include "../sdk/alias.hpp"
#include "../sdk/errors.hpp"
#include "../sdk/facts.hpp"
#include "../sdk/prompt_registry.hpp"
#include "../sdk/stage.hpp"

#include <filesystem>
#include <ostream>
#include <string>
#include <string_view>

namespace dana {

    namespace {

        // `.paths.hide [PATH…]`: a silent transform sink. It rewrites the accumulated
        // context in place (so nothing is printed or copied) and publishes the mapping
        // for every later `.ai.*` sink.
        void execute_paths_hide(view const& v, result& r) {
            alias::table table = alias::for_environment(v.env);
            if (auto const prior = v.prior.get(keys::path_alias)) {
                if (auto const decoded = alias::decode(*prior)) {
                    for (auto const& item : decoded->entries()) {
                        table.add(item.real, item.shown);
                    }
                }
            }

            for (std::string_view const arg : v.args) {
                if (!arg.empty() && arg.front() == '-') {
                    throw usage_error{"prompt paths.hide: unknown option " + std::string(arg) + "\n"};
                }
                table.add_path(std::filesystem::path(arg));
            }

            r.out          = table.hide(v.text);
            r.silent       = true;
            r.no_clipboard = true;
            r.published.set(keys::path_alias, alias::encode(table));
        }

        // `.paths.reveal`: the inverse transform, for the end of a chain or before a
        // prompt that needs the real paths.
        void execute_paths_reveal(view const& v, result& r) {
            if (!v.args.empty()) {
                throw usage_error{"prompt paths.reveal: does not take arguments\n"};
            }

            auto const fact = v.prior.get(keys::path_alias);
            if (!fact) {
                throw soft_error{"prompt paths.reveal: no .paths.hide earlier in the chain\n"};
            }

            auto const table = alias::decode(*fact);
            if (!table) {
                throw soft_error{"prompt paths.reveal: the path mapping is corrupt\n"};
            }

            r.out          = table->reveal(v.text);
            r.silent       = true;
            r.no_clipboard = true;
        }

        void render_help_paths_hide(std::ostream& os) {
            os << R"EOF(Usage: prompt <other prompts...> .paths.hide [PATH...]

Replace the workspace's real absolute paths in the chain context with a stable,
non-existent placeholder directory (`/prompt-ws-<hash>`), and publish the mapping
so every later `.ai.*` sink hides what it sends and reveals the answer it gets
back. The placeholder is a hash of the normalized canonical path, so the same
project always gets the same one and a resumed `.ai.reply` chat stays coherent.

The working directory and the git root are mapped automatically; extra PATH
arguments are mapped too. Nothing is printed or copied: the context is rewritten
in place, so `.paths.hide` can sit anywhere before an AI sink.

Examples:
  prompt .note "fix src/main.cpp" .paths.hide .ai.dseek
  prompt .files src/a.cpp .paths.hide .ai.gap .paths.reveal
)EOF";
        }

        void render_help_paths_reveal(std::ostream& os) {
            os << R"EOF(Usage: prompt <other prompts...> .paths.reveal

Translate the placeholder workspace paths in the chain context back to the real
ones, using the mapping a `.paths.hide` earlier in the chain published. Nothing
is printed or copied: the context is rewritten in place.

Examples:
  prompt .note "review this" .paths.hide .ai.dseek .paths.reveal
)EOF";
        }

    } // namespace

    // This file registers two silent sinks: `.paths.hide` and `.paths.reveal`.
    bool const registered_paths_hide = dana::register_sink(
      "paths.hide",
      "Replace real workspace paths in the chain context with a stable placeholder directory.",
      &execute_paths_hide,
      &render_help_paths_hide,
      nullptr,
      false);

    bool const registered_paths_reveal = dana::register_sink(
      "paths.reveal",
      "Translate the placeholder workspace paths in the chain context back to real paths.",
      &execute_paths_reveal,
      &render_help_paths_reveal,
      nullptr,
      false);

} // namespace dana
