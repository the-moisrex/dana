#pragma once

// HTTP transport for the providers: streaming lines/chunks, JSON requests and
// multipart uploads.  Uses libcurl-impersonate when available so the TLS
// fingerprint matches the browser session the sites expect; falls back to the
// system libcurl otherwise.

#include "../core/strings.hpp"

#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dana::ai {

    struct [[nodiscard]] http_error : std::runtime_error {
        http_error(long status, std::string body, std::string_view message)
          : std::runtime_error(std::string(message)),
            status_(status),
            body_(std::move(body)) {}

        [[nodiscard]] long status() const {
            return status_;
        }

        [[nodiscard]] std::string_view body() const {
            return body_;
        }

      private:
        long        status_ = 0;
        std::string body_;
    };

    struct [[nodiscard]] http_request {
        std::string                                      url;
        std::string                                      method = "GET";
        std::vector<std::pair<std::string, std::string>> headers;
        std::optional<std::string>                       json_body;
        std::optional<std::string>                       body;
        std::string                                      content_type;
        long                                             timeout_seconds = 600;
        // Resilience knobs. A value of 0 (timeouts) or -1 (retries) means "use the
        // environment/default", so a caller can override per request.
        long        connect_timeout_seconds                              = 0; // default 15
        long        stall_timeout_seconds                                = 0; // default 300 (1 byte/s low-speed window)
        int         retries                                              = -1; // default PROMPT_AI_RETRIES or 3
        long        retry_base_ms = -1; // default PROMPT_AI_RETRY_DELAY_MS or 500
        long        retry_max_ms  = -1; // default PROMPT_AI_RETRY_MAX_DELAY_MS or 8000
        std::string impersonate   = "firefox";
        bool        debug         = false;
    };

    struct [[nodiscard]] http_response {
        long                                             status = 0;
        std::string                                      body;
        std::vector<std::pair<std::string, std::string>> headers;
    };

    using line_callback  = std::function<void(std::string_view)>;
    using chunk_callback = std::function<void(std::string_view)>;

    // Streaming requests.  A status >= 400 is read to completion and thrown as
    // http_error (the body carries the site's own reason).
    http_response stream_lines(http_request const& request, line_callback const& on_line);
    http_response stream_chunks(http_request const& request, chunk_callback const& on_chunk);

    // Non-streaming requests.
    http_response  perform(http_request const& request);
    nlohmann::json request_json(http_request const& request);
    std::string    request_text(http_request const& request);

    // Send a raw body and return (status, body) without decoding; errors are not
    // thrown (pre-signed blob uploads need the status).
    std::pair<long, std::string> request_raw(http_request const& request);

    // Whether an error looks like a rate limit, timeout or flaky edge.
    bool is_transient(std::string_view message);

    // Whether an error looks like a transport-level failure (DNS, connect, reset,
    // TLS, stalled transfer) rather than the site rejecting the request.  Used to
    // decide whether retrying the whole turn is worth it.
    bool is_network_failure(std::string_view message);

    // The payload of each SSE `data:` line (ignoring comments/blanks).
    std::vector<std::string> sse_payloads(text::string_views lines);

    // Build a multipart/form-data body.  A field whose value has `filename` set is
    // a file part.
    struct [[nodiscard]] multipart_field {
        std::string name;
        std::string value;
        std::string filename;
        std::string content_type;
    };

    std::string encode_multipart(std::vector<multipart_field> const& fields, std::string& content_type);

    // True when the linked libcurl is the impersonating build.
    bool have_impersonate();

} // namespace dana::ai
