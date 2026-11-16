#include "http.hpp"

#include "diagnostics.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <curl/curl.h>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>

#ifdef PROMPT_HAVE_CURL_IMPERSONATE
extern "C" CURLcode curl_easy_impersonate(CURL* curl, char const* target, int default_headers);
#endif

namespace dana::ai {

    namespace {

        std::once_flag g_curl_init;

        void ensure_global_init() {
            std::call_once(g_curl_init, [] {
                curl_global_init(CURL_GLOBAL_DEFAULT);
            });
        }

        std::string lower(std::string_view value) {
            std::string out(value);
            for (char& c : out) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return out;
        }

        long env_long(char const* name, long fallback) {
            char const* raw = std::getenv(name);
            if (raw == nullptr || *raw == '\0') {
                return fallback;
            }
            try {
                return std::stol(raw);
            } catch (std::exception const&) {
                return fallback;
            }
        }

        // The retry budget is resolved once per request from the environment, so the
        // defaults stay in one place and a caller can still pin its own values.
        struct retry_settings {
            int  attempts  = 3;
            long base_ms   = 500;
            long max_ms    = 8000;
            long budget_ms = 180'000;
        };

        retry_settings retry_for(http_request const& request) {
            retry_settings settings;
            settings.attempts =
              request.retries >= 0 ? request.retries : static_cast<int>(env_long("PROMPT_AI_RETRIES", 3));
            settings.base_ms =
              request.retry_base_ms >= 0 ? request.retry_base_ms : env_long("PROMPT_AI_RETRY_DELAY_MS", 500);
            settings.max_ms =
              request.retry_max_ms >= 0 ? request.retry_max_ms : env_long("PROMPT_AI_RETRY_MAX_DELAY_MS", 8000);
            settings.budget_ms = env_long("PROMPT_AI_RETRY_BUDGET_MS", 180'000);
            settings.attempts  = std::max(settings.attempts, 1);
            settings.base_ms   = std::max<long>(settings.base_ms, 1);
            settings.max_ms    = std::max(settings.max_ms, settings.base_ms);
            settings.budget_ms = std::max<long>(settings.budget_ms, 0);
            return settings;
        }

        long retry_after_ms(std::vector<std::pair<std::string, std::string>> const& headers) {
            for (auto const& [name, value] : headers) {
                if (lower(name) != "retry-after") {
                    continue;
                }
                long seconds            = 0;
                auto const [end, error] = std::from_chars(value.data(), value.data() + value.size(), seconds);
                // An HTTP-date Retry-After is rare here; fall back to backoff.
                if (error == std::errc{} && end == value.data() + value.size() && seconds >= 0) {
                    return seconds * 1000;
                }
            }
            return -1;
        }

        long jittered_delay(retry_settings const& settings, int attempt) {
            long delay = settings.base_ms;
            for (int i = 0; i < attempt && delay < settings.max_ms; ++i) {
                delay *= 2;
            }
            delay = std::min(delay, settings.max_ms);
            // ±20% jitter so a fleet of retries does not hammer the same edge.
            static thread_local std::mt19937    rng{std::random_device{}()};
            std::uniform_int_distribution<long> spread(-delay / 5, delay / 5);
            return std::max<long>(1, delay + spread(rng));
        }

        bool retryable_status(long status) {
            return status == 408 || status == 425 || status == 429 || status == 500 || status == 502 || status == 503 ||
                   status == 504;
        }

        std::string http_error_message(long status, std::string_view body) {
            std::string text(body);
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
                text.pop_back();
            }
            if (!text.empty()) {
                if (text.size() > 300) {
                    text.resize(300);
                }
                return "HTTP " + std::to_string(status) + ": " + text;
            }
            if (status == 413) {
                return "HTTP 413: request body too large";
            }
            return "HTTP " + std::to_string(status);
        }

        struct transfer_state {
            long                                             status = 0;
            std::string                                      header_buffer;
            std::string                                      body;
            std::string                                      line_buffer;
            std::vector<std::pair<std::string, std::string>> headers;
            line_callback                                    on_line;
            chunk_callback                                   on_chunk;
            bool                                             streaming = false;
            bool                                             debug     = false;
            std::string                                      error_body;
            // Bytes already handed to the caller. A retry is only safe while this is
            // still zero: once a stream has produced output, replaying it would
            // duplicate the answer.
            std::size_t delivered = 0;

            void reset() {
                status = 0;
                header_buffer.clear();
                body.clear();
                line_buffer.clear();
                headers.clear();
                error_body.clear();
                delivered = 0;
            }

            void header(char const* data, std::size_t size) {
                header_buffer.append(data, size);
                std::size_t start = 0;
                for (;;) {
                    auto const newline = header_buffer.find('\n', start);
                    if (newline == std::string::npos) {
                        break;
                    }
                    std::string line = header_buffer.substr(start, newline - start);
                    start            = newline + 1;
                    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
                        line.pop_back();
                    }
                    if (line.empty()) {
                        continue;
                    }
                    if (line.starts_with("HTTP/")) {
                        auto const space = line.find(' ');
                        if (space != std::string::npos) {
                            long        parsed = 0;
                            std::size_t index  = space + 1;
                            while (index < line.size() && std::isdigit(static_cast<unsigned char>(line[index])) != 0) {
                                parsed = (parsed * 10) + (line[index] - '0');
                                ++index;
                            }
                            if (parsed > 0) {
                                status = parsed;
                            }
                        }
                        continue;
                    }
                    auto const colon = line.find(':');
                    if (colon == std::string::npos) {
                        continue;
                    }
                    std::string name  = line.substr(0, colon);
                    std::string value = line.substr(colon + 1);
                    std::size_t first = value.find_first_not_of(" \t");
                    value             = first == std::string::npos ? std::string{} : value.substr(first);
                    headers.emplace_back(std::move(name), std::move(value));
                }
                if (start > 0) {
                    header_buffer.erase(0, start);
                }
            }

            void chunk(char const* data, std::size_t size) {
                if (status >= 400) {
                    if (error_body.size() < 64UL * 1024UL) {
                        error_body.append(data, size);
                    }
                    return;
                }
                if (!streaming) {
                    body.append(data, size);
                    return;
                }
                if (on_chunk) {
                    delivered += size;
                    on_chunk(std::string_view(data, size));
                    return;
                }
                line_buffer.append(data, size);
                std::size_t start = 0;
                for (;;) {
                    auto const newline = line_buffer.find('\n', start);
                    if (newline == std::string::npos) {
                        break;
                    }
                    std::string line = line_buffer.substr(start, newline - start);
                    start            = newline + 1;
                    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
                        line.pop_back();
                    }
                    if (debug) {
                        std::fprintf(stderr, "[net] %s\n", line.c_str());
                    }
                    if (on_line) {
                        ++delivered;
                        on_line(line);
                    }
                }
                if (start > 0) {
                    line_buffer.erase(0, start);
                }
            }

            void finish_lines() {
                if (!streaming || on_chunk || line_buffer.empty()) {
                    return;
                }
                std::string line = std::move(line_buffer);
                line_buffer.clear();
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (debug) {
                    std::fprintf(stderr, "[net] %s\n", line.c_str());
                }
                if (on_line) {
                    ++delivered;
                    on_line(line);
                }
            }
        };

        std::size_t write_callback(char* data, std::size_t size, std::size_t count, void* user) {
            auto* state = static_cast<transfer_state*>(user);
            state->chunk(data, size * count);
            return size * count;
        }

        std::size_t header_callback(char* data, std::size_t size, std::size_t count, void* user) {
            auto* state = static_cast<transfer_state*>(user);
            state->header(data, size * count);
            return size * count;
        }

        struct curl_slist_deleter {
            void operator()(curl_slist* list) const {
                curl_slist_free_all(list);
            }
        };

        using header_list = std::unique_ptr<curl_slist, curl_slist_deleter>;

        header_list build_headers(std::vector<std::pair<std::string, std::string>> const& headers) {
            curl_slist* list = nullptr;
            for (auto const& [name, value] : headers) {
                std::string line  = name;
                line             += ": ";
                line             += value;
                list              = curl_slist_append(list, line.c_str());
            }
            return header_list(list);
        }

        std::string impersonate_target(std::string const& requested) {
            // libcurl-impersonate only accepts versioned profile names; the Python
            // engine's generic "firefox" means "the newest bundled Firefox profile".
            if (requested == "firefox") {
                return "firefox147";
            }
            if (requested == "chrome") {
                return "chrome136";
            }
            if (requested == "safari") {
                return "safari180";
            }
            return requested;
        }

        // TCP keepalive plus a low-speed window: a VPN cut leaves the socket open but
        // dead, so keepalive probes (and the stall detector) turn it into a real error
        // instead of a hang until the whole-request timeout.
        void apply_resilience(CURL* easy, http_request const& request) {
            long const connect_timeout =
              request.connect_timeout_seconds > 0
                ? request.connect_timeout_seconds
                : env_long("PROMPT_AI_CONNECT_TIMEOUT", 15);
            curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, connect_timeout);
            curl_easy_setopt(easy, CURLOPT_TCP_KEEPALIVE, 1L);
            curl_easy_setopt(easy, CURLOPT_TCP_KEEPIDLE, env_long("PROMPT_AI_KEEPIDLE", 30));
            curl_easy_setopt(easy, CURLOPT_TCP_KEEPINTVL, env_long("PROMPT_AI_KEEPINTVL", 15));
#if LIBCURL_VERSION_NUM >= 0x07'2b00
            curl_easy_setopt(easy, CURLOPT_TCP_KEEPCNT, env_long("PROMPT_AI_KEEPCNT", 4));
#endif
            long const stall = request.stall_timeout_seconds > 0 ? request.stall_timeout_seconds
                                                                 : env_long("PROMPT_AI_STALL_TIMEOUT", 300);
            if (stall > 0) {
                curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
                curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, stall);
            }
        }

        http_response run_once(http_request const& request, transfer_state& state) {
            ensure_global_init();
            CURL* easy = curl_easy_init();
            if (easy == nullptr) {
                throw http_error(0, {}, "could not initialise libcurl");
            }

            struct guard {
                CURL* easy;

                ~guard() {
                    curl_easy_cleanup(easy);
                }
            } cleanup{easy};

#ifdef PROMPT_HAVE_CURL_IMPERSONATE
            bool const impersonating = !request.impersonate.empty();
            if (impersonating) {
                CURLcode const impersonate_code =
                  curl_easy_impersonate(easy, impersonate_target(request.impersonate).c_str(), 1);
                if (impersonate_code != CURLE_OK) {
                    warn(std::string("could not impersonate ") + request.impersonate + ": " +
                           curl_easy_strerror(impersonate_code),
                         "ai");
                }
            }
#else
            bool const impersonating = false;
            if (!request.impersonate.empty()) {
                static std::once_flag warned;
                std::call_once(warned, [] {
                    warn(
                      "this build links the system libcurl, not libcurl-impersonate; Cloudflare-protected sites may "
                      "reject the request");
                });
            }
#endif

            curl_easy_setopt(easy, CURLOPT_URL, request.url.c_str());
            curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 10L);
            // Enable response decompression with the same Accept-Encoding Firefox
            // advertises; the impersonation profile sets the header but not always the
            // decoder, so a gzip/br body would otherwise reach the line splitter raw.
            curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "gzip, deflate, br, zstd");
            if (!impersonating) {
                curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_2TLS));
            }
            curl_easy_setopt(easy, CURLOPT_TIMEOUT, request.timeout_seconds > 0 ? request.timeout_seconds : 600L);
            apply_resilience(easy, request);
            curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &write_callback);
            curl_easy_setopt(easy, CURLOPT_WRITEDATA, &state);
            curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, &header_callback);
            curl_easy_setopt(easy, CURLOPT_HEADERDATA, &state);

            std::string method = request.method;
            std::transform(method.begin(), method.end(), method.begin(), [](unsigned char c) {
                return static_cast<char>(std::toupper(c));
            });

            std::string                                      payload;
            std::vector<std::pair<std::string, std::string>> headers = request.headers;
            if (request.json_body) {
                payload = *request.json_body;
                if (std::ranges::none_of(headers,
                                         [](auto const& h) {
                                             return lower(h.first) == "content-type";
                                         }))
                {
                    headers.emplace_back("Content-Type", "application/json");
                }
            } else if (request.body) {
                payload = *request.body;
                if (!request.content_type.empty() &&
                    std::ranges::none_of(headers,
                                         [](auto const& h) {
                                             return lower(h.first) == "content-type";
                                         }))
                {
                    headers.emplace_back("Content-Type", request.content_type);
                }
            }

            auto list = build_headers(headers);
            if (list) {
                curl_easy_setopt(easy, CURLOPT_HTTPHEADER, list.get());
            }

            if (method == "GET" && payload.empty()) {
                curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L);
            } else if (method == "POST" && payload.empty() && !request.body && !request.json_body) {
                curl_easy_setopt(easy, CURLOPT_POST, 1L);
                curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, 0L);
            } else {
                if (method != "POST") {
                    curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, method.c_str());
                }
                curl_easy_setopt(easy, CURLOPT_POST, 1L);
                curl_easy_setopt(easy, CURLOPT_POSTFIELDS, payload.data());
                curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(payload.size()));
            }

            CURLcode const code = curl_easy_perform(easy);

            if (code != CURLE_OK) {
                // Do not flush the trailing partial line: a failed transfer's last
                // fragment is not a complete event and replaying it on a retry would
                // duplicate output.
                std::string message = curl_easy_strerror(code);
                if (state.status > 0) {
                    message += " (HTTP " + std::to_string(state.status) + ")";
                }
                throw http_error(state.status, state.error_body, message);
            }
            state.finish_lines();
            if (state.status >= 400) {
                std::string body = state.error_body;
                if (body.empty()) {
                    body = state.body;
                }
                throw http_error(state.status, body, http_error_message(state.status, body));
            }

            http_response response;
            response.status  = state.status;
            response.body    = std::move(state.body);
            response.headers = std::move(state.headers);
            return response;
        }

        // The delay before the next attempt, or -1 when the retry budget would be
        // exceeded (in which case the last failure is final).
        long next_delay(retry_settings const&                 settings,
                        transfer_state const&                 state,
                        int                                   attempt,
                        std::chrono::steady_clock::time_point started) {
            long delay = retry_after_ms(state.headers);
            if (delay < 0) {
                delay = jittered_delay(settings, attempt);
            }
            auto const elapsed =
              std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            if (settings.budget_ms > 0 && elapsed + delay > settings.budget_ms) {
                return -1;
            }
            return delay;
        }

        void note_retry(std::string_view reason, int attempt, retry_settings const& settings, long delay) {
            warn(std::string("network hiccup (") + std::string(reason) + "); retrying in " + std::to_string(delay) +
                   " ms (attempt " + std::to_string(attempt + 2) + "/" + std::to_string(settings.attempts) + ")",
                 "ai");
        }

        // Retry a whole request only while nothing has been handed to the caller: once
        // a stream delivered a line or a chunk, replaying it would duplicate output.
        http_response run_with_retry(http_request const& request, transfer_state& state) {
            retry_settings const settings = retry_for(request);
            auto const           started  = std::chrono::steady_clock::now();
            for (int attempt = 0;; ++attempt) {
                state.reset();
                try {
                    return run_once(request, state);
                } catch (http_error const& failure) {
                    bool const retryable = state.delivered == 0 &&
                                           (retryable_status(failure.status()) || is_network_failure(failure.what()));
                    if (!retryable || attempt + 1 >= settings.attempts) {
                        throw;
                    }
                    long const delay = next_delay(settings, state, attempt, started);
                    if (delay < 0) {
                        throw;
                    }
                    note_retry(failure.what(), attempt, settings, delay);
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                }
            }
        }

    } // namespace

    bool have_impersonate() {
#ifdef PROMPT_HAVE_CURL_IMPERSONATE
        return true;
#else
        return false;
#endif
    }

    http_response stream_lines(http_request const& request, line_callback const& on_line) {
        transfer_state state;
        state.streaming = true;
        state.on_line   = on_line;
        state.debug     = request.debug;
        return run_with_retry(request, state);
    }

    http_response stream_chunks(http_request const& request, chunk_callback const& on_chunk) {
        transfer_state state;
        state.streaming = true;
        state.on_chunk  = on_chunk;
        state.debug     = request.debug;
        return run_with_retry(request, state);
    }

    http_response perform(http_request const& request) {
        transfer_state state;
        return run_with_retry(request, state);
    }

    nlohmann::json request_json(http_request const& request) {
        auto const response = perform(request);
        if (response.body.empty()) {
            return {};
        }
        auto data = nlohmann::json::parse(response.body, nullptr, false);
        if (data.is_discarded()) {
            std::string preview = response.body.substr(0, 200);
            throw http_error(response.status, response.body, "expected JSON, got: " + preview);
        }
        return data;
    }

    std::string request_text(http_request const& request) {
        return perform(request).body;
    }

    namespace {

        std::pair<long, std::string> request_raw_once(http_request const& request, transfer_state& state) {
            ensure_global_init();
            CURL* easy = curl_easy_init();
            if (easy == nullptr) {
                throw http_error(0, {}, "could not initialise libcurl");
            }

            struct guard {
                CURL* easy;

                ~guard() {
                    curl_easy_cleanup(easy);
                }
            } cleanup{easy};

#ifdef PROMPT_HAVE_CURL_IMPERSONATE
            if (!request.impersonate.empty()) {
                curl_easy_impersonate(easy, impersonate_target(request.impersonate).c_str(), 1);
            }
#endif
            curl_easy_setopt(easy, CURLOPT_URL, request.url.c_str());
            curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(easy, CURLOPT_TIMEOUT, request.timeout_seconds > 0 ? request.timeout_seconds : 600L);
            apply_resilience(easy, request);
            curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &write_callback);
            curl_easy_setopt(easy, CURLOPT_WRITEDATA, &state);
            curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, &header_callback);
            curl_easy_setopt(easy, CURLOPT_HEADERDATA, &state);
            auto list = build_headers(request.headers);
            if (list) {
                curl_easy_setopt(easy, CURLOPT_HTTPHEADER, list.get());
            }

            std::string const payload = request.body.value_or(std::string{});
            if (request.method == "PUT" || request.method == "POST") {
                curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, request.method.c_str());
                curl_easy_setopt(easy, CURLOPT_POSTFIELDS, payload.data());
                curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(payload.size()));
            }
            CURLcode const code = curl_easy_perform(easy);
            if (code != CURLE_OK) {
                throw http_error(state.status, state.body, curl_easy_strerror(code));
            }
            return {state.status, state.body};
        }

    } // namespace

    std::pair<long, std::string> request_raw(http_request const& request) {
        retry_settings const settings = retry_for(request);
        auto const           started  = std::chrono::steady_clock::now();
        for (int attempt = 0;; ++attempt) {
            transfer_state state;
            try {
                auto result = request_raw_once(request, state);
                if (!retryable_status(result.first) || attempt + 1 >= settings.attempts) {
                    return result;
                }
                long const delay = next_delay(settings, state, attempt, started);
                if (delay < 0) {
                    return result;
                }
                note_retry("HTTP " + std::to_string(result.first), attempt, settings, delay);
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            } catch (http_error const& failure) {
                if (!is_network_failure(failure.what()) || attempt + 1 >= settings.attempts) {
                    throw;
                }
                long const delay = next_delay(settings, state, attempt, started);
                if (delay < 0) {
                    throw;
                }
                note_retry(failure.what(), attempt, settings, delay);
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            }
        }
    }

    bool is_network_failure(std::string_view message) {
        // Markers of a transport-level failure: DNS, connect, TLS, reset, stall.
        // These are what a VPN cut-off or a dropped Wi-Fi link looks like.
        static constexpr auto kMarkers = std::to_array<std::string_view>({
          "could not resolve",
          "couldn't resolve",
          "failed to connect",
          "connection refused",
          "connection reset",
          "connection aborted",
          "network is unreachable",
          "no route to host",
          "ssl connect",
          "ssl: ",
          "tls",
          "certificate",
          "timed out",
          "timeout",
          "operation too slow",
          "transfer closed",
          "partial file",
          "unexpected eof",
          "empty reply",
          "recv failure",
          "send failure",
          "broken pipe",
          "curl: (6)",
          "curl: (7)",
          "curl: (18)",
          "curl: (28)",
          "curl: (35)",
          "curl: (52)",
          "curl: (55)",
          "curl: (56)",
        });
        std::string const     text     = lower(message);
        return std::ranges::any_of(kMarkers, [&text](std::string_view marker) {
            return text.contains(marker);
        });
    }

    bool is_transient(std::string_view message) {
        static constexpr auto kMarkers = std::to_array<std::string_view>({
          "408",
          "425",
          "429",
          "500",
          "502",
          "503",
          "504",
          "too many requests",
          "temporarily unavailable",
          "service unavailable",
          "bad gateway",
          "gateway timeout",
        });
        if (is_network_failure(message)) {
            return true;
        }
        std::string const text = lower(message);
        return std::ranges::any_of(kMarkers, [&text](std::string_view marker) {
            return text.contains(marker);
        });
    }

    std::vector<std::string> sse_payloads(std::vector<std::string> const& lines) {
        std::vector<std::string> out;
        for (auto const& line : lines) {
            if (line.empty() || line.front() == ':') {
                continue;
            }
            if (line.starts_with("data:")) {
                std::string payload = line.substr(5);
                std::size_t first   = payload.find_first_not_of(" \t");
                out.push_back(first == std::string::npos ? std::string{} : payload.substr(first));
            }
        }
        return out;
    }

    std::string encode_multipart(std::vector<multipart_field> const& fields, std::string& content_type) {
        static thread_local std::mt19937_64 rng{std::random_device{}()};
        std::ostringstream                  boundary_stream;
        boundary_stream << std::hex << rng();
        std::string const boundary = boundary_stream.str();
        content_type               = "multipart/form-data; boundary=" + boundary;
        std::string out;
        for (auto const& field : fields) {
            out += "--" + boundary + "\r\n";
            if (!field.filename.empty()) {
                out += "Content-Disposition: form-data; name=\"" + field.name + "\"; filename=\"" + field.filename +
                       "\"\r\n";
                out += "Content-Type: " +
                       (field.content_type.empty() ? std::string("application/octet-stream") : field.content_type) +
                       "\r\n\r\n";
                out += field.value;
            } else {
                out += "Content-Disposition: form-data; name=\"" + field.name + "\"\r\n\r\n";
                out += field.value;
            }
            out += "\r\n";
        }
        out += "--" + boundary + "--\r\n";
        return out;
    }

} // namespace dana::ai
