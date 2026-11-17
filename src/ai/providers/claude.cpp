#include "../diagnostics.hpp"
#include "../files.hpp"
#include "../http.hpp"
#include "../models.hpp"
#include "../sse.hpp"
#include "../uuid.hpp"
#include "provider.hpp"
#include "registry.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace dana::ai {

    namespace {

        using nlohmann::json;

        constexpr std::string_view kOrgsPath       = "/api/organizations";
        constexpr std::string_view kChatsPath      = "/api/organizations/{org}/chat_conversations";
        constexpr std::string_view kChatPath       = "/api/organizations/{org}/chat_conversations/{chat}";
        constexpr std::string_view kUploadPath     = "/api/organizations/{org}/conversations/{chat}/wiggle/upload-file";
        constexpr std::string_view kCompletionPath = "/api/organizations/{org}/chat_conversations/{chat}/completion";
        constexpr std::string_view kZeroUuid       = "00000000-0000-4000-8000-000000000000";
        constexpr std::size_t      kTitleLimit     = 60;
        constexpr std::size_t      kMaxReplay      = 40;

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

        std::string timezone_name() {
            char const* zone = std::getenv("TZ");
            if (zone != nullptr && *zone != '\0') {
                return zone;
            }
            std::error_code ec;
            auto const      target = std::filesystem::read_symlink("/etc/localtime", ec);
            if (ec) {
                return "UTC";
            }
            std::string const text   = target.string();
            auto const        marker = text.find("/zoneinfo/");
            if (marker == std::string::npos) {
                return "UTC";
            }
            return text.substr(marker + 10);
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

        std::optional<std::string> model_alias(std::string const& requested) {
            if (auto const alias = models::resolve_alias("claude", requested)) {
                if (alias->empty()) {
                    return std::nullopt;
                }
                return std::string(*alias);
            }
            return requested;
        }

        void apply_headers(http_request& request, std::vector<std::pair<std::string, std::string>> const& headers) {
            for (auto const& header : headers) {
                request.headers.push_back(header);
            }
        }

        std::string error_message(json const& event) {
            if (!event.contains("error")) {
                return "Claude returned an error";
            }
            auto const& error = event["error"];
            if (error.is_object() && error.contains("message") && error["message"].is_string()) {
                return error["message"].get<std::string>();
            }
            if (error.is_string()) {
                return error.get<std::string>();
            }
            return "Claude returned an error";
        }

        std::string friendly(std::string const& message) {
            std::string lowered = message;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (lowered.contains("model_not_available") || lowered.contains("model not available")) {
                return message + " (this account cannot use that model; try --model auto)";
            }
            return message;
        }

        class claude_provider : public provider {
          public:
            claude_provider()
              : info_{
                  .name                = "claude",
                  .label               = "Claude",
                  .origin              = "https://claude.ai",
                  .session_target      = "claude.ai",
                  .default_model       = std::string(models::default_model("claude")),
                  .models              = models::list("claude"),
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
            open(session::match const& match, std::string const& model, open_options const& options) override;

            std::string org_uuid(std::vector<std::pair<std::string, std::string>> const& headers,
                                 session::match const&                                   match);

            json create_chat(std::vector<std::pair<std::string, std::string>> const& headers,
                             std::string const&                                      org,
                             std::string const&                                      prompt);
            json fetch_chat(std::vector<std::pair<std::string, std::string>> const& headers,
                            std::string const&                                      org,
                            std::string const&                                      chat);
            json upload(std::vector<std::pair<std::string, std::string>> const& headers,
                        std::string const&                                      org,
                        std::string const&                                      chat,
                        attachment const&                                       file);

            [[nodiscard]] std::string const& origin() const {
                return info_.origin;
            }

          private:
            provider_info info_;
            std::string   org_;
        };

        std::vector<std::pair<std::string, std::string>> base_headers(
          session::match const& match,
          std::string const&    origin) {
            return session::build_headers(
              match,
              {
                {                   "Accept", "application/json"},
                {             "Content-Type", "application/json"},
                {                   "Origin",             origin},
                {                  "Referer",  origin + "/chats"},
                {"anthropic-client-platform",    "web_claude_ai"},
                { "anthropic-client-version",            "1.0.0"},
            });
        }

        std::string claude_provider::org_uuid(std::vector<std::pair<std::string, std::string>> const& headers,
                                              session::match const&                                   match) {
            if (!org_.empty()) {
                return org_;
            }
            if (auto const cookie = session::cookie_value(match, "lastActiveOrg")) {
                std::string const value = trim(*cookie);
                if (value.size() == 36) {
                    org_ = value;
                    return org_;
                }
            }
            http_request request;
            request.url             = info_.origin + std::string(kOrgsPath);
            request.timeout_seconds = 60;
            apply_headers(request, headers);
            json data = request_json(request);
            json orgs = json::array();
            if (data.is_array()) {
                orgs = std::move(data);
            } else if (data.is_object() && data.contains("organizations")) {
                orgs = data["organizations"];
            }
            json chosen;
            for (auto const& org : orgs) {
                if (!org.is_object() || !org.contains("uuid") || !org["uuid"].is_string()) {
                    continue;
                }
                if (chosen.is_null()) {
                    chosen = org;
                }
                if (org.contains("capabilities") && org["capabilities"].is_array()) {
                    for (auto const& capability : org["capabilities"]) {
                        if (capability.is_string() && capability.get<std::string>() == "chat") {
                            chosen = org;
                            break;
                        }
                    }
                }
            }
            if (chosen.is_null() || !chosen.contains("uuid")) {
                throw std::runtime_error("claude.ai returned no organization for this session");
            }
            org_ = chosen["uuid"].get<std::string>();
            return org_;
        }

        json claude_provider::create_chat(
          std::vector<std::pair<std::string, std::string>> const& headers,
          std::string const&                                      org,
          std::string const&                                      prompt) {
            http_request request;
            request.url       = info_.origin + replace_all(std::string(kChatsPath), "{org}", org);
            request.method    = "POST";
            request.json_body = json{
              {"uuid",uuid4()                      },
              {"name",
               title_of(prompt)}
            }.dump();
            request.timeout_seconds = 60;
            apply_headers(request, headers);
            json data = request_json(request);
            if (data.is_array()) {
                data = data.empty() ? json() : data[0];
            }
            if (!data.is_object() || !data.contains("uuid") || !data["uuid"].is_string()) {
                throw std::runtime_error("could not create a Claude chat: " + data.dump());
            }
            return data;
        }

        json claude_provider::fetch_chat(
          std::vector<std::pair<std::string, std::string>> const& headers,
          std::string const&                                      org,
          std::string const&                                      chat) {
            std::string path = replace_all(std::string(kChatPath), "{org}", org);
            path             = replace_all(std::move(path), "{chat}", chat);
            http_request request;
            request.url             = info_.origin + path;
            request.timeout_seconds = 60;
            apply_headers(request, headers);
            json data = request_json(request);
            if (!data.is_object()) {
                throw std::runtime_error("could not read Claude chat " + chat + ": " + data.dump());
            }
            return data;
        }

        json claude_provider::upload(
          std::vector<std::pair<std::string, std::string>> const& headers,
          std::string const&                                      org,
          std::string const&                                      chat,
          attachment const&                                       file) {
            std::string path = replace_all(std::string(kUploadPath), "{org}", org);
            path             = replace_all(std::move(path), "{chat}", chat);
            std::vector<multipart_field> fields{
              {"file", file.data, file.name, file.mime}
            };
            std::string       content_type;
            std::string const body = encode_multipart(fields, content_type);
            http_request      request;
            request.url             = info_.origin + path;
            request.method          = "POST";
            request.body            = body;
            request.content_type    = content_type;
            request.timeout_seconds = 300;
            apply_headers(request, headers);
            for (auto& header : request.headers) {
                if (header.first == "Content-Type") {
                    header.second = content_type;
                }
            }
            json data = request_json(request);
            if (!data.is_object() || !data.contains("file_uuid")) {
                throw std::runtime_error("upload failed: " + data.dump());
            }
            return data;
        }

        class claude_conversation : public conversation {
          public:
            claude_conversation(claude_provider&    owner,
                                session::match      match,
                                std::string         model,
                                open_options const& options)
              : conversation(std::move(match), std::move(model), options),
                owner_(owner),
                headers_(base_headers(match_, owner_.origin())) {
                std::string const requested = model_.empty() ? owner_.info().default_model : model_;
                model_preference_           = model_alias(requested);
                if (options.resume.is_object() && options.resume.contains("parent_message_uuid") &&
                    options.resume["parent_message_uuid"].is_string())
                {
                    parent_uuid_ = options.resume["parent_message_uuid"].get<std::string>();
                }
            }

            void send(std::string const&              prompt,
                      std::vector<std::string> const& files,
                      event_sink const&               sink) override {
                bool has_cookie = false;
                for (auto const& header : headers_) {
                    if (header.first == "Cookie" && !header.second.empty()) {
                        has_cookie = true;
                    }
                }
                if (!has_cookie) {
                    sink(error("no signed-in session found for claude.ai; log in once in a browser"));
                    return;
                }
                auto headers = headers_;
                for (auto& header : headers) {
                    if (header.first == "Accept") {
                        header.second = "text/event-stream";
                    }
                }
                std::string       org;
                json              created;
                std::vector<json> attachments;
                try {
                    org     = owner_.org_uuid(headers, match_);
                    created = ensure_chat(headers, org, prompt);
                    std::vector<attachment> uploaded;
                    try {
                        uploaded = read_attachments(files);
                    } catch (file_error const& failure) {
                        sink(error(failure.what()));
                        return;
                    }
                    for (auto const& file : uploaded) {
                        json record = owner_.upload(headers, org, conversation_id_, file);
                        attachments.push_back(attachment_entry(record, file));
                        sink(tool_call("upload", "done", file.name));
                    }
                } catch (std::exception const& failure) {
                    sink(error(friendly(failure.what())));
                    return;
                }

                json        body = request_body(prompt, created, attachments);
                std::string path = replace_all(std::string(kCompletionPath), "{org}", org);
                path             = replace_all(std::move(path), "{chat}", conversation_id_);
                http_request request;
                request.url             = owner_.origin() + path;
                request.method          = "POST";
                request.json_body       = body.dump();
                request.timeout_seconds = timeout_;
                request.debug           = debug_;
                apply_headers(request, headers);

                std::string answer;
                std::string thinking_text;
                std::string stop_reason;
                std::string error_text;
                sse_decoder decoder;
                try {
                    std::ignore = stream_lines(request, [&](std::string_view line) {
                        for (auto const& payload : decoder.feed(line)) {
                            json event = json::parse(payload, nullptr, false);
                            if (event.is_discarded() || !event.is_object()) {
                                sink(raw(payload));
                                continue;
                            }
                            feed(event, answer, thinking_text, sink);
                            std::string const kind = event.value("type", "");
                            if (kind == "error") {
                                error_text = error_message(event);
                                return;
                            }
                            if (kind == "message_stop") {
                                return;
                            }
                            if (kind == "message_delta" && event.contains("delta") && event["delta"].is_object()) {
                                auto const& delta = event["delta"];
                                if (delta.contains("stop_reason") && delta["stop_reason"].is_string()) {
                                    stop_reason = delta["stop_reason"].get<std::string>();
                                }
                            }
                            if (
                              kind == "completion" && event.contains("stop_reason") && event["stop_reason"].is_string())
                            {
                                stop_reason = event["stop_reason"].get<std::string>();
                                return;
                            }
                        }
                    });
                } catch (std::exception const& failure) {
                    sink(error(friendly(failure.what())));
                    return;
                }
                if (!error_text.empty()) {
                    return;
                }
                if (trim(answer).empty()) {
                    sink(error("Claude returned no answer" +
                               (conversation_id_.empty() ? std::string{} : " (chat " + conversation_id_ + ")")));
                    return;
                }
                parent_uuid_ = assistant_uuid_.empty() ? parent_uuid_ : assistant_uuid_;
                resume_      = json{
                  {"parent_message_uuid", parent_uuid_},
                  {              "model",       model_}
                };
                ++turns_;
                record("user", prompt);
                record("assistant", answer);
                sink(done());
            }

          private:
            json ensure_chat(std::vector<std::pair<std::string, std::string>> const& headers,
                             std::string const&                                      org,
                             std::string const&                                      prompt) {
                if (!conversation_id_.empty()) {
                    if (!model_preference_ || parent_uuid_.empty()) {
                        json data = owner_.fetch_chat(headers, org, conversation_id_);
                        if (!model_preference_ && data.contains("model") && data["model"].is_string()) {
                            model_ = data["model"].get<std::string>();
                        }
                        if (parent_uuid_.empty()) {
                            if (data.contains("current_leaf_message_uuid") &&
                                data["current_leaf_message_uuid"].is_string())
                            {
                                parent_uuid_ = data["current_leaf_message_uuid"].get<std::string>();
                            } else {
                                parent_uuid_ = kZeroUuid;
                            }
                        }
                    }
                    return json::object();
                }
                json data        = owner_.create_chat(headers, org, prompt);
                conversation_id_ = data["uuid"].get<std::string>();
                if (!model_preference_ && data.contains("model") && data["model"].is_string()) {
                    model_ = data["model"].get<std::string>();
                }
                parent_uuid_ = kZeroUuid;
                if (data.contains("current_leaf_message_uuid") && data["current_leaf_message_uuid"].is_string()) {
                    parent_uuid_ = data["current_leaf_message_uuid"].get<std::string>();
                }
                return data;
            }

            json request_body(std::string const& prompt, json const& created, std::vector<json> const& attachments) {
                std::string query;
                if (!system_.empty() && turns_ == 0) {
                    query = system_;
                }
                bool const replay =
                  created.is_object() && created.contains("uuid") && history_.is_array() && !history_.empty();
                if (replay) {
                    std::string lines =
                      "This is a continuation of the conversation so far. Reply to the final user "
                      "message below.\n\n";
                    std::size_t start = history_.size() > kMaxReplay ? history_.size() - kMaxReplay : 0;
                    for (std::size_t i = start; i < history_.size(); ++i) {
                        auto const& turn = history_[i];
                        if (!turn.is_object()) {
                            continue;
                        }
                        std::string const who  = turn.value("role", "") == "user" ? "User" : "Assistant";
                        lines                 += who + ": " + turn.value("text", "") + "\n";
                    }
                    lines += "\nUser: " + prompt;
                    if (!query.empty()) {
                        query += "\n\n";
                    }
                    query += lines;
                } else {
                    if (!query.empty()) {
                        query += "\n\n";
                    }
                    query += prompt;
                }
                return json{
                  {             "prompt",                                                        query},
                  {"parent_message_uuid", parent_uuid_.empty() ? std::string(kZeroUuid) : parent_uuid_},
                  {           "timezone",                                              timezone_name()},
                  {              "model",                           model_preference_.value_or(model_)},
                  {        "attachments",                                                  attachments},
                  {              "files",                                                json::array()},
                  {     "rendering_mode",                                                   "messages"},
                };
            }

            static json attachment_entry(json const& record, attachment const& file) {
                return json{
                  {"file_uuid", record.value("file_uuid", "")},
                  {"file_name", record.value("file_name", file.name)},
                  {"file_type", record.value("media_type", file.mime)},
                  {"file_size", record.value("size_bytes", static_cast<std::int64_t>(file.size()))},
                  {"extracted_content", ""},
                  {"path", record.value("path", "")},
                  {"file_kind", record.value("file_kind", "blob")},
                };
            }

            void feed(json const& event, std::string& answer, std::string& thinking_text, event_sink const& sink) {
                std::string const kind = event.value("type", "");
                if (kind == "message_start") {
                    if (event.contains("message") && event["message"].is_object()) {
                        auto const& message = event["message"];
                        if (message.contains("uuid") && message["uuid"].is_string()) {
                            assistant_uuid_ = message["uuid"].get<std::string>();
                        }
                        if (message.contains("model") && message["model"].is_string()) {
                            model_ = message["model"].get<std::string>();
                        }
                    }
                    return;
                }
                if (kind == "content_block_start") {
                    if (event.contains("content_block") && event["content_block"].is_object()) {
                        auto const& block = event["content_block"];
                        if (block.value("type", "") == "tool_use") {
                            sink(tool_call(block.value("name", "tool"),
                                           "start",
                                           block.contains("input") ? block["input"].dump() : std::string("{}")));
                        }
                    }
                    return;
                }
                if (kind == "content_block_delta") {
                    if (!event.contains("delta") || !event["delta"].is_object()) {
                        return;
                    }
                    auto const&       delta      = event["delta"];
                    std::string const delta_type = delta.value("type", "");
                    if (delta_type == "text_delta") {
                        std::string const value = delta.value("text", "");
                        if (!value.empty()) {
                            answer += value;
                            sink(text(value));
                        }
                    } else if (delta_type == "thinking_delta") {
                        std::string const value = delta.value("thinking", "");
                        if (!value.empty()) {
                            thinking_text += value;
                            sink(thinking(value));
                        }
                    }
                    return;
                }
                if (kind == "completion") {
                    std::string const value = event.value("completion", "");
                    if (!value.empty()) {
                        answer += value;
                        sink(text(value));
                    }
                    return;
                }
                if (kind == "error") {
                    sink(error(error_message(event)));
                }
            }

            claude_provider&                                 owner_;
            std::vector<std::pair<std::string, std::string>> headers_;
            std::optional<std::string>                       model_preference_;
            std::string                                      parent_uuid_;
            std::string                                      assistant_uuid_;
        };

        std::unique_ptr<conversation>
        claude_provider::open(session::match const& match, std::string const& model, open_options const& options) {
            return std::make_unique<claude_conversation>(*this, match, model, options);
        }

    } // namespace

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization)
    bool const claude_registered = register_provider(std::make_unique<claude_provider>());

} // namespace dana::ai
