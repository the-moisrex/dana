#pragma once

#include <string_view>

namespace prompt::ai {

    // Visible failure reporting.  Every operation either raises (a hard failure) or
    // reports through warn/note; a best-effort failure still shows up under
    // --debug / AI_DEBUG=1.

    bool debug_enabled();

    // A recoverable failure the user should know about.
    void warn(std::string_view message, std::string_view prefix = "ai");

    // A best-effort failure, reported only when debugging is on.
    void note(std::string_view message, bool debug, std::string_view prefix = "ai");
    void note(std::string_view message, std::string_view prefix = "ai");

    // An unrecoverable failure: report it and return the exit code.
    int failed(std::string_view message, std::string_view prefix = "ai", int code = 1);

} // namespace prompt::ai
