#include "../diagnostics.hpp"
#include "../files.hpp"
#include "../http.hpp"
#include "../models.hpp"
#include "provider.hpp"
#include "registry.hpp"

#include <cstdint>
#include <ctime>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dana::ai {

    namespace {

        using nlohmann::json;

        constexpr std::string_view kChatPath         = "/api/chat";
        constexpr std::string_view kStreamPath       = "/api/chat/{chat}/completion/stream";
        constexpr std::string_view kUploadPath       = "/apiv2-files/file/upload";
        constexpr std::string_view kRefreshPath      = "/api/auth/token/refresh";
        constexpr std::string_view kListMessagesPath = "/apiv2/kimi.gateway.chat.v1.ChatService/ListMessages";
        constexpr std::string_view kContinuePath     = "/apiv2/kimi.gateway.chat.v1.ChatService/ContinueChat";

        /// Renew the access token this many seconds before it expires.
        constexpr double kRefreshMargin = 60.0;

        /// The status the site stores on an answer it cut short (the Continue button).
        constexpr std::string_view kTruncatedStatus = "MESSAGE_STATUS_TRUNCATED";

        /// Each continuation is a new stream, so this only bounds a pathological chain.
        constexpr int kMaxContinuations = 8;

        /// The site titles a new chat from this much of the first prompt.
        constexpr std::size_t kTitleLimit = 60;

        std::string trim(std::string_view value) {
            std::size_t first = value.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos) {
                return {};
            }
            std::size_t last = value.find_last_not_of(" \t\r\n");
            return std::string(value.substr(first, last - first + 1));
        }

        std::string replace_all(std::string text, std::string_view needle, std::string_view replacement) {
            std::size_t position = 0;
            while ((position = text.find(needle, position)) != std::string::npos) {
                text.replace(position, needle.size(), replacement);
                position += replacement.size();
            }
            return text;
        }

        std::string title_of(std::string_view prompt) {
            std::string text;
            bool        space = false;
            for (char c : prompt) {
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    space = !text.empty();
                    continue;
                }
                if (space) {
                    text += ' ';
                }
                space  = false;
                text  += c;
            }
            if (text.size() > kTitleLimit) {
                text.resize(kTitleLimit);
            }
            return text.empty() ? "New chat" : text;
        }

        // Translate the site's conversation-length error into a remedy.
        std::string_view limit_message(std::string_view const message) noexcept {
            if (message.contains("超过") && (message.contains("万字") || message.contains("对话"))) {
                return {
                  "Kimi: this conversation has reached the site's 200,000-character limit; start a new chat "
                  "(--new) or split the prompt."};
            }
            return message;
        }

        // Decode a base64url segment (padding optional).
        std::string base64url_decode(std::string_view text) {
            std::string  out;
            unsigned int buffer = 0;
            int          bits   = 0;
            for (char c : text) {
                int index = 0;
                if (c >= 'A' && c <= 'Z') {
                    index = c - 'A';
                } else if (c >= 'a' && c <= 'z') {
                    index = c - 'a' + 26;
                } else if (c >= '0' && c <= '9') {
                    index = c - '0' + 52;
                } else if (c == '-' || c == '+') {
                    index = 62;
                } else if (c == '_' || c == '/') {
                    index = 63;
                } else if (c == '=') {
                    break;
                } else {
                    return {};
                }
                buffer  = (buffer << 6U) | static_cast<unsigned int>(index);
                bits   += 6;
                if (bits >= 8) {
                    bits   -= 8;
                    out    += static_cast<char>((buffer >> static_cast<unsigned int>(bits)) & 0xFFU);
                    buffer &= bits == 0 ? 0U : (1U << static_cast<unsigned int>(bits)) - 1U;
                }
            }
            return out;
        }

        // The `exp` claim of a JWT, or 0 when it cannot be read.
        double expires_at(std::string_view token) {
            auto const first = token.find('.');
            if (first == std::string::npos) {
                return 0.0;
            }
            auto const  second  = token.find('.', first + 1);
            std::string payload = std::string(
              token.substr(first + 1, second == std::string::npos ? std::string::npos : second - first - 1));
            while (payload.size() % 4 != 0) {
                payload += '=';
            }
            json data = json::parse(base64url_decode(payload), nullptr, false);
            if (data.is_discarded() || !data.is_object()) {
                return 0.0;
            }
            if (!data.contains("exp") || !data["exp"].is_number()) {
                return 0.0;
            }
            return data["exp"].get<double>();
        }

        // The payload of one SSE `data:` line, mirroring the Python side's
        // per-line `sse_payloads([line])` (blank lines and comments are ignored).
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
                        std::string_view                                  value) {
            for (auto& header : headers) {
                if (std::string_view(header.first) == name) {
                    header.second = value;
                    return;
                }
            }
            headers.emplace_back(name, value);
        }

        void apply_headers(http_request& request, std::vector<std::pair<std::string, std::string>> const& headers) {
            for (auto const& header : headers) {
                request.headers.push_back(header);
            }
        }

        // One Connect streaming frame around a JSON payload.
        std::string connect_frame(json const& payload) {
            std::string const data   = payload.dump();
            auto const        length = static_cast<std::uint32_t>(data.size());
            std::string       frame;
            frame.reserve(5 + data.size());
            frame.push_back('\0');
            frame.push_back(static_cast<char>((length >> 24U) & 0xFFU));
            frame.push_back(static_cast<char>((length >> 16U) & 0xFFU));
            frame.push_back(static_cast<char>((length >> 8U) & 0xFFU));
            frame.push_back(static_cast<char>(length & 0xFFU));
            frame += data;
            return frame;
        }

        class kimi_provider : public provider {
          public:
            kimi_provider()
              : info_{
                  .name                = "kimi",
                  .label               = "Kimi",
                  .origin              = "https://www.kimi.ai",
                  .session_target      = "www.kimi.ai",
                  .default_model       = std::string(models::default_model("kimi")),
                  .models              = models::list("kimi"),
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
              session::match const&             match,
              std::optional<std::string> const& token = std::nullopt) const {
                auto values = session::build_headers(
                  match,
                  {
                    { "Accept", "application/json, text/plain, */*"},
                    {"Referer",                  info_.origin + "/"},
                    { "Origin",                        info_.origin},
                });
                if (token && !token->empty()) {
                    values.emplace_back("Authorization", "Bearer " + *token);
                }
                return values;
            }

            [[nodiscard]] static std::optional<std::string> token(session::match const& match) {
                return session::storage_value(match, {"access_token", "accessToken"});
            }

            // Exchange the refresh token for a new access token (or nullopt).
            [[nodiscard]] std::optional<std::string> refresh(session::match const& match) const {
                auto const refresh_token = session::storage_value(match, {"refresh_token", "refreshToken"});
                if (!refresh_token || refresh_token->empty()) {
                    return std::nullopt;
                }
                http_request request;
                request.url             = info_.origin + std::string(kRefreshPath);
                request.timeout_seconds = 60;
                apply_headers(request, build_headers(match, refresh_token));
                json data = request_json(request);
                if (!data.is_object() || !data.contains("access_token") || !data["access_token"].is_string()) {
                    return std::nullopt;
                }
                std::string value = data["access_token"].get<std::string>();
                if (value.empty()) {
                    return std::nullopt;
                }
                return value;
            }

            [[nodiscard]] std::string create_chat(std::vector<std::pair<std::string, std::string>> const& values,
                                                  std::string_view prompt) const {
                http_request request;
                request.url       = info_.origin + std::string(kChatPath);
                request.method    = "POST";
                request.json_body = json{
                  {"name",
                   title_of(prompt)}
                }.dump();
                request.timeout_seconds = 60;
                apply_headers(request, values);
                json data = request_json(request);
                if (!data.is_object() || !data.contains("id") || !data["id"].is_string()) {
                    throw std::runtime_error("could not create a chat: " + data.dump());
                }
                return data["id"].get<std::string>();
            }

            // Upload one attachment and return the site's `file.id`.
            [[nodiscard]] std::string upload(std::vector<std::pair<std::string, std::string>> const& values,
                                             attachment const&                                       file) const {
                std::vector<multipart_field> fields{
                  {.name = "file", .value = file.data, .filename = file.name, .content_type = file.mime},
                };
                std::string       content_type;
                std::string const body            = encode_multipart(fields, content_type);
                auto              request_headers = values;
                set_header(request_headers, "Content-Type", content_type);
                http_request request;
                request.url             = info_.origin + std::string(kUploadPath);
                request.method          = "POST";
                request.body            = body;
                request.content_type    = content_type;
                request.timeout_seconds = 300;
                apply_headers(request, request_headers);
                json data = request_json(request);
                if (!data.is_object() || !data.contains("file") || !data["file"].is_object() ||
                    !data["file"].contains("id") || !data["file"]["id"].is_string())
                {
                    throw std::runtime_error("upload failed: " + data.dump());
                }
                return data["file"]["id"].get<std::string>();
            }

            static std::vector<std::pair<std::string, std::string>> connect_headers(
              std::vector<std::pair<std::string, std::string>> values,
              std::string_view                                 content_type) {
                set_header(values, "Accept", content_type);
                set_header(values, "Content-Type", content_type);
                set_header(values, "Connect-Protocol-Version", "1");
                return values;
            }

            // The stored status of one message ("" when it cannot be read).
            [[nodiscard]] std::string message_status(
              std::vector<std::pair<std::string, std::string>> const& values,
              std::string_view                                        chat_id,
              std::string_view                                        message_id) const {
                http_request request;
                request.url       = info_.origin + std::string(kListMessagesPath);
                request.method    = "POST";
                request.json_body = json{
                  {"chatId", chat_id}
                }.dump();
                request.timeout_seconds = 60;
                apply_headers(request, connect_headers(values, "application/json"));
                json data = request_json(request);
                if (!data.is_object() || !data.contains("messages") || !data["messages"].is_array()) {
                    return {};
                }
                for (auto const& message : data["messages"]) {
                    if (!message.is_object()) {
                        continue;
                    }
                    if (!message.contains("id") || !message["id"].is_string()) {
                        continue;
                    }
                    if (message["id"].get<std::string>() != message_id) {
                        continue;
                    }
                    if (message.contains("status") && message["status"].is_string()) {
                        return message["status"].get<std::string>();
                    }
                    return {};
                }
                return {};
            }

          private:
            provider_info info_;
        };

        struct [[nodiscard]] kimi_conversation : conversation {
            kimi_conversation(kimi_provider&      owner,
                              session::match      match,
                              std::string_view    model,
                              open_options const& options)
              : conversation(std::move(match), model, options),
                owner_(owner) {
                if (auto const stored = kimi_provider::token(match_)) {
                    token_ = *stored;
                }
            }

            void send(std::string_view prompt, text::string_views files, event_sink const& sink) override {
                ensure_token();
                if (token_.empty()) {
                    sink(error("no Kimi token in web storage; log in to www.kimi.ai"));
                    return;
                }
                auto values = owner_.build_headers(match_, token_);
                set_header(values, "Accept", "text/event-stream");

                std::vector<std::string> refs;
                std::vector<attachment>  attachments;
                try {
                    attachments = read_attachments(files);
                } catch (file_error const& failure) {
                    sink(error(failure.what()));
                    return;
                }
                for (auto const& file : attachments) {
                    try {
                        refs.push_back(owner_.upload(values, file));
                    } catch (std::exception const& failure) {
                        sink(error(failure.what()));
                        return;
                    }
                    sink(tool_call("upload", "done", file.name));
                }

                bool const        first = turns_ == 0 && conversation_id_.empty();
                std::string const content =
                  (first && !system_.empty()) ? system_ + "\n\n" + prompt : std::string(prompt);
                json body{
                  {  "messages", json::array({json{{"role", "user"}, {"content", content}}})},
                  {      "refs",                                                        refs},
                  {"use_search",                                                       false},
                };
                if (!model_.empty() && model_ != owner_.info().default_model) {
                    body["model"] = model_;
                }

                std::string answer;
                std::string message_id;
                bool        stopped = false;
                try {
                    if (conversation_id_.empty()) {
                        conversation_id_ = owner_.create_chat(values, prompt);
                    }
                    std::string const path = replace_all(std::string(kStreamPath), "{chat}", conversation_id_);
                    http_request      request;
                    request.url             = owner_.origin() + path;
                    request.method          = "POST";
                    request.json_body       = body.dump();
                    request.timeout_seconds = timeout_;
                    request.debug           = debug_;
                    apply_headers(request, values);
                    std::ignore = stream_lines(request, [&](std::string_view line) {
                        if (stopped || line.empty()) {
                            return;
                        }
                        if (debug_) {
                            sink(raw(std::string(line)));
                        }
                        std::string const payload = sse_payload(line);
                        if (payload.empty()) {
                            return;
                        }
                        json event = json::parse(payload, nullptr, false);
                        if (event.is_discarded()) {
                            sink(raw(payload));
                            return;
                        }
                        if (!event.is_object()) {
                            return;
                        }
                        std::string const kind = event.value("event", std::string{});
                        if (kind == "cmpl") {
                            if (!event.contains("text") || !event["text"].is_string()) {
                                return;
                            }
                            std::string const value = event["text"].get<std::string>();
                            if (value.empty()) {
                                return;
                            }
                            answer += value;
                            sink(text(value));
                        } else if (kind == "resp") {
                            if (!event.contains("id") || !event["id"].is_string()) {
                                return;
                            }
                            std::string const value = event["id"].get<std::string>();
                            if (!value.empty()) {
                                message_id = value;
                            }
                        } else if (kind == "error") {
                            std::string message = "Kimi returned an error";
                            if (event.contains("error")) {
                                auto const& failure = event["error"];
                                if (
                                  failure.is_object() && failure.contains("message") && failure["message"].is_string())
                                {
                                    message = failure["message"].get<std::string>();
                                } else if (failure.is_string()) {
                                    message = failure.get<std::string>();
                                }
                            }
                            stopped = true;
                            sink(error(limit_message(message)));
                        }
                    });
                } catch (std::exception const& failure) {
                    sink(error(failure.what()));
                    return;
                }
                if (stopped) {
                    return;
                }

                // The site stores a cut-short answer as MESSAGE_STATUS_TRUNCATED and
                // shows a "Continue" button; the SSE stream itself never says so, so
                // the message has to be looked up.  ContinueChat answers with a new
                // child message, hence the loop on the latest id.
                int continuations = 0;
                while (continuations < kMaxContinuations && !message_id.empty() &&
                       status(values, message_id) == kTruncatedStatus)
                {
                    ++continuations;
                    sink(tool_call("continue", "auto", "message " + message_id));
                    std::string new_status;
                    try {
                        auto const [new_id, final_status] = continue_stream(values, message_id, answer, sink);
                        message_id                        = new_id;
                        new_status                        = final_status;
                    } catch (std::exception const& failure) {
                        sink(error(failure.what()));
                        break;
                    }
                    if (new_status != kTruncatedStatus) {
                        break;
                    }
                }

                ++turns_;
                record("user", prompt);
                record("assistant", answer);
                sink(done());
            }

          private:
            std::string status(std::vector<std::pair<std::string, std::string>> const& values,
                               std::string_view                                        message_id) {
                try {
                    return owner_.message_status(values, conversation_id_, message_id);
                } catch (std::exception const& failure) {
                    note(std::format("status lookup for message {} failed: {}; not asking for a continuation",
                                     message_id,
                                     failure.what()),
                         debug_,
                         "kimi");
                    return {};
                }
            }

            // Stream the rest of a truncated answer; return (id, final status).
            //
            // The response is Connect server-streaming, so each frame is a
            // length-prefixed JSON patch rather than an SSE line; `block.text.content`
            // patches carry the text.
            std::pair<std::string, std::string> continue_stream(
              std::vector<std::pair<std::string, std::string>> const& values,
              std::string_view                                        message_id,
              std::string&                                            answer,
              event_sink const&                                       sink) {
                auto const        request_headers = kimi_provider::connect_headers(values, "application/connect+json");
                std::string const body            = connect_frame(json{
                  {   "chatId", conversation_id_},
                  {"messageId",       message_id}
                });
                http_request      request;
                request.url             = owner_.origin() + std::string(kContinuePath);
                request.method          = "POST";
                request.body            = body;
                request.content_type    = "application/connect+json";
                request.timeout_seconds = timeout_;
                request.debug           = debug_;
                apply_headers(request, request_headers);

                std::string                        buffer;
                std::map<std::string, std::string> texts;
                auto                               new_id = message_id;
                std::string                        final_status;
                bool                               finished = false;

                std::ignore = stream_chunks(request, [&](std::string_view chunk) {
                    if (finished) {
                        return;
                    }
                    buffer.append(chunk);
                    while (buffer.size() >= 5) {
                        auto const flag = static_cast<unsigned char>(buffer[0]);
                        auto const length =
                          (static_cast<std::uint32_t>(static_cast<unsigned char>(buffer[1])) << 24U) |
                          (static_cast<std::uint32_t>(static_cast<unsigned char>(buffer[2])) << 16U) |
                          (static_cast<std::uint32_t>(static_cast<unsigned char>(buffer[3])) << 8U) |
                          static_cast<std::uint32_t>(static_cast<unsigned char>(buffer[4]));
                        if (buffer.size() < 5 + length) {
                            break;
                        }
                        std::string const payload = buffer.substr(5, length);
                        buffer.erase(0, 5 + length);
                        if (flag == 2) {
                            finished = true;
                            return;
                        }
                        if (flag != 0) {
                            continue;
                        }
                        json patch = json::parse(payload, nullptr, false);
                        if (patch.is_discarded() || !patch.is_object()) {
                            continue;
                        }
                        if (patch.contains("message") && patch["message"].is_object()) {
                            auto const& message = patch["message"];
                            if (message.contains("id") && message["id"].is_string()) {
                                std::string const value = message["id"].get<std::string>();
                                if (!value.empty()) {
                                    new_id = value;
                                }
                            }
                            if (message.contains("status") && message["status"].is_string()) {
                                std::string const value = message["status"].get<std::string>();
                                if (!value.empty()) {
                                    final_status = value;
                                }
                            }
                        }
                        if (!patch.contains("block") || !patch["block"].is_object()) {
                            continue;
                        }
                        auto const& block = patch["block"];
                        if (!block.contains("text") || !block["text"].is_object()) {
                            continue;
                        }
                        auto const& text_block = block["text"];
                        if (!text_block.contains("content") || !text_block["content"].is_string()) {
                            continue;
                        }
                        std::string const content = text_block["content"].get<std::string>();
                        if (content.empty()) {
                            continue;
                        }
                        std::string const block_id = block.value("id", std::string{});
                        std::string const previous = texts.contains(block_id) ? texts[block_id] : std::string{};
                        if (patch.value("op", std::string{}) == "append") {
                            texts[block_id]  = previous + content;
                            answer          += content;
                            sink(text(content));
                        } else if (content.starts_with(previous) && content.size() > previous.size()) {
                            std::string const delta  = content.substr(previous.size());
                            texts[block_id]          = content;
                            answer                  += delta;
                            sink(text(delta));
                        }
                    }
                });
                return {std::string(new_id), final_status};
            }

            // Renew an expiring access token (best effort; keep the old one).
            void ensure_token() {
                if (!token_.empty() && expires_at(token_) > static_cast<double>(std::time(nullptr)) + kRefreshMargin) {
                    return;
                }
                try {
                    if (auto const refreshed = owner_.refresh(match_)) {
                        token_ = *refreshed;
                    }
                } catch (std::exception const& failure) {
                    note("token refresh failed: " + std::string(failure.what()), debug_, "kimi");
                }
            }

            kimi_provider& owner_;
            std::string    token_;
        };

        std::unique_ptr<conversation>
        kimi_provider::open(session::match const& match, std::string_view model, open_options const& options) {
            return std::make_unique<kimi_conversation>(*this, match, model, options);
        }

    } // namespace

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization)
    bool const kimi_registered = register_provider(std::make_unique<kimi_provider>());

} // namespace dana::ai
