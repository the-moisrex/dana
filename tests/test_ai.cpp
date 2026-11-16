// The native AI engine's foundations: prompt-file policy, state records, key
// masking, attachments, the HTTP/SSE transport and the provider registry.

#include "ai/files.hpp"
#include "ai/history.hpp"
#include "ai/http.hpp"
#include "ai/keys.hpp"
#include "ai/promptfile.hpp"
#include "ai/providers/openai_compat.hpp"
#include "ai/providers/registry.hpp"
#include "ai/render.hpp"
#include "ai/state.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

    int failures = 0;

    void check(bool condition, std::string_view what) {
        if (!condition) {
            std::cerr << "FAIL: " << what << '\n';
            ++failures;
        }
    }

    void check_equal(std::string const& actual, std::string const& expected, std::string_view what) {
        if (actual != expected) {
            std::cerr << "FAIL: " << what << "\n  expected: " << expected << "\n  actual:   " << actual << '\n';
            ++failures;
        }
    }

    std::filesystem::path temp_dir(std::string_view name) {
        auto            path = std::filesystem::temp_directory_path() /
                               ("prompt-ai-test-" + std::string(name) + "-" + std::to_string(::getpid()));
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
        return path;
    }

    void test_promptfile() {
        using namespace prompt::ai;
        check_equal(human_size(1), "1 byte", "human_size singular");
        check_equal(human_size(512), "512 bytes", "human_size bytes");
        check_equal(human_size(2048), "2.0 KiB", "human_size KiB");
        check(limit_for(100, std::nullopt) == 100, "limit_for default");
        check(limit_for(100, 5) == 5, "limit_for override");
        check(should_attach("hello world", 10, std::nullopt, std::nullopt), "should_attach above budget");
        check(!should_attach("hi", 10, std::nullopt, std::nullopt), "should_attach below budget");
        check(should_attach("hi", 10, true, std::nullopt), "should_attach forced");
        check(!should_attach("a very long prompt", 10, false, std::nullopt), "should_attach disabled");
        check(!should_attach("", 10, true, std::nullopt), "should_attach empty");

        auto const    path = write_prompt_file("hello\nworld");
        std::ifstream stream(path);
        std::string   text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        check_equal(text, "hello\nworld\n", "write_prompt_file adds a newline");
        check(prompt_file_reference(path, 11).find(path.filename().string()) != std::string::npos,
              "reference names the file");
        remove_prompt_file(path);
        check(!std::filesystem::exists(path), "remove_prompt_file");

        check(looks_too_large("HTTP 413: request entity too large"), "looks_too_large 413");
        check(!looks_too_large("connection reset"), "looks_too_large unrelated");
        auto const annotated = annotate_error(
          "HTTP 413: too large",
          "openrouter",
          "OpenRouter",
          "prompt",
          false,
          false,
          false,
          false,
          std::nullopt);
        check(annotated.find("no upload endpoint") != std::string::npos, "annotate_error no-upload remedy");
    }

    void test_history() {
        using namespace prompt::ai::history;
        check_equal(iso_from_epoch(0), "", "epoch zero");
        check_equal(iso_from_epoch(0), "", "epoch zero again");
        check_equal(iso_from_epoch(1'700'000'000), "2023-11-14T22:13:20Z", "iso seconds");
        check_equal(iso_from_epoch(1700000000000.0), "2023-11-14T22:13:20Z", "iso milliseconds");
        check(epoch_of("2023-11-14T22:13:20Z") == 1700000000.0, "epoch round trip");
        check(epoch_of("nonsense") == 0, "epoch nonsense");
    }

    void test_state() {
        using namespace prompt::ai;
        auto const dir = temp_dir("state");
        ::setenv("XDG_STATE_HOME", dir.string().c_str(), 1);

        state::conversation_record record;
        record.conversation_id = "conv-1";
        record.model           = "model-a";
        record.history         = nlohmann::json::array({
          {{"role", "user"}, {"text", "hi"}}
        });
        record.resume          = {
          {"cursor", 7}
        };
        state::save("openrouter", record, "scope-test");

        auto const loaded = state::load("openrouter");
        check(loaded.has_value(), "state load");
        check_equal(loaded->conversation_id, "conv-1", "state conversation id");
        check_equal(loaded->model, "model-a", "state model");
        check(loaded->history.is_array() && loaded->history.size() == 1, "state history");

        auto const terminal = state::load_terminal("openrouter", "scope-test");
        check(terminal.has_value(), "state terminal mirror");
        check_equal(terminal->conversation_id, "conv-1", "state terminal id");

        state::clear("openrouter", "scope-test");
        check(!state::load("openrouter").has_value(), "state clear global");
        check(!state::load_terminal("openrouter", "scope-test").has_value(), "state clear terminal");

        std::map<std::string, state::conversation_record> participants;
        participants["openai"] = record;
        state::save_debate({"openai", "openrouter"}, participants, "scope-test");
        auto const debate = state::load_debate({"openrouter", "openai"});
        check(debate.has_value() && debate->size() == 1, "debate state round trip");
        state::clear_debate({"openai", "openrouter"}, "scope-test");
        check(!state::load_debate({"openai", "openrouter"}).has_value(), "debate clear");

        std::filesystem::remove_all(dir);
        ::unsetenv("XDG_STATE_HOME");
    }

    void test_files() {
        using namespace prompt::ai;
        check_equal(base64_encode("hello"), "aGVsbG8=", "base64 hello");
        check_equal(base64_encode("a"), "YQ==", "base64 one");
        check_equal(base64_encode("ab"), "YWI=", "base64 two");
        check_equal(guess_mime("x.json"), "application/json", "mime json");
        check_equal(guess_mime("x.png"), "image/png", "mime png");

        auto const dir  = temp_dir("files");
        auto const path = dir / "note.txt";
        {
            std::ofstream stream(path);
            stream << "contents";
        }
        auto const attachment = read_attachment(path.string());
        check_equal(attachment.name, "note.txt", "attachment name");
        check_equal(attachment.data, "contents", "attachment data");
        check_equal(attachment.mime, "text/plain", "attachment mime");
        std::filesystem::remove_all(dir);
    }

    void test_keys() {
        using namespace prompt::ai;
        check_equal(keys::mask(""), "", "mask empty");
        check_equal(keys::mask("short"), "*****", "mask short");
        check(keys::mask("sk-or-v1-abcdefghijklmnop").find("sk-or-") == 0, "mask prefix");
        check(keys::mask("sk-or-v1-abcdefghijklmnop").find("mnop") != std::string::npos, "mask suffix");
        check(keys::mask("sk-or-v1-abcdefghijklmnop").find("abcdefghijkl") == std::string::npos, "mask hides middle");
    }

    void test_transport_helpers() {
        using namespace prompt::ai;
        check(normalise_base("localhost:1234") == "http://localhost:1234/v1", "normalise host");
        check(normalise_base("https://openrouter.ai/api/v1/") == "https://openrouter.ai/api/v1", "normalise v1");
        check(normalise_base("http://127.0.0.1:8080") == "http://127.0.0.1:8080/v1", "normalise port");

        auto const params = parse_params({"temperature=0.2", "top_p=1", "stop=null", "label=fast"});
        check(params["temperature"].is_number() && params["temperature"].get<double>() == 0.2, "param number");
        check(params["stop"].is_null(), "param null");
        check(params["label"].is_string() && params["label"].get<std::string>() == "fast", "param string");

        check(is_transient("HTTP 429: too many requests"), "transient 429");
        check(is_transient("curl: (28) timed out"), "transient timeout");
        check(!is_transient("HTTP 401: unauthorized"), "not transient 401");

        auto const payloads = sse_payloads({"data: {\"a\":1}", "", ": comment", "event: x", "data:[DONE]"});
        check(payloads.size() == 2, "sse payload count");
        check_equal(payloads[0], "{\"a\":1}", "sse payload 1");
        check_equal(payloads[1], "[DONE]", "sse payload 2");

        std::string content_type;
        auto const  body = encode_multipart(
          {
            {"field", "value",      "",           ""},
            { "file", "bytes", "a.txt", "text/plain"}
        },
          content_type);
        check(content_type.find("multipart/form-data; boundary=") == 0, "multipart content type");
        check(body.find("name=\"field\"") != std::string::npos, "multipart field");
        check(body.find("filename=\"a.txt\"") != std::string::npos, "multipart filename");
        check(body.find("bytes") != std::string::npos, "multipart bytes");
    }

    void test_registry() {
        using namespace prompt::ai;
        check(find_provider("lms") != nullptr, "registry lms");
        check(find_provider("openai") != nullptr, "registry openai");
        check(find_provider("openrouter") != nullptr, "registry openrouter");
        check(canonical_provider("lmstudio") == std::optional<std::string>{"lms"}, "alias lmstudio");
        check(canonical_provider("oai") == std::optional<std::string>{"openai"}, "alias oai");
        check(canonical_provider("ai.openrouter") == std::optional<std::string>{"openrouter"}, "alias ai. prefix");
        check(!canonical_provider("nope").has_value(), "unknown provider");
        check(find_provider("openrouter")->info().api, "api flag");
        check(!find_provider("openrouter")->info().needs_session, "no session for api");
    }

    // A one-shot HTTP server: it answers `body` with `status` and closes.
    class stub_server {
      public:
        explicit stub_server(std::string body, int status = 200, std::string content_type = "text/event-stream")
          : body_(std::move(body)),
            status_(status),
            content_type_(std::move(content_type)) {
            socket_       = ::socket(AF_INET, SOCK_STREAM, 0);
            int const one = 1;
            ::setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            sockaddr_in address{};
            address.sin_family      = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port        = 0;
            ::bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
            socklen_t length = sizeof(address);
            ::getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length);
            port_ = ntohs(address.sin_port);
            ::listen(socket_, 4);
            thread_ = std::thread([this] {
                serve();
            });
        }

        ~stub_server() {
            ::shutdown(socket_, SHUT_RDWR);
            ::close(socket_);
            if (thread_.joinable()) {
                thread_.join();
            }
        }

        [[nodiscard]] std::string url() const {
            return "http://127.0.0.1:" + std::to_string(port_);
        }

      private:
        void serve() {
            int const client = ::accept(socket_, nullptr, nullptr);
            if (client < 0) {
                return;
            }
            std::string request;
            char        buffer[4096];
            while (request.find("\r\n\r\n") == std::string::npos) {
                auto const count = ::read(client, buffer, sizeof(buffer));
                if (count <= 0) {
                    break;
                }
                request.append(buffer, static_cast<std::size_t>(count));
            }
            std::string response =
              "HTTP/1.1 " + std::to_string(status_) + " OK\r\nContent-Type: " + content_type_ +
              "\r\nContent-Length: " + std::to_string(body_.size()) + "\r\nConnection: close\r\n\r\n" + body_;
            ::write(client, response.data(), response.size());
            ::close(client);
        }

        std::string body_;
        int         status_;
        std::string content_type_;
        int         socket_ = -1;
        int         port_   = 0;
        std::thread thread_;
    };

    void test_http_retry();

    void test_http_stub() {
        using namespace prompt::ai;
        {
            stub_server  server("data: one\n\ndata: two\n\n");
            http_request request;
            request.url             = server.url() + "/stream";
            request.timeout_seconds = 5;
            std::vector<std::string> lines;
            auto const               response = stream_lines(request, [&lines](std::string_view line) {
                lines.emplace_back(line);
            });
            check(response.status == 200, "stub status");
            check(lines.size() == 4, "stub line count");
            check_equal(lines[0], "data: one", "stub line 1");
        }
        {
            stub_server  server("{\"error\":\"boom\"}", 401, "application/json");
            http_request request;
            request.url             = server.url() + "/fail";
            request.timeout_seconds = 5;
            bool threw              = false;
            try {
                (void) perform(request);
            } catch (http_error const& error) {
                threw = true;
                check(error.status() == 401, "http_error status");
                check(error.body().find("boom") != std::string::npos, "http_error body");
            }
            check(threw, "http_error thrown");
        }
        test_http_retry();
    }

    // One reply per connection, in order, so a retry can be observed.
    struct stub_reply {
        int                                              status = 200;
        std::string                                      body;
        std::string                                      content_type = "text/event-stream";
        std::vector<std::pair<std::string, std::string>> headers;
        bool                                             truncate = false; // advertise more bytes than sent, then close
    };

    class sequence_server {
      public:
        explicit sequence_server(std::vector<stub_reply> replies) : replies_(std::move(replies)) {
            socket_       = ::socket(AF_INET, SOCK_STREAM, 0);
            int const one = 1;
            ::setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            sockaddr_in address{};
            address.sin_family      = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port        = 0;
            ::bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
            socklen_t length = sizeof(address);
            ::getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length);
            port_ = ntohs(address.sin_port);
            ::listen(socket_, 8);
            thread_ = std::thread([this] {
                serve();
            });
        }

        ~sequence_server() {
            ::shutdown(socket_, SHUT_RDWR);
            ::close(socket_);
            if (thread_.joinable()) {
                thread_.join();
            }
        }

        [[nodiscard]] std::string url() const {
            return "http://127.0.0.1:" + std::to_string(port_);
        }

        [[nodiscard]] int connections() const {
            return connections_.load();
        }

      private:
        void serve() {
            std::size_t index = 0;
            for (;;) {
                int const client = ::accept(socket_, nullptr, nullptr);
                if (client < 0) {
                    return;
                }
                ++connections_;
                std::string request;
                char        buffer[4096];
                while (request.find("\r\n\r\n") == std::string::npos) {
                    auto const count = ::read(client, buffer, sizeof(buffer));
                    if (count <= 0) {
                        break;
                    }
                    request.append(buffer, static_cast<std::size_t>(count));
                }
                stub_reply reply;
                if (index < replies_.size()) {
                    reply = replies_[index++];
                } else {
                    reply = {.status       = 500,
                             .body         = "exhausted",
                             .content_type = "text/plain",
                             .headers      = {},
                             .truncate     = false};
                }
                std::string response =
                  "HTTP/1.1 " + std::to_string(reply.status) + " X\r\nContent-Type: " + reply.content_type + "\r\n";
                for (auto const& [name, value] : reply.headers) {
                    response += name + ": " + value + "\r\n";
                }
                std::size_t const length = reply.body.size() + (reply.truncate ? 64 : 0);
                response += "Content-Length: " + std::to_string(length) + "\r\nConnection: close\r\n\r\n";
                response += reply.body;
                ::write(client, response.data(), response.size());
                ::close(client);
            }
        }

        std::vector<stub_reply> replies_;
        int                     socket_ = -1;
        int                     port_   = 0;
        std::atomic<int>        connections_{0};
        std::thread             thread_;
    };

    void test_http_retry() {
        using namespace prompt::ai;
        {
            // A transient 503 is retried and the next attempt's body wins.
            sequence_server server({
              {      .status = 503,.body = "busy",.content_type = "text/event-stream",.headers = {},.truncate = false               },
              {.status       = 200,
               .body         = "recovered",
               .content_type = "text/event-stream",
               .headers      = {},
               .truncate     = false},
            });
            http_request    request;
            request.url             = server.url() + "/retry";
            request.timeout_seconds = 5;
            request.retries         = 2;
            request.retry_base_ms   = 1;
            request.retry_max_ms    = 2;
            auto const response     = perform(request);
            check_equal(response.body, "recovered", "retry recovers");
            check(server.connections() == 2, "retry made two attempts");
        }
        {
            // Retry-After is honoured (0 keeps the test fast).
            sequence_server server({
              {
               .status       = 429,
               .body         = "slow down",
               .content_type = "text/event-stream",
               .headers      = {{"Retry-After", "0"}},
               .truncate     = false,
               },
              {.status = 200, .body = "ok", .content_type = "text/event-stream", .headers = {}, .truncate = false},
            });
            http_request    request;
            request.url             = server.url() + "/retry-after";
            request.timeout_seconds = 5;
            request.retries         = 2;
            request.retry_base_ms   = 1;
            request.retry_max_ms    = 2;
            check_equal(perform(request).body, "ok", "retry-after recovers");
            check(server.connections() == 2, "retry-after made two attempts");
        }
        {
            // A 400 is the site rejecting the request: no retry.
            sequence_server server({
              {.status = 400, .body = "bad", .content_type = "text/event-stream", .headers = {}, .truncate = false},
              {
               .status       = 200,
               .body         = "unexpected",
               .content_type = "text/event-stream",
               .headers      = {},
               .truncate     = false,
               },
            });
            http_request    request;
            request.url             = server.url() + "/reject";
            request.timeout_seconds = 5;
            request.retries         = 2;
            request.retry_base_ms   = 1;
            request.retry_max_ms    = 2;
            bool threw              = false;
            try {
                (void) perform(request);
            } catch (http_error const& error) {
                threw = true;
                check(error.status() == 400, "400 status");
            }
            check(threw, "400 threw");
            check(server.connections() == 1, "400 not retried");
        }
        {
            // A stream that already delivered a line is never replayed.
            sequence_server server({
              {
               .status       = 200,
               .body         = "data: one\n\n",
               .content_type = "text/event-stream",
               .headers      = {},
               .truncate     = true,
               },
              {
               .status       = 200,
               .body         = "data: one\n\ndata: two\n\n",
               .content_type = "text/event-stream",
               .headers      = {},
               .truncate     = false,
               },
            });
            http_request    request;
            request.url             = server.url() + "/partial";
            request.timeout_seconds = 5;
            request.retries         = 2;
            request.retry_base_ms   = 1;
            request.retry_max_ms    = 2;
            std::vector<std::string> lines;
            bool                     threw = false;
            try {
                (void) stream_lines(request, [&lines](std::string_view line) {
                    lines.emplace_back(line);
                });
            } catch (http_error const&) {
                threw = true;
            }
            check(threw, "partial stream failed");
            check(lines.size() == 2, "partial stream delivered once");
            check(server.connections() == 1, "partial stream not replayed");
        }
    }

    void test_render() {
        using namespace prompt::ai;
        {
            std::string    out;
            std::string    err;
            render_options options;
            options.force_tty = true;
            options.color     = true;
            renderer view(
              options,
              [&out](std::string_view chunk) {
                  out.append(chunk);
              },
              [&err](std::string_view chunk) {
                  err.append(chunk);
              });
            view.handle(thinking("pondering"));
            view.handle(text("**bold** and `code`\n"));
            view.handle(text("plain\n"));
            view.finish();
            check(err.find("pondering") != std::string::npos, "render thinking text");
            check(err.find("\n\n") != std::string::npos, "render thinking separated from answer");
            check(out.find("\x1b[1m") != std::string::npos, "render bold");
            check(out.find("\x1b[36m") != std::string::npos, "render code");
            check(out.find("\x1b[0m") != std::string::npos, "render reset");
            check_equal(view.text(), "**bold** and `code`\nplain\n", "render plain answer");
        }
        {
            std::string    out;
            render_options options;
            options.raw = true;
            renderer view(
              options,
              [&out](std::string_view chunk) {
                  out.append(chunk);
              },
              [](std::string_view) {});
            view.handle(text("**raw**\n"));
            view.finish();
            check_equal(out, "**raw**\n", "render raw passthrough");
        }
        {
            std::string    out;
            render_options options;
            options.force_tty = true;
            options.color     = false;
            renderer view(
              options,
              [&out](std::string_view chunk) {
                  out.append(chunk);
              },
              [](std::string_view) {});
            view.handle(text("# Heading\n"));
            view.finish();
            check_equal(out, "# Heading\n", "render no color");
        }
    }

} // namespace

int main() {
    test_promptfile();
    test_history();
    test_state();
    test_files();
    test_keys();
    test_transport_helpers();
    test_registry();
    test_render();
    test_http_stub();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "ai: all checks passed\n";
    return 0;
}
