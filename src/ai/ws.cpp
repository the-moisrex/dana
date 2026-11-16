#include "ws.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <curl/curl.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace prompt::ai {

namespace {

std::once_flag g_curl_init;

void ensure_global_init() {
    std::call_once(g_curl_init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

long env_long(char const* name, long fallback) {
    char const* raw = std::getenv(name);
    if (raw == nullptr || *raw == '\0') return fallback;
    try {
        return std::stol(raw);
    } catch (std::exception const&) {
        return fallback;
    }
}

//: Send waits are bounded so a stalled edge cannot hang the whole turn.
constexpr int kSendTimeoutMs = 60'000;

struct curl_slist_deleter {
    void operator()(curl_slist* list) const { curl_slist_free_all(list); }
};

using header_list = std::unique_ptr<curl_slist, curl_slist_deleter>;

header_list build_headers(std::vector<std::pair<std::string, std::string>> const& headers) {
    curl_slist* list = nullptr;
    for (auto const& [name, value] : headers) {
        std::string line = name;
        line += ": ";
        line += value;
        list = curl_slist_append(list, line.c_str());
    }
    return header_list(list);
}

// The connect-only connection is not part of the multi handle's own poll set
// once the handshake transfer is done, so the socket is handed to
// curl_multi_poll as an extra descriptor.
CURLMcode poll_socket(CURLM* multi, int socket, int events, int timeout_ms, int& numfds) {
    if (socket < 0) return curl_multi_poll(multi, nullptr, 0, timeout_ms, &numfds);
    curl_waitfd fd{};
    fd.fd = static_cast<curl_socket_t>(socket);
    fd.events = static_cast<short>(events);
    fd.revents = 0;
    return curl_multi_poll(multi, &fd, 1, timeout_ms, &numfds);
}

} // namespace

bool have_websocket() {
    curl_version_info_data const* info = curl_version_info(CURLVERSION_NOW);
    if (info == nullptr || info->protocols == nullptr) return false;
    for (char const* const* protocol = info->protocols; *protocol != nullptr; ++protocol) {
        if (std::string_view(*protocol) == "ws") return true;
    }
    return false;
}

websocket::~websocket() { close(); }

websocket::websocket(websocket&& other) noexcept
    : easy_(other.easy_), multi_(other.multi_), socket_(other.socket_), partial_(std::move(other.partial_)),
      partial_text_(other.partial_text_), debug_(other.debug_) {
    other.easy_ = nullptr;
    other.multi_ = nullptr;
    other.socket_ = -1;
}

websocket& websocket::operator=(websocket&& other) noexcept {
    if (this == &other) return *this;
    close();
    easy_ = other.easy_;
    multi_ = other.multi_;
    socket_ = other.socket_;
    partial_ = std::move(other.partial_);
    partial_text_ = other.partial_text_;
    debug_ = other.debug_;
    other.easy_ = nullptr;
    other.multi_ = nullptr;
    other.socket_ = -1;
    return *this;
}

void websocket::connect(std::string const& url, std::vector<std::pair<std::string, std::string>> const& headers,
                        std::string const& origin, long timeout_seconds) {
    close();
    if (!have_websocket())
        throw ws_error("this libcurl has no WebSocket support (needs libcurl 7.86+ built with the ws protocol)");
    ensure_global_init();

    CURL* easy = curl_easy_init();
    if (easy == nullptr) throw ws_error("could not initialise libcurl for a websocket");

    // The handshake runs through a multi handle from the start: libcurl only
    // remembers the connect-only connection in the pool that drove it, and
    // curl_ws_send/recv (plus curl_multi_poll) look it up there.
    CURLM* multi = curl_multi_init();
    if (multi == nullptr) {
        curl_easy_cleanup(easy);
        throw ws_error("could not initialise libcurl's multi handle for a websocket");
    }
    auto cleanup = [&easy, &multi] {
        if (multi != nullptr) {
            curl_multi_remove_handle(multi, easy);
            curl_multi_cleanup(multi);
            multi = nullptr;
        }
        curl_easy_cleanup(easy);
    };

    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    // 2 = run the handshake and hand the raw connection back to us.
    curl_easy_setopt(easy, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1));
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, env_long("PROMPT_AI_CONNECT_TIMEOUT", 15));
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, timeout_seconds > 0 ? timeout_seconds : 600L);
    // A VPN cut leaves the socket open but dead; keepalive probes turn that
    // into a real error instead of a receive that waits out the whole timeout.
    curl_easy_setopt(easy, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(easy, CURLOPT_TCP_KEEPIDLE, env_long("PROMPT_AI_KEEPIDLE", 30));
    curl_easy_setopt(easy, CURLOPT_TCP_KEEPINTVL, env_long("PROMPT_AI_KEEPINTVL", 15));
#if LIBCURL_VERSION_NUM >= 0x072b00
    curl_easy_setopt(easy, CURLOPT_TCP_KEEPCNT, env_long("PROMPT_AI_KEEPCNT", 4));
#endif

    std::vector<std::pair<std::string, std::string>> all = headers;
    if (!origin.empty()) all.emplace_back("Origin", origin);
    auto list = build_headers(all);
    if (list) curl_easy_setopt(easy, CURLOPT_HTTPHEADER, list.get());

    if (curl_multi_add_handle(multi, easy) != CURLM_OK) {
        cleanup();
        throw ws_error("could not register the websocket with libcurl's multi handle");
    }

    auto const deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds > 0 ? timeout_seconds : 600);
    int running = 0;
    CURLMcode multi_code = curl_multi_perform(multi, &running);
    while (multi_code == CURLM_OK && running > 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            cleanup();
            throw ws_error("websocket handshake timed out");
        }
        int numfds = 0;
        multi_code = curl_multi_poll(multi, nullptr, 0, 1000, &numfds);
        if (multi_code != CURLM_OK) break;
        multi_code = curl_multi_perform(multi, &running);
    }
    if (multi_code != CURLM_OK) {
        std::string const message = std::string("websocket handshake failed: ") + curl_multi_strerror(multi_code);
        cleanup();
        throw ws_error(message);
    }

    CURLcode code = CURLE_OK;
    bool finished = false;
    int queued = 0;
    while (CURLMsg* message = curl_multi_info_read(multi, &queued)) {
        if (message->msg != CURLMSG_DONE) continue;
        code = message->data.result;
        finished = true;
    }
    if (!finished) {
        cleanup();
        throw ws_error("websocket handshake did not complete");
    }
    if (code != CURLE_OK) {
        std::string const message = std::string("websocket handshake failed: ") + curl_easy_strerror(code);
        cleanup();
        throw ws_error(message);
    }
    long status = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
    if (status != 101) {
        cleanup();
        throw ws_error("websocket handshake failed: HTTP " + std::to_string(status));
    }

    easy_ = easy;
    multi_ = multi;
    curl_socket_t socket = CURL_SOCKET_BAD;
    curl_easy_getinfo(easy, CURLINFO_ACTIVESOCKET, &socket);
    socket_ = static_cast<int>(socket);
    partial_.clear();
    partial_text_ = true;
}

void websocket::send_text(std::string_view data) {
    if (easy_ == nullptr) throw ws_error("websocket is not connected");
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kSendTimeoutMs);
    std::size_t offset = 0;
    for (;;) {
        std::size_t sent = 0;
        CURLcode const code = curl_ws_send(easy_, data.data() + offset, data.size() - offset, &sent, 0, CURLWS_TEXT);
        offset += sent;
        if (code == CURLE_OK && offset >= data.size()) return;
        if (code != CURLE_OK && code != CURLE_AGAIN)
            throw ws_error(std::string("websocket send failed: ") + curl_easy_strerror(code));
        int numfds = 0;
        CURLMcode const polled = poll_socket(multi_, socket_, CURL_WAIT_POLLOUT, 1000, numfds);
        if (polled != CURLM_OK) throw ws_error(std::string("websocket poll failed: ") + curl_multi_strerror(polled));
        if (std::chrono::steady_clock::now() >= deadline) throw ws_error("websocket send timed out");
    }
}

void websocket::send_ping() {
    if (easy_ == nullptr) throw ws_error("websocket is not connected");
    std::size_t sent = 0;
    CURLcode const code = curl_ws_send(easy_, "", 0, &sent, 0, CURLWS_PING);
    if (code != CURLE_OK && code != CURLE_AGAIN)
        throw ws_error(std::string("websocket ping failed: ") + curl_easy_strerror(code));
}

std::optional<ws_message> websocket::receive(int wait_ms) {
    if (easy_ == nullptr) throw ws_error("websocket is not connected");
    bool const infinite = wait_ms < 0;
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(infinite ? 0 : wait_ms);

    for (;;) {
        // Drain whatever libcurl already has buffered before waiting again.
        for (;;) {
            std::array<char, 16384> buffer{};
            std::size_t got = 0;
            curl_ws_frame const* meta = nullptr;
            CURLcode const code = curl_ws_recv(easy_, buffer.data(), buffer.size(), &got, &meta);
            if (code == CURLE_AGAIN) break;
            if (code != CURLE_OK) throw ws_error(std::string("websocket receive failed: ") + curl_easy_strerror(code));
            if (meta == nullptr) break;

            auto const flags = static_cast<unsigned>(meta->flags);
            if ((flags & CURLWS_CLOSE) != 0) {
                partial_.clear();
                return ws_message{.data = {}, .text = true, .close = true};
            }
            // libcurl answers pings itself; pongs are not part of a message.
            if ((flags & CURLWS_PING) != 0 || (flags & CURLWS_PONG) != 0) continue;

            partial_.append(buffer.data(), got);
            partial_text_ = (flags & CURLWS_BINARY) == 0;
            if (meta->bytesleft > 0) continue;        // more of this frame
            if ((flags & CURLWS_CONT) != 0) continue; // more fragments of this message

            ws_message message;
            message.data = std::move(partial_);
            message.text = partial_text_;
            partial_.clear();
            if (debug_) std::fprintf(stderr, "[ws] %s\n", message.data.c_str());
            return message;
        }

        if (!infinite && std::chrono::steady_clock::now() >= deadline) return std::nullopt;
        int remaining = 1000;
        if (!infinite) {
            auto const left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
                    .count();
            remaining = static_cast<int>(std::max<long long>(0, left));
        }
        int numfds = 0;
        CURLMcode const polled = poll_socket(multi_, socket_, CURL_WAIT_POLLIN, remaining, numfds);
        if (polled != CURLM_OK) throw ws_error(std::string("websocket poll failed: ") + curl_multi_strerror(polled));
        if (numfds == 0) {
            if (!infinite) return std::nullopt;
            // A spurious wake-up must not spin the CPU.
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

void websocket::close() {
    if (multi_ != nullptr && easy_ != nullptr)
        curl_multi_remove_handle(static_cast<CURLM*>(multi_), static_cast<CURL*>(easy_));
    if (multi_ != nullptr) curl_multi_cleanup(static_cast<CURLM*>(multi_));
    if (easy_ != nullptr) curl_easy_cleanup(static_cast<CURL*>(easy_));
    easy_ = nullptr;
    multi_ = nullptr;
    socket_ = -1;
    partial_.clear();
}

} // namespace prompt::ai
