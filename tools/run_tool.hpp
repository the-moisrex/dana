#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dana::tools {

    struct [[nodiscard]] run_options {
        std::optional<std::filesystem::path> build_dir;
        bool                                 clean   = true;
        bool                                 oneline = false;
    };

    struct [[nodiscard]] run_result {
        std::string output; // build and run output, both streams merged
        int         exit_code = 0;
    };

    // Build a CMake/Ninja/Make target and run the resulting executable. The first
    // argument is the target; the rest are passed to the program.
    [[nodiscard]] run_result run_target(std::vector<std::string> const& args, run_options const& options = {});

    // The build targets known to the discovered build directory.
    [[nodiscard]] std::vector<std::string> list_targets(std::optional<std::filesystem::path> const& build_dir = {});

} // namespace dana::tools
