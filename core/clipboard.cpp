#include "clipboard.hpp"

#include "process.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace dana::clipboard {

    static constexpr std::chrono::milliseconds default_timeout{3000};

    namespace {

        using namespace std::chrono_literals;

        enum struct family : std::uint8_t {
            wayland,
            x11,
        };

        struct session_config {
            family                    session{family::x11};
            bool                      kde{false};
            std::chrono::milliseconds timeout{3s};
            bool                      initialized{false};
        };

        session_config config;
        std::string    error_text;

        bool env_set(char const* name) {
            char const* value = std::getenv(name);
            return value != nullptr && *value != '\0';
        }

        bool env_equals(char const* name, std::string_view expected) {
            char const* value = std::getenv(name);
            return value != nullptr && expected == value;
        }

        family detect_family() {
            // An actual display socket wins over XDG_SESSION_TYPE: a test or a
            // nested session may advertise one type while only the other is
            // reachable, and the clipboard family must follow the socket.
            if (env_set("WAYLAND_DISPLAY")) {
                return family::wayland;
            }
            if (env_set("DISPLAY")) {
                return family::x11;
            }
            return env_equals("XDG_SESSION_TYPE", "wayland") ? family::wayland : family::x11;
        }

        void ensure_init() {
            if (!config.initialized) {
                init();
            }
        }

        struct run_result {
            std::string out;
            std::string err;
            int         exit_code{0};
            bool        timed_out{false};
        };

        // Runs a session utility directly (no shell). `discard_output` is for the
        // tools that fork a clipboard owner: a captured pipe would stay open in
        // the daemon and make the parent wait for it.
        run_result run(std::vector<std::string> argv, std::string_view stdin_data = {}, bool discard_output = false) {
            process::command cmd{std::move(argv)};
            cmd.stdin_data(std::string(stdin_data));
            cmd.timeout(config.timeout);
            if (discard_output) {
                cmd.discard_stdout();
                cmd.discard_stderr();
            } else {
                cmd.capture_stderr();
            }

            auto result = cmd.run();
            return {
              .out       = std::move(result.stdout_data),
              .err       = std::move(result.stderr_data),
              .exit_code = result.exit_code,
              .timed_out = result.timed_out,
            };
        }

        std::string failure(std::string_view tool, run_result const& result) {
            if (result.timed_out) {
                return std::format("{} timed out after {}ms", tool, config.timeout.count());
            }
            if (!result.err.empty()) {
                auto message = result.err;
                while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
                    message.pop_back();
                }
                return std::format("{} failed (exit {}): {}", tool, result.exit_code, message);
            }
            return std::format("{} failed (exit {})", tool, result.exit_code);
        }

        std::optional<std::string> qdbus_tool() {
            for (std::string_view const name : {"qdbus6", "qdbus", "qdbus-qt5"}) {
                if (process::tool_exists(name)) {
                    return std::string(name);
                }
            }
            return std::nullopt;
        }

        bool is_empty_selection(std::string_view const tool, run_result const& result) {
            if (tool == "wl-paste") {
                return result.exit_code == 1 && result.err.contains("Nothing is copied");
            }
            if (tool == "xclip") {
                return result.err.contains("not available");
            }
            if (tool == "xsel") {
                return result.exit_code != 0 || result.out.empty();
            }
            return false;
        }

        bool klipper_get(std::string& out) {
            auto const qdbus = qdbus_tool();
            if (!qdbus) {
                return false;
            }

            auto result = run({*qdbus, "org.kde.klipper", "/klipper", "org.kde.klipper.klipper.getClipboardContents"});
            if (result.exit_code != 0) {
                error_text = failure(*qdbus, result);
                return false;
            }

            out = std::move(result.out);
            // qdbus appends exactly one newline to a string reply; strip it.
            if (!out.empty() && out.back() == '\n') {
                out.pop_back();
            }
            return true;
        }

        bool klipper_set(std::string_view data) {
            auto const qdbus = qdbus_tool();
            if (!qdbus) {
                return false;
            }

            auto const result = run({
              *qdbus,
              "org.kde.klipper",
              "/klipper",
              "org.kde.klipper.klipper.setClipboardContents",
              std::string(data),
            });
            if (result.exit_code != 0) {
                error_text = failure(*qdbus, result);
                return false;
            }
            return true;
        }

        bool klipper_call(char const* method) {
            auto const qdbus = qdbus_tool();
            if (!qdbus) {
                return false;
            }

            auto const result = run({*qdbus, "org.kde.klipper", "/klipper", method});
            if (result.exit_code != 0) {
                error_text = failure(*qdbus, result);
                return false;
            }
            return true;
        }

    } // namespace

    backend detect_backend() {
        if (env_set("KDE_SESSION_VERSION")) {
            return backend::kde;
        }
        if (env_set("WAYLAND_DISPLAY")) {
            return backend::wayland;
        }
        if (env_set("DISPLAY")) {
            return backend::x11;
        }
        return backend::none;
    }

    void init(std::chrono::milliseconds const timeout) {
        config.timeout = timeout.count() > 0 ? timeout : default_timeout;
        if (config.initialized) {
            return;
        }

        config.session     = detect_family();
        config.kde         = env_set("KDE_SESSION_VERSION");
        config.initialized = true;
    }

    bool available() {
        ensure_init();

        if (config.kde && qdbus_tool()) {
            return true;
        }
        if (config.session == family::wayland) {
            return process::tool_exists("wl-paste") || process::tool_exists("wl-copy");
        }
        return process::tool_exists("xclip") || process::tool_exists("xsel");
    }

    std::string paste() {
        ensure_init();
        error_text.clear();

        if (config.session == family::wayland) {
            if (process::tool_exists("wl-paste")) {
                auto result = run({"wl-paste", "--no-newline"});
                if (result.exit_code == 0) {
                    return std::move(result.out);
                }
                if (is_empty_selection("wl-paste", result)) {
                    return {};
                }
                error_text = failure("wl-paste", result);
            } else {
                error_text = "no wayland clipboard utility found (install wl-clipboard)";
            }
        } else {
            if (process::tool_exists("xclip")) {
                auto result = run({"xclip", "-selection", "clipboard", "-o"});
                if (result.exit_code == 0) {
                    error_text.clear();
                    return std::move(result.out);
                }
                if (is_empty_selection("xclip", result)) {
                    error_text.clear();
                    return {};
                }
                error_text = failure("xclip", result);
            }
            if (process::tool_exists("xsel")) {
                auto result = run({"xsel", "--clipboard", "--output"});
                if (result.exit_code == 0) {
                    error_text.clear();
                    return std::move(result.out);
                }
                if (is_empty_selection("xsel", result)) {
                    error_text.clear();
                    return {};
                }
                error_text = failure("xsel", result);
            }
            if (error_text.empty()) {
                error_text = "no x11 clipboard utility found (install xclip or xsel)";
            }
        }

        // Klipper is a fallback on either family; its "current" item is history
        // entry 0, which is why it is not the primary paste path.
        if (config.kde) {
            std::string out;
            if (klipper_get(out)) {
                error_text.clear();
                return out;
            }
        }
        return {};
    }

    bool copy(std::string_view const data) {
        ensure_init();
        error_text.clear();

        if (config.session == family::wayland) {
            if (process::tool_exists("wl-copy")) {
                // wl-copy occasionally loses the race with a compositor that is
                // still starting; retry a couple of times unless it timed out.
                for (int attempt = 0; attempt < 3; ++attempt) {
                    auto result = run({"wl-copy", "--type", "text/plain"}, data, true);
                    if (result.exit_code == 0) {
                        error_text.clear();
                        return true;
                    }
                    error_text = failure("wl-copy", result);
                    if (result.timed_out) {
                        break;
                    }
                    if (attempt < 2) {
                        std::this_thread::sleep_for(200ms);
                    }
                }
            } else {
                error_text = "no wayland clipboard utility found (install wl-clipboard)";
            }
        } else {
            if (process::tool_exists("xclip")) {
                auto result = run({"xclip", "-selection", "clipboard", "-i"}, data, true);
                if (result.exit_code == 0) {
                    error_text.clear();
                    return true;
                }
                error_text = failure("xclip", result);
            }
            if (process::tool_exists("xsel")) {
                auto result = run({"xsel", "--clipboard", "--input"}, data, true);
                if (result.exit_code == 0) {
                    error_text.clear();
                    return true;
                }
                error_text = failure("xsel", result);
            }
            if (error_text.empty()) {
                error_text = "no x11 clipboard utility found (install xclip or xsel)";
            }
        }

        if (config.kde && klipper_set(data)) {
            error_text.clear();
            return true;
        }
        return false;
    }

    bool clear() {
        ensure_init();
        error_text.clear();

        // Klipper first: it repopulates the selection from history right after a
        // plain X11/Wayland clear.
        if (config.kde && klipper_call("org.kde.klipper.klipper.clearClipboardContents")) {
            error_text.clear();
            return true;
        }

        if (config.session == family::wayland) {
            if (process::tool_exists("wl-copy")) {
                auto result = run({"wl-copy", "--clear"}, {}, true);
                if (result.exit_code == 0) {
                    error_text.clear();
                    return true;
                }
                error_text = failure("wl-copy --clear", result);
            } else {
                error_text = "no wayland clipboard utility found (install wl-clipboard)";
            }
        } else {
            if (process::tool_exists("xclip")) {
                auto const result = run({"xclip", "-selection", "clipboard", "-i"}, {}, true);
                if (result.exit_code == 0) {
                    error_text.clear();
                    return true;
                }
                error_text = failure("xclip", result);
            }
            if (process::tool_exists("xsel")) {
                auto const result = run({"xsel", "--clipboard", "--delete"}, {}, true);
                if (result.exit_code == 0) {
                    error_text.clear();
                    return true;
                }
                error_text = failure("xsel", result);
            }
            if (error_text.empty()) {
                error_text = "no x11 clipboard utility found (install xclip or xsel)";
            }
        }
        return false;
    }

    std::string history() {
        ensure_init();
        error_text.clear();

        if (!config.kde) {
            error_text = "clipboard history is only supported with klipper";
            return {};
        }

        auto const qdbus = qdbus_tool();
        if (!qdbus) {
            error_text = "klipper history needs qdbus6/qdbus";
            return {};
        }

        auto result = run({*qdbus, "org.kde.klipper", "/klipper", "org.kde.klipper.klipper.getClipboardHistoryMenu"});
        if (result.exit_code != 0) {
            error_text = failure(*qdbus, result);
            return {};
        }
        error_text.clear();
        return std::move(result.out);
    }

    bool clear_history() {
        ensure_init();
        error_text.clear();

        if (!config.kde) {
            error_text = "clipboard history is only supported with klipper";
            return false;
        }
        return klipper_call("org.kde.klipper.klipper.clearClipboardHistory");
    }

    std::string const& last_error() {
        return error_text;
    }

    std::optional<std::string> detect_identifier(std::string_view pasted) {
        std::string text(pasted);
        text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
        text.erase(std::remove(text.begin(), text.end(), '\n'), text.end());
        if (text.empty()) {
            return std::nullopt;
        }

        // Order matters: the URL forms are more specific than the bare ISIN and
        // insCode tokens they contain.
        static std::regex const easytrader_re(
          R"(https?://d\.easytrader\.ir/(easy-chart|stock-details)/[A-Za-z0-9]{12})");
        static std::regex const tsetmc_re(R"(https?://(www\.)?tsetmc\.com/instInfo/[0-9]{15,})");
        static std::regex const codal_re(
          R"(https?://(www\.)?codal\.ir/ReportList\.aspx\?([^&]*&)?Symbol=[^&[:space:]]+)");
        static std::regex const isin_re(R"(IR[A-Za-z0-9]{10})");
        static std::regex const inscode_re(R"([0-9]{15,})");

        std::smatch match;
        if (std::regex_search(text, match, easytrader_re) || std::regex_search(text, match, tsetmc_re) ||
            std::regex_search(text, match, codal_re) || std::regex_search(text, match, isin_re) ||
            std::regex_search(text, match, inscode_re))
        {
            return std::string(match[0]);
        }

        // A copied Persian symbol ("فولاد") is an identifier; a sentence is not,
        // so cap the length and reject anything containing a Latin letter or digit.
        if (text.size() <= 64 &&
            std::ranges::none_of(text,
                                 [](char c) {
                                     return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
                                 }))
        {
            return text;
        }

        return std::nullopt;
    }

} // namespace dana::clipboard
