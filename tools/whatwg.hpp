#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace dana::tools {

    struct [[nodiscard]] whatwg_options {
        bool algorithm_only = false;
        bool all            = false;
        bool html           = false;
        bool clean_cache    = false;
        bool verbose        = false;
    };

    // Query the cached WHATWG URL specification. Sections are matched by id or
    // title (exact first, then substring), rendered to Markdown through pandoc.
    [[nodiscard]] std::string whatwg_query(std::vector<std::string> const& queries, whatwg_options const& options = {});

    [[nodiscard]] std::string whatwg_help();

} // namespace dana::tools
