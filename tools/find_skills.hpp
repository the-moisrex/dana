#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace dana::tools {

    struct [[nodiscard]] find_skills_options {
        int  limit   = 10;
        int  scan    = 2; // security-scan the top K per registry (0 = skip)
        bool verbose = false;
    };

    // Search skills.sh, clawhub.ai and GitHub, enrich each result with a match
    // percentage and an installed flag, and return the same JSON document the
    // `--json` mode exposes.
    [[nodiscard]] nlohmann::json find_skills(std::string_view query, find_skills_options const& options = {});

} // namespace dana::tools
