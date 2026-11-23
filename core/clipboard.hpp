#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace dana::clipboard {

    enum struct [[nodiscard]] backend : std::uint8_t {
        wayland,
        x11,
        kde,
        none
    };

    // The session's primary clipboard family, for diagnostics.
    backend detect_backend();

    // Detect the session backend and set the per-operation deadline. Calling it
    // again only updates the timeout. Zero or negative keeps the existing timeout (or defaults to 3s if uninitialized).
    void init(std::chrono::milliseconds timeout = std::chrono::milliseconds{0});

    // True when the session has a usable clipboard utility (wl-clipboard on
    // Wayland, xclip/xsel on X11, or Klipper on KDE).
    bool available();

    // Read the clipboard byte-for-byte. An empty selection is an empty string,
    // not an error; a failed backend also returns empty (see last_error()).
    std::string paste();

    // Write `data` byte-for-byte. Returns false when no backend accepted it.
    bool copy(std::string_view data);

    // Clear the clipboard. On KDE this goes through Klipper first, since it
    // would otherwise repopulate the selection from its history.
    bool clear();

    // Klipper-only clipboard history (one entry per line) and its reset.
    std::string history();
    bool        clear_history();

    // The most recent backend failure, for callers that want to report why an
    // operation failed. Empty after a successful operation.
    std::string const& last_error();

    // The instrument a pasted text identifies, or nullopt when it identifies
    // nothing: an easytrader/TSETMC/Codal URL, an ISIN, an insCode (15+ digits), or
    // a short symbol written without Latin letters. CR/LF are stripped first, so a
    // terminal copy works. Pure, so the policy is testable without a clipboard.
    std::optional<std::string> detect_identifier(std::string_view pasted);

} // namespace dana::clipboard
