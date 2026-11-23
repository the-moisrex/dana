#include "../diagnostics.hpp"
#include "../files.hpp"
#include "../http.hpp"
#include "../models.hpp"
#include "../pow.hpp"
#include "../uuid.hpp"
#include "provider.hpp"
#include "registry.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace dana::ai {

    namespace {

        using nlohmann::json;

        constexpr std::string_view kCompletionPath    = "/api/v0/chat/completion";
        constexpr std::string_view kContinuePath      = "/api/v0/chat/continue";
        constexpr std::string_view kUploadPath        = "/api/v0/file/upload_file";
        constexpr std::string_view kFilesPath         = "/api/v0/file/fetch_files";
        constexpr std::string_view kCreateSessionPath = "/api/v0/chat_session/create";
        constexpr std::string_view kChallengePath     = "/api/v0/chat/create_pow_challenge";
        constexpr std::string_view kClientVersion     = "2.5.0";

        /// The site cuts a long answer (or a slow reasoning run) short and offers a
        /// "Continue" button; each click is one more stream, so this bounds the chain.
        constexpr int kMaxContinuations = 8;

        /// How long to wait for an uploaded file to become referenceable.
        constexpr int kFilePollSeconds = 120;

        std::string trim(std::string_view value) {
            std::size_t first = value.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos) {
                return {};
            }
            std::size_t last = value.find_last_not_of(" \t\r\n");
            return std::string(value.substr(first, last - first + 1));
        }

        // The payload of one SSE `data:` line, mirroring the Python side's per-line
        // `sse_payloads([line])` (blank lines and comments are ignored).
        std::string sse_payload(std::string_view line) {
            if (line.empty() || line.front() == ':') {
                return {};
            }
            if (!line.starts_with("data:")) {
                return {};
            }
            return trim(line.substr(5));
        }

        void set_header(std::vector<std::pair<std::string, std::string>>& headers,
                        std::string_view                                  name,
                        std::string                                       value) {
            for (auto& header : headers) {
                if (std::string_view(header.first) == name) {
                    header.second = std::move(value);
                    return;
                }
            }
            headers.emplace_back(std::string(name), std::move(value));
        }

        void apply_headers(http_request& request, std::vector<std::pair<std::string, std::string>> const& headers) {
            for (auto const& header : headers) {
                request.headers.push_back(header);
            }
        }

        std::string string_or(json const& object, std::string_view key, std::string fallback = {}) {
            if (!object.is_object() || !object.contains(key)) {
                return fallback;
            }
            auto const& value = object[std::string(key)];
            if (!value.is_string()) {
                return fallback;
            }
            return value.get<std::string>();
        }

        std::int64_t integer_or(json const& object, std::string_view key, std::int64_t fallback) {
            if (!object.is_object() || !object.contains(key)) {
                return fallback;
            }
            auto const& value = object[std::string(key)];
            if (value.is_number_integer()) {
                return value.get<std::int64_t>();
            }
            if (value.is_number_unsigned()) {
                return static_cast<std::int64_t>(value.get<std::uint64_t>());
            }
            if (value.is_number_float()) {
                return static_cast<std::int64_t>(value.get<double>());
            }
            return fallback;
        }

        // The site's ids are JSON numbers (message ids) or strings (chat/file ids).
        std::string id_text(json const& value) {
            if (value.is_string()) {
                return value.get<std::string>();
            }
            if (value.is_number_integer()) {
                return std::to_string(value.get<std::int64_t>());
            }
            if (value.is_number_unsigned()) {
                return std::to_string(value.get<std::uint64_t>());
            }
            return {};
        }

        std::string id_text_of(json const& object, std::string_view key) {
            if (!object.is_object() || !object.contains(key)) {
                return {};
            }
            return id_text(object[std::string(key)]);
        }

        std::string kind_of(json const& fragment) {
            std::string const kind = string_or(fragment, "type");
            return kind.empty() ? std::string("RESPONSE") : kind;
        }

        // Python's `str` length counts code points, and the continuation prefix is
        // dropped by code point, so the skip counts must too (Persian/Chinese answers).
        std::size_t codepoint_length(std::string_view text) {
            std::size_t count = 0;
            for (char byte : text) {
                if ((static_cast<unsigned char>(byte) & 0xC0U) != 0x80U) {
                    ++count;
                }
            }
            return count;
        }

        std::string drop_codepoints(std::string_view text, std::size_t count) {
            std::size_t index = 0;
            std::size_t seen  = 0;
            while (index < text.size() && seen < count) {
                ++index;
                while (index < text.size() && (static_cast<unsigned char>(text[index]) & 0xC0U) == 0x80U) {
                    ++index;
                }
                ++seen;
            }
            return std::string(text.substr(index));
        }

        std::string lowercase(std::string text) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
            return text;
        }

        bool file_settled(json const& record) {
            std::string const status = string_or(record, "status");
            if (status.empty()) {
                return false;
            }
            return status == "SUCCESS" || status == "FAILED" || status == "CONTENT_EMPTY" ||
                   status == "CONTENT_TOO_LONG";
        }

        // Find the `{"id": file_id, ..., "status": ...}` record inside a nested
        // response (iterative, so a deeply nested body cannot overflow the stack).
        json find_file(json const& root, std::string_view file_id) {
            std::vector<json const*> stack{&root};
            while (!stack.empty()) {
                json const* node = stack.back();
                stack.pop_back();
                if (node->is_object()) {
                    auto const id = node->find("id");
                    if (id != node->end() && id->is_string() && id->get<std::string>() == file_id &&
                        node->contains("status"))
                    {
                        return *node;
                    }
                    for (auto const& value : *node) {
                        stack.push_back(&value);
                    }
                } else if (node->is_array()) {
                    for (auto const& value : *node) {
                        stack.push_back(&value);
                    }
                }
            }
            return {};
        }

        // Turn DeepSeek's fragment-patch frames into deltas.
        class fragment_parser {
          public:
            explicit fragment_parser(std::int64_t skip_response = 0, std::int64_t skip_thinking = 0) {
                skip_["RESPONSE"] = skip_response;
                skip_["THINK"]    = skip_thinking;
            }

            // A BATCH frame nests its own frames, so this one call is recursive.
            // NOLINTNEXTLINE(misc-no-recursion)
            std::vector<stream_event> feed(json const& object) {
                std::vector<stream_event> out;
                if (!object.is_object()) {
                    return out;
                }
                if (object.contains("click_behavior")) {
                    close_ = object;
                    return out;
                }
                json const* value = object.contains("v") ? &object["v"] : nullptr;
                if (value != nullptr && value->is_object()) {
                    auto const response = value->find("response");
                    if (response != value->end() && response->is_object()) {
                        return snapshot(*response);
                    }
                }
                if (object.contains("response") && object["response"].is_object()) {
                    return snapshot(object["response"]);
                }
                if (!object.contains("p") || object["p"].is_null()) {
                    if (value != nullptr && value->is_string()) {
                        return content(value->get<std::string>());
                    }
                    return out;
                }
                std::string const path = object["p"].is_string() ? object["p"].get<std::string>() : std::string{};
                std::string       op   = string_or(object, "o");
                if (op.empty()) {
                    op = "APPEND";
                }
                if (path == "response/status" || path == "quasi_status") {
                    if (value != nullptr && value->is_string()) {
                        status_ = value->get<std::string>();
                    }
                    return out;
                }
                if (op == "BATCH" && value != nullptr && value->is_array()) {
                    for (auto const& sub : *value) {
                        auto const events = feed(sub);
                        out.insert(out.end(), events.begin(), events.end());
                    }
                    return out;
                }
                if (path == "response/fragments" && op == "APPEND") {
                    if (value == nullptr) {
                        return out;
                    }
                    if (value->is_array()) {
                        for (auto const& fragment : *value) {
                            if (!fragment.is_object()) {
                                continue;
                            }
                            fragments_.push_back(kind_of(fragment));
                            auto const events = content(string_or(fragment, "content"));
                            out.insert(out.end(), events.begin(), events.end());
                        }
                    } else if (value->is_object()) {
                        fragments_.push_back(kind_of(*value));
                        auto const events = content(string_or(*value, "content"));
                        out.insert(out.end(), events.begin(), events.end());
                    }
                    return out;
                }
                if (path.ends_with("/content") && (op == "APPEND" || op == "SET")) {
                    return content(value != nullptr && value->is_string() ? value->get<std::string>() : std::string{});
                }
                if (path.ends_with("/accumulated_token_usage") && value != nullptr && value->is_number_integer()) {
                    usage_ = value->get<std::int64_t>();
                }
                return out;
            }

            [[nodiscard]] bool has_message_id() const {
                return message_id_.is_number_integer();
            }

            [[nodiscard]] json const& message_id() const {
                return message_id_;
            }

            void set_message_id(json value) {
                message_id_ = std::move(value);
            }

            // True when the site closed the stream before the message finished.
            [[nodiscard]] bool incomplete() const {
                std::string const behavior = string_or(close_, "click_behavior");
                if (!behavior.empty() && behavior != "none") {
                    return true;
                }
                return !status_.empty() && status_ != "FINISHED";
            }

            // True once the site reported the message as finished. DeepSeek often
            // resets the SSE socket right after the final frame, which libcurl
            // reports as a receive error even though the answer is complete.
            [[nodiscard]] bool finished() const {
                return status_ == "FINISHED";
            }

          private:
            std::vector<stream_event> snapshot(json const& response) {
                std::vector<stream_event> out;
                if (response.contains("message_id") && response["message_id"].is_number_integer()) {
                    message_id_ = response["message_id"];
                }
                if (response.contains("fragments") && response["fragments"].is_array()) {
                    for (auto const& fragment : response["fragments"]) {
                        if (!fragment.is_object()) {
                            continue;
                        }
                        std::string kind = kind_of(fragment);
                        std::string text = string_or(fragment, "content");
                        auto const  skip = skip_.find(kind);
                        if (skip != skip_.end() && skip->second > 0) {
                            auto const drop =
                              std::min<std::size_t>(static_cast<std::size_t>(skip->second), codepoint_length(text));
                            text          = drop_codepoints(text, drop);
                            skip->second -= static_cast<std::int64_t>(drop);
                        }
                        fragments_.push_back(kind);
                        auto const events = content(text);
                        out.insert(out.end(), events.begin(), events.end());
                    }
                }
                if (response.contains("accumulated_token_usage") &&
                    response["accumulated_token_usage"].is_number_integer())
                {
                    usage_ = response["accumulated_token_usage"].get<std::int64_t>();
                }
                return out;
            }

            std::vector<stream_event> content(std::string_view text) {
                if (text.empty()) {
                    return {};
                }
                std::string const kind = fragments_.empty() ? std::string("RESPONSE") : fragments_.back();
                if (kind == "THINK") {
                    return {thinking(text)};
                }
                if (kind == "RESPONSE") {
                    return {text_event(text)};
                }
                return {tool_call(lowercase(kind), "delta", std::string(text))};
            }

            static stream_event text_event(std::string_view value) {
                return text(value);
            }

            std::vector<std::string>            fragments_;
            std::int64_t                        usage_      = 0;
            json                                message_id_ = nullptr;
            std::string                         status_;
            json                                close_ = nullptr;
            std::map<std::string, std::int64_t> skip_;
        };

        class deepseek_provider : public provider {
          public:
            deepseek_provider()
              : info_{
                  .name                = "dseek",
                  .label               = "DeepSeek",
                  .origin              = "https://chat.deepseek.com",
                  .session_target      = "chat.deepseek.com",
                  .default_model       = std::string(models::default_model("dseek")),
                  .models              = models::list("dseek"),
                  .inline_limit        = 256UL * 1024UL,
                  .needs_session       = true,
                  .api                 = false,
                  .local               = false,
                  .recordable          = true,
                  .prompt_file_upload  = true,
                  .auto_attach_by_size = true,
                  .discovers_model     = false,
                  .base_url            = "",
                  .base_url_env        = "",
                  .key_env             = "",
                  .model_env           = "",
                  .default_base_url    = "",
                } {}

            [[nodiscard]] provider_info const& info() const override {
                return info_;
            }

            std::unique_ptr<conversation>
            open(session::match const& match, std::string_view model, open_options const& options) override;

            [[nodiscard]] std::string_view origin() const {
                return info_.origin;
            }

            [[nodiscard]] std::vector<std::pair<std::string, std::string>> build_headers(
              session::match const& match) const {
                std::string device;
                if (auto const stored = session::storage_value(match, {"deepseek-device-id:chat"});
                    stored && !stored->empty())
                {
                    device = *stored;
                } else {
                    device = uuid4();
                }
                return session::build_headers(
                  match,
                  {
                    {                  "Accept",                       "*/*"},
                    {            "Content-Type",          "application/json"},
                    {                 "Referer",          info_.origin + "/"},
                    {       "x-client-platform",                       "web"},
                    {      "x-client-bundle-id",         "com.deepseek.chat"},
                    {        "x-client-version", std::string(kClientVersion)},
                    {         "x-client-locale",                     "en_US"},
                    {"x-client-timezone-offset",   std::to_string(-timezone)},
                    {             "x-device-id",                      device},
                });
            }

            [[nodiscard]] static std::optional<std::string> token(session::match const& match) {
                return session::storage_value(match,
                                              {"userToken", "token", "Authorization", "access_token", "ds_token"});
            }

            // The site wraps every payload in `data` (and sometimes `data.biz_data`).
            [[nodiscard]] static json biz(json const& data) {
                json result = json::object();
                if (data.is_object() && data.contains("data") && data["data"].is_object()) {
                    result = data["data"];
                }
                if (result.is_object() && result.contains("biz_data")) {
                    result = result["biz_data"];
                }
                return result;
            }

            [[nodiscard]] std::string create_session(
              std::vector<std::pair<std::string, std::string>> const& headers) const {
                http_request request;
                request.url             = info_.origin + std::string(kCreateSessionPath);
                request.method          = "POST";
                request.json_body       = json::object().dump();
                request.timeout_seconds = 60;
                apply_headers(request, headers);
                json const data = biz(request_json(request));
                if (!data.is_object()) {
                    return {};
                }
                if (data.contains("chat_session") && data["chat_session"].is_object()) {
                    std::string const id = id_text_of(data["chat_session"], "id");
                    if (!id.empty()) {
                        return id;
                    }
                }
                std::string id = id_text_of(data, "id");
                if (id.empty()) {
                    id = id_text_of(data, "chat_session_id");
                }
                return id;
            }

            // Solve the challenge for `target_path` and return the header value ("" when
            // the site did not hand one out).
            [[nodiscard]] std::string pow_header(std::vector<std::pair<std::string, std::string>> const& headers,
                                                 std::string_view target_path) const {
                http_request request;
                request.url       = info_.origin + std::string(kChallengePath);
                request.method    = "POST";
                request.json_body = json{
                  {"target_path",
                   std::string(
                     target_path)}
                }.dump();
                request.timeout_seconds = 60;
                apply_headers(request, headers);
                json challenge = biz(request_json(request));
                if (challenge.is_object() && challenge.contains("challenge")) {
                    challenge = challenge["challenge"];
                }
                if (!challenge.is_object()) {
                    return {};
                }
                std::string const  salt       = string_or(challenge, "salt");
                std::int64_t const expire_at  = integer_or(challenge, "expire_at", 0);
                std::int64_t const difficulty = integer_or(challenge, "difficulty", 1);
                std::string const  value      = string_or(challenge, "challenge");
                std::int64_t const answer     = pow::solve_deepseek(salt, expire_at, difficulty, value);
                return pow::deepseek_response(
                  salt,
                  value,
                  string_or(challenge, "signature"),
                  answer,
                  std::string(target_path));
            }

            // Upload one attachment and wait for it to become referenceable.
            [[nodiscard]] json upload(std::vector<std::pair<std::string, std::string>> const& headers,
                                      attachment const&                                       file,
                                      bool                                                    debug) const {
                auto              request_headers = headers;
                std::string const pow             = pow_header(request_headers, kUploadPath);
                if (!pow.empty()) {
                    set_header(request_headers, "x-ds-pow-response", pow);
                }
                std::vector<multipart_field> const fields{
                  {.name = "file", .value = file.data, .filename = file.name, .content_type = file.mime},
                };
                std::string       content_type;
                std::string const body = encode_multipart(fields, content_type);
                set_header(request_headers, "Content-Type", content_type);
                http_request request;
                request.url             = info_.origin + std::string(kUploadPath);
                request.method          = "POST";
                request.body            = body;
                request.content_type    = content_type;
                request.timeout_seconds = 300;
                apply_headers(request, request_headers);
                json const data = biz(request_json(request));
                if (!data.is_object() || !data.contains("id") || id_text(data["id"]).empty()) {
                    throw std::runtime_error("upload failed: " + data.dump());
                }
                return await_file(request_headers, data, debug);
            }

          private:
            // Poll `fetch_files` until the file is parsed (or clearly dead).
            [[nodiscard]] json
            await_file(std::vector<std::pair<std::string, std::string>> const& headers, json record, bool debug) const {
                std::string const file_id  = id_text_of(record, "id");
                auto const        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kFilePollSeconds);
                while (!file_settled(record)) {
                    if (std::chrono::steady_clock::now() >= deadline) {
                        return record;
                    }
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    try {
                        http_request request;
                        request.url             = info_.origin + std::string(kFilesPath) + "?file_ids=" + file_id;
                        request.timeout_seconds = 60;
                        apply_headers(request, headers);
                        json const found = find_file(biz(request_json(request)), file_id);
                        if (found.is_null()) {
                            return record;
                        }
                        record = found;
                    } catch (std::exception const& failure) {
                        note("could not poll file " + file_id + ": " + failure.what() +
                               "; using its last known status '" + string_or(record, "status") + "'",
                             debug,
                             "deepseek");
                        return record;
                    }
                }
                return record;
            }

            provider_info info_;
        };

        struct [[nodiscard]] deepseek_conversation : conversation {
            deepseek_conversation(
              deepseek_provider&     owner,
              session::match         match,
              std::string_view const model,
              open_options const&    options)
              : conversation(std::move(match), model, options),
                owner_(owner),
                headers_(owner.build_headers(match_)) {
                if (auto const stored = deepseek_provider::token(match_)) {
                    token_ = *stored;
                }
                thinking_ = model_ == "deepseek_reasoner" || model_ == "reasoner";
                if (options.resume.is_object() && options.resume.contains("parent_message_id")) {
                    parent_message_id_ = options.resume["parent_message_id"];
                }
            }

            void send(std::string_view prompt, text::string_views files, event_sink const& sink) override {
                if (token_.empty()) {
                    sink(error("no DeepSeek token in web storage; log in to chat.deepseek.com"));
                    return;
                }
                auto headers = headers_;
                set_header(headers, "Authorization", "Bearer " + token_);
                try {
                    if (conversation_id_.empty()) {
                        conversation_id_ = owner_.create_session(headers);
                    }
                    std::string const pow = owner_.pow_header(headers, kCompletionPath);
                    if (!pow.empty()) {
                        set_header(headers, "x-ds-pow-response", pow);
                    }
                } catch (std::exception const& failure) {
                    sink(error(failure.what()));
                    return;
                }

                std::vector<std::string> ref_ids;
                std::vector<attachment>  attachments;
                try {
                    attachments = read_attachments(files);
                } catch (file_error const& failure) {
                    sink(error(failure.what()));
                    return;
                }
                for (auto const& file : attachments) {
                    json record;
                    try {
                        record = owner_.upload(headers, file, debug_);
                    } catch (std::exception const& failure) {
                        sink(error(failure.what()));
                        return;
                    }
                    std::string const status = string_or(record, "status");
                    if (!status.empty() && status != "SUCCESS") {
                        sink(error(file.name + ": the site could not read the file (" + status +
                                   "); split it into smaller files or shorten the prompt"));
                        return;
                    }
                    ref_ids.push_back(id_text_of(record, "id"));
                    sink(tool_call("upload", "done", file.name));
                }

                bool const        first = turns_ == 0;
                std::string const content =
                  (first && !system_.empty()) ? system_ + "\n\n" + std::string(prompt) : std::string(prompt);
                json const body{
                  {  "chat_session_id",   conversation_id_},
                  {"parent_message_id", parent_message_id_},
                  {       "model_type",          "default"},
                  {           "prompt",            content},
                  {     "ref_file_ids",            ref_ids},
                  { "thinking_enabled",          thinking_},
                  {   "search_enabled",              false},
                  {           "action",            nullptr},
                  {          "preempt",              false},
                };

                fragment_parser parser;
                std::string     answer;
                std::string     thinking_text;
                try {
                    consume(kCompletionPath, headers, body, parser, answer, thinking_text, sink);
                } catch (std::exception const& failure) {
                    sink(error(failure.what()));
                    return;
                }

                // The site truncates an answer that ran long (or whose reasoning did)
                // and stops without finishing the message; the web UI then shows a
                // "Continue" button.  Ask for the rest, appending to the same answer.
                int continuations = 0;
                while (continuations < kMaxContinuations && parser.incomplete() && parser.has_message_id()) {
                    ++continuations;
                    sink(tool_call("continue", "auto", "message " + id_text(parser.message_id())));
                    auto continue_headers = headers_;
                    set_header(continue_headers, "Authorization", "Bearer " + token_);
                    try {
                        std::string const pow = owner_.pow_header(continue_headers, kContinuePath);
                        if (!pow.empty()) {
                            set_header(continue_headers, "x-ds-pow-response", pow);
                        }
                    } catch (std::exception const& failure) {
                        sink(error(failure.what()));
                        break;
                    }
                    json const previous_id = parser.message_id();
                    json const continue_body{
                      {"chat_session_id", conversation_id_},
                      {     "message_id",      previous_id},
                    };
                    fragment_parser next(static_cast<std::int64_t>(codepoint_length(answer)),
                                         static_cast<std::int64_t>(codepoint_length(thinking_text)));
                    try {
                        consume(kContinuePath, continue_headers, continue_body, next, answer, thinking_text, sink);
                    } catch (std::exception const& failure) {
                        sink(error(failure.what()));
                        break;
                    }
                    if (!next.has_message_id()) {
                        next.set_message_id(previous_id);
                    }
                    parser = std::move(next);
                }

                ++turns_;
                if (parser.has_message_id()) {
                    parent_message_id_           = parser.message_id();
                    resume_["parent_message_id"] = parser.message_id();
                }
                record("user", prompt);
                record("assistant", answer);
                sink(done());
            }

          private:
            // Stream one response through `parser`, filling `answer`/`thinking_text`.
            void consume(
              std::string_view                                        path,
              std::vector<std::pair<std::string, std::string>> const& headers,
              json const&                                             body,
              fragment_parser&                                        parser,
              std::string&                                            answer,
              std::string&                                            thinking_text,
              event_sink const&                                       sink) {
                http_request request;
                request.url             = owner_.origin() + std::string(path);
                request.method          = "POST";
                request.json_body       = body.dump();
                request.timeout_seconds = timeout_;
                request.debug           = debug_;
                apply_headers(request, headers);
                try {
                    std::ignore = stream_lines(request, [&](std::string_view line) {
                        std::string const payload = sse_payload(line);
                        if (!payload.empty()) {
                            json const object = json::parse(payload, nullptr, false);
                            if (object.is_discarded()) {
                                sink(raw(payload));
                            } else {
                                for (auto const& event : parser.feed(object)) {
                                    if (event.kind == event_kind::text) {
                                        answer += event.text;
                                    } else if (event.kind == event_kind::thinking) {
                                        thinking_text += event.text;
                                    }
                                    sink(event);
                                }
                            }
                        }
                        if (debug_ && !line.empty() && !line.starts_with("data:") && !line.starts_with("event:")) {
                            sink(raw(line));
                        }
                    });
                } catch (http_error const& failure) {
                    // The site closes the SSE socket abruptly once the final frame
                    // is out; libcurl then reports a receive error even though the
                    // message already reached FINISHED. Keep the complete answer
                    // instead of failing a turn that actually succeeded.
                    if (!parser.finished() || !is_network_failure(failure.what())) {
                        throw;
                    }
                }
            }

            deepseek_provider&                               owner_;
            std::vector<std::pair<std::string, std::string>> headers_;
            std::string                                      token_;
            bool                                             thinking_          = false;
            json                                             parent_message_id_ = nullptr;
        };

        std::unique_ptr<conversation>
        deepseek_provider::open(session::match const& match, std::string_view model, open_options const& options) {
            return std::make_unique<deepseek_conversation>(*this, match, model, options);
        }

    } // namespace

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization)
    bool const deepseek_registered = register_provider(std::make_unique<deepseek_provider>());

} // namespace dana::ai
