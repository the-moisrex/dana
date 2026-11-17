#pragma once

// A small WebSocket client over libcurl's ws API (CURLOPT_CONNECT_ONLY=2 +
// curl_ws_send/curl_ws_recv).  The sites that moved their composer onto an
// application socket (Grok, GapGPT) drive one connection per conversation, so
// this stays deliberately thin: connect with the browser's headers, send a
// text frame, and read whole messages with a timeout.  libcurl handles the
// handshake, TLS, ping/pong and frame reassembly; `receive` only reassembles a
// fragmented *message* and skips control frames.

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dana::ai {

    struct [[nodiscard]] ws_error : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    struct [[nodiscard]] ws_message {
        std::string data;
        bool        text  = true;
        bool        close = false;
    };

    struct [[nodiscard]] websocket {
        websocket() = default;
        ~websocket();

        websocket(websocket&& other) noexcept;
        websocket& operator=(websocket&& other) noexcept;

        websocket(websocket const&)            = delete;
        websocket& operator=(websocket const&) = delete;

        // Open the socket: `url` must be ws:// or wss://.  `headers` reach the
        // handshake verbatim (User-Agent, Cookie, …) and a non-empty `origin`
        // becomes the Origin header.  Throws ws_error on any failure.
        void connect(std::string const&                                      url,
                     std::vector<std::pair<std::string, std::string>> const& headers,
                     std::string const&                                      origin,
                     long                                                    timeout_seconds);

        void send_text(std::string_view data);
        void send_ping();

        // Wait up to `wait_ms` for one complete message (negative waits forever).
        // Returns nullopt on timeout; a close frame comes back with `close` set.
        std::optional<ws_message> receive(int wait_ms);

        void close();

        [[nodiscard]] bool connected() const noexcept {
            return easy_ != nullptr;
        }

        void set_debug(bool const value) noexcept {
            debug_ = value;
        }

      private:
        void*       easy_   = nullptr; // CURL*
        void*       multi_  = nullptr; // CURLM*
        int         socket_ = -1;      // the raw socket behind the websocket
        std::string partial_;
        bool        partial_text_ = true;
        bool        debug_        = false;
    };

    // True when the linked libcurl was built with the ws/wss protocols.
    bool have_websocket();

} // namespace dana::ai
