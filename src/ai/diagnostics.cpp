#include "diagnostics.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace prompt::ai {

    namespace {

        bool truthy(std::string_view value) {
            return value == "1" || value == "true" || value == "yes" || value == "on" || value == "TRUE" ||
                   value == "YES" || value == "ON";
        }

        void write_line(std::string_view message, std::string_view prefix) {
            std::cerr << '[' << prefix << "] " << message << '\n';
            std::cerr.flush();
        }

    } // namespace

    bool debug_enabled() {
        char const* raw = std::getenv("AI_DEBUG");
        return raw != nullptr && truthy(raw);
    }

    void warn(std::string_view message, std::string_view prefix) {
        write_line(message, prefix);
    }

    void note(std::string_view message, bool debug, std::string_view prefix) {
        if (debug) {
            write_line(message, prefix);
        }
    }

    void note(std::string_view message, std::string_view prefix) {
        note(message, debug_enabled(), prefix);
    }

    int failed(std::string_view message, std::string_view prefix, int code) {
        write_line(message, prefix);
        return code;
    }

} // namespace prompt::ai
