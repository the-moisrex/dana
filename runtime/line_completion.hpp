#pragma once

#include "../core/strings.hpp"

#include <filesystem>
#include <string_view>

namespace dana {

    // Whole-line shell completion, the entry point `shell/completions.fish` calls:
    //
    //     prompt --complete-line <CURRENT> [PREV…]
    //
    // `current` is the token being completed (possibly empty) and `prior` the
    // tokens already typed after `prompt`. The dispatcher classifies the position
    // itself — the head, the start of the next `.name`/`-name` step, or an argument
    // of the open prompt — and prints `value<TAB>description` lines, so the shell
    // script only has to forward the command line. Answered before the clipboard is
    // initialized and before stdin is read: completion never runs a prompt.
    void complete_line(std::string_view             current,
                       text::string_views           prior,
                       std::filesystem::path const& git_root,
                       std::ostream&                out);

} // namespace dana
