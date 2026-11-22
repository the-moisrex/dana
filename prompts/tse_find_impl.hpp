#pragma once

#include "../runtime/errors.hpp"
#include "../runtime/stage.hpp"

namespace dana {

    // The chat/agentic command generator, the .tse snapshot review and the piped
    // tse.find output review shown by `prompt tse.find`.
    void execute_tse_find_impl(view const& v, result& r);
    void render_help_tse_find_impl(std::ostream& os);

} // namespace dana
