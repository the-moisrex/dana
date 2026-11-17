#include "../diagnostics.hpp"
#include "../files.hpp"
#include "../http.hpp"
#include "../models.hpp"
#include "../uuid.hpp"
#include "../ws.hpp"
#include "provider.hpp"
#include "registry.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace dana::ai {

    namespace {

        using nlohmann::json;

        constexpr std::string_view kWsUrl      = "wss://grok.com/ws/mgw/?uid={uid}";
        constexpr std::string_view kUploadPath = "/rest/app-chat/upload-file";

        //: How many transcript entries to replay when a conversation is resumed from a
        //: new process (the socket cannot be reattached).
        constexpr std::size_t kMaxReplay = 20;

        //: Whole-turn attempts when the socket drops before any text was emitted.
        constexpr int kTurnRetries = 2;

        std::string replace_all(std::string text, std::string_view needle, std::string_view replacement) {
            std::size_t position = 0;
            while ((position = text.find(needle, position)) != std::string::npos) {
                text.replace(position, needle.size(), replacement);
                position += replacement.size();
            }
            return text;
        }

        std::string lower(std::string text) {
            for (char& c : text) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return text;
        }

        std::size_t codepoint_length(std::string_view text) {
            std::size_t count = 0;
            for (char byte : text) {
                if ((static_cast<unsigned char>(byte) & 0xC0U) != 0x80U) {
                    ++count;
                }
            }
            return count;
        }

        // Python slices strings by code point; keep the same cut so a multi-byte
        // answer never ends in a broken UTF-8 sequence.
        std::string head_codepoints(std::string_view text, std::size_t limit) {
            if (codepoint_length(text) <= limit) {
                return std::string(text);
            }
            std::size_t index = 0;
            std::size_t seen  = 0;
            while (index < text.size() && seen < limit) {
                ++index;
                while (index < text.size() && (static_cast<unsigned char>(text[index]) & 0xC0U) == 0x80U) {
                    ++index;
                }
                ++seen;
            }
            return std::string(text.substr(0, index));
        }

        json object_or(json const& value, std::string_view key) {
            if (!value.is_object() || !value.contains(key)) {
                return json::object();
            }
            auto const& found = value[std::string(key)];
            return found.is_object() ? found : json::object();
        }

        json array_or(json const& value, std::string_view key) {
            if (!value.is_object() || !value.contains(key)) {
                return json::array();
            }
            auto const& found = value[std::string(key)];
            return found.is_array() ? found : json::array();
        }

        std::string argument_text(json const& value) {
            if (value.is_string()) {
                return value.get<std::string>();
            }
            if (value.is_null()) {
                return {};
            }
            return value.dump();
        }

        // The capability block the web client advertises in `session.create`.
        json capabilities() {
            return json{
              {    "protocol_capabilities",
               json::array({"conversation_attached", "custom_methods_v1", "workspace_servers_v1"})},
              {                "use_chunk",                                                   true},
              {"accept_interleaved_phases",                                                   true},
              {     "client_side_toolsets",                         json::array({"connectors-v3"})},
              {      "enable_side_by_side",                                                   true},
              {       "force_side_by_side",                                                  false},
              {  "enable_image_generation",                                                   true},
              {   "image_generation_count",                                                      2},
              {  "disable_text_follow_ups",                                                  false},
              {         "disable_artifact",                                                   true},
              {            "force_concise",                                                  false},
            };
        }

        // The persistent `uid` the site uses for the socket (best effort).  Grok keeps
        // it in the workspace's marketing-prompt key; `__cuid` is a close second.  The
        // `grok_device_id` cookie is *not* the socket id — the server drops the
        // connection when it is sent as `uid`.
        std::string client_id(session::match const& match) {
            auto const& storage = session::localstorage(match);
            if (storage.is_object()) {
                for (auto const& item : storage.items()) {
                    std::string const& key = item.key();
                    if (!key.starts_with("dismissed:marketing-opt-in-prompt:")) {
                        continue;
                    }
                    auto const colon = key.find_last_of(':');
                    if (colon != std::string::npos && colon + 1 < key.size()) {
                        return key.substr(colon + 1);
                    }
                }
            }
            if (auto const value = session::storage_value(match, {"__cuid"}); value && !value->empty()) {
                return *value;
            }
            return uuid4();
        }

        std::string error_text(json const& event) {
            if (event.contains("error")) {
                auto const& value = event["error"];
                if (value.is_string() && !value.get<std::string>().empty()) {
                    return head_codepoints(value.get<std::string>(), 300);
                }
                if (value.is_object() && !value.empty()) {
                    return head_codepoints(value.dump(), 300);
                }
            }
            return head_codepoints(event.dump(), 300);
        }

        std::string tool_name(json const& call) {
            if (call.contains("function") && call["function"].is_object()) {
                auto const& function = call["function"];
                if (function.contains("name") && function["name"].is_string() &&
                    !function["name"].get<std::string>().empty())
                {
                    return function["name"].get<std::string>();
                }
            }
            if (call.contains("name") && !call["name"].is_null()) {
                std::string const name = argument_text(call["name"]);
                if (!name.empty()) {
                    return name;
                }
            }
            return "tool";
        }

        // Channels the site stamps on a `response.chunk`; anything unrecognised is
        // shown as reasoning rather than leaking into the answer.
        stream_event chunk_event(std::string const& channel, std::string const& body) {
            if (channel.contains("RESPONSE")) {
                return text(body);
            }
            static constexpr std::array<std::string_view, 5>
              kToolChannels{"TOOL", "SEARCH", "BROWSE", "CODE", "WIDGET"};
            for (auto const token : kToolChannels) {
                if (channel.contains(token)) {
                    return tool_call(lower(replace_all(channel, "CHANNEL_", "")), "delta", body);
                }
            }
            // CHANNEL_ASSISTANT_NOTETAKER_HEADER and any thinking/reasoning channel.
            return thinking(body);
        }

        class grok_provider : public provider {
          public:
            grok_provider()
              : info_{
                  .name                = "grok",
                  .label               = "Grok",
                  .origin              = "https://grok.com",
                  .session_target      = "grok.com",
                  .default_model       = std::string(models::default_model("grok")),
                  .models              = models::list("grok"),
                  .inline_limit        = 64UL * 1024UL,
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

            [[nodiscard]] std::string upload(session::match const& match, attachment const& file) const;

            [[nodiscard]] std::vector<std::pair<std::string, std::string>> rest_headers(
              session::match const& match) const {
                return session::build_headers(
                  match,
                  {
                    {      "Accept",              "*/*"},
                    {"Content-Type", "application/json"},
                    {     "Referer", info_.origin + "/"},
                    {      "Origin",       info_.origin},
                });
            }

            [[nodiscard]] static std::vector<std::pair<std::string, std::string>> socket_headers(
              session::match const& match) {
                std::vector<std::pair<std::string, std::string>> headers;
                if (!match.user_agent.empty()) {
                    headers.emplace_back("User-Agent", match.user_agent);
                }
                if (!match.cookie_header.empty()) {
                    headers.emplace_back("Cookie", match.cookie_header);
                }
                return headers;
            }

            [[nodiscard]] std::string const& origin() const {
                return info_.origin;
            }

          private:
            provider_info info_;
        };

        std::string grok_provider::upload(session::match const& match, attachment const& file) const {
            json const payload{
              {    "fileName",                 file.name},
              {"fileMimeType",                 file.mime},
              {     "content",  base64_encode(file.data)},
              {  "makePublic",                     false},
              {  "fileSource", "SELF_UPLOAD_FILE_SOURCE"},
            };
            http_request request;
            request.url             = info_.origin + std::string(kUploadPath);
            request.method          = "POST";
            request.json_body       = payload.dump();
            request.timeout_seconds = 300;
            for (auto const& header : rest_headers(match)) {
                request.headers.push_back(header);
            }
            json const data = request_json(request);
            if (!data.is_object()) {
                throw std::runtime_error("upload failed: " + data.dump());
            }
            std::string const id = data.value("fileMetadataId", "");
            if (id.empty()) {
                throw std::runtime_error("upload failed: " + data.dump());
            }
            return id;
        }

        class grok_conversation : public conversation {
          public:
            grok_conversation(grok_provider&      owner,
                              session::match      match,
                              std::string         model,
                              open_options const& options)
              : conversation(std::move(match), std::move(model), options),
                owner_(owner),
                replay_(history_.is_array() && !history_.empty()) {
                if (model_.empty()) {
                    model_ = owner_.info().default_model;
                }
            }

            void send(std::string const&              prompt,
                      std::vector<std::string> const& files,
                      event_sink const&               sink) override {
                if (match_.cookie_header.empty()) {
                    sink(error("no cookies in the Grok session; log in to grok.com"));
                    return;
                }
                std::vector<attachment> attachments;
                try {
                    attachments = read_attachments(files);
                } catch (file_error const& failure) {
                    sink(error(failure.what()));
                    return;
                }
                std::vector<std::string> file_ids;
                for (auto const& file : attachments) {
                    try {
                        file_ids.push_back(owner_.upload(match_, file));
                    } catch (std::exception const& failure) {
                        sink(error(failure.what()));
                        return;
                    }
                    sink(tool_call("upload", "done", file.name));
                }

                std::string const content = content_for(prompt);
                replay_                   = false;

                std::string answer;
                for (int attempt = 0;; ++attempt) {
                    bool emitted  = false;
                    auto tracking = [&emitted, &sink](stream_event const& event) {
                        emitted = true;
                        sink(event);
                    };
                    try {
                        if (!socket_.connected()) {
                            socket_.set_debug(debug_);
                            std::string url(kWsUrl);
                            auto const  marker = url.find("{uid}");
                            if (marker != std::string::npos) {
                                url.replace(marker, 5, client_id(match_));
                            }
                            socket_.connect(url, grok_provider::socket_headers(match_), owner_.origin(), timeout_);
                            session_id_ = create_session();
                        }
                        create_response(content, file_ids);
                        read(tracking, answer);
                        break;
                    } catch (std::exception const& failure) {
                        close();
                        // A dropped socket is safe to replay only while nothing reached
                        // the caller; once a chunk was emitted, a retry would duplicate it.
                        if (emitted || attempt + 1 >= kTurnRetries || !is_network_failure(failure.what())) {
                            sink(error("Grok stream failed: " + std::string(failure.what())));
                            return;
                        }
                        warn("Grok connection dropped (" + std::string(failure.what()) + "); retrying", "ai");
                    }
                }
                ++turns_;
                if (!session_id_.empty()) {
                    conversation_id_ = session_id_;
                }
                record("user", prompt);
                record("assistant", answer);
            }

            void close() override {
                socket_.close();
            }

          private:
            [[nodiscard]] std::string content_for(std::string const& prompt) const {
                std::string query;
                if (turns_ == 0 && !system_.empty()) {
                    query = system_;
                }
                if (replay_ && history_.is_array() && !history_.empty()) {
                    std::string lines =
                      "This is a continuation of the conversation so far. Reply to the final user message below.\n\n";
                    std::size_t const start = history_.size() > kMaxReplay ? history_.size() - kMaxReplay : 0;
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
                return query;
            }

            void send(json const& payload) {
                socket_.send_text(payload.dump());
            }

            std::string create_session() {
                json const event{
                  {    "type",                                "session.create"},
                  {"event_id",                           "evt_init_" + uuid4()},
                  { "session", {{"model", model_}, {"x_grok", capabilities()}}},
                };
                send(json{
                  {"event", event}
                });

                auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
                for (;;) {
                    auto const left =
                      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
                        .count();
                    if (left <= 0) {
                        break;
                    }
                    auto message = socket_.receive(static_cast<int>(left));
                    if (!message) {
                        continue;
                    }
                    if (message->close) {
                        throw ws_error("the Grok websocket closed during the session handshake");
                    }
                    json const frame = json::parse(message->data, nullptr, false);
                    if (frame.is_discarded() || !frame.is_object()) {
                        continue;
                    }
                    json const        event_frame = object_or(frame, "event");
                    std::string const kind        = event_frame.value("type", "");
                    if (kind == "session.created") {
                        std::string const id = frame.value("session_id", "");
                        if (!id.empty()) {
                            return id;
                        }
                    } else if (kind == "error" || kind == "session.error") {
                        throw std::runtime_error(error_text(event_frame));
                    }
                }
                throw std::runtime_error("the site never created a websocket session");
            }

            void create_response(std::string const& prompt, std::vector<std::string> const& file_ids) {
                auto const millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count();
                json       item{
                  {"type", "message"},
                  {"role",    "user"},
                };
                item["x_grok"] = json{
                  {"client_message_id",                                           uuid4()},
                  {     "input_chunks", json::array({json{{"text", {{"text", prompt}}}}})},
                };
                json event{
                  {    "type",                    "response.create"},
                  {"event_id", "evt_resp_" + std::to_string(millis)},
                  {    "item",                                 item},
                };
                if (!file_ids.empty()) {
                    event["file_attachment_ids"] = file_ids;
                }
                send(json{
                  {"session_id", session_id_},
                  {     "event",       event}
                });
                send(json{
                  {"session_id",                                      session_id_},
                  {     "event", {{"type", "presence.update"}, {"visible", true}}}
                });
            }

            void read(event_sink const& sink, std::string& answer) {
                int const wait = static_cast<int>(std::min<long>(timeout_, 600) * 1000);
                for (;;) {
                    auto message = socket_.receive(wait);
                    if (!message) {
                        throw ws_error("timed out waiting for the Grok stream");
                    }
                    if (message->close) {
                        throw ws_error("the Grok websocket closed before the answer finished");
                    }
                    json const frame = json::parse(message->data, nullptr, false);
                    if (frame.is_discarded() || !frame.is_object()) {
                        sink(raw(message->data));
                        continue;
                    }
                    json const        event = object_or(frame, "event");
                    std::string const kind  = event.value("type", "");
                    if (kind == "error" || kind == "session.error") {
                        sink(error(error_text(event)));
                        return;
                    }
                    if (kind == "response.chunk") {
                        json const        chunk = object_or(event, "chunk");
                        json const        text  = object_or(chunk, "text");
                        std::string const body  = text.value("text", "");
                        if (!body.empty()) {
                            stream_event const chunked = chunk_event(text.value("channel", ""), body);
                            if (chunked.kind == event_kind::text) {
                                answer += body;
                            }
                            sink(chunked);
                        }
                        for (auto const& call : array_or(chunk, "tool_calls")) {
                            if (!call.is_object()) {
                                continue;
                            }
                            json const arguments = call.contains("arguments") ? call["arguments"] : json();
                            sink(tool_call(tool_name(call), "delta", head_codepoints(argument_text(arguments), 400)));
                        }
                        continue;
                    }
                    if (kind == "response.output_item.added") {
                        json const        item = object_or(event, "item");
                        std::string const name = item.value("name", "");
                        std::string const type = item.value("type", "");
                        if (!name.empty() || type == "function_call" || type == "tool_call") {
                            sink(tool_call(name.empty() ? "tool" : name, "start"));
                        }
                        continue;
                    }
                    if (kind == "response.grok.output") {
                        json const output = object_or(event, "output");
                        if (output.contains("stream_error") && output["stream_error"].is_object() &&
                            !output["stream_error"].empty())
                        {
                            auto const& problem = output["stream_error"];
                            sink(error(problem.value("message", "the model reported a stream error")));
                            return;
                        }
                        continue;
                    }
                    if (kind == "response.done") {
                        json const response = object_or(event, "response");
                        if (response.contains("usage") && response["usage"].is_object() && !response["usage"].empty()) {
                            sink(usage(response["usage"]));
                        }
                        sink(done());
                        return;
                    }
                    if (kind == "raw") {
                        sink(raw(event.value("raw", "")));
                    }
                }
            }

            grok_provider& owner_;
            websocket      socket_;
            std::string    session_id_;
            bool           replay_ = false;
        };

        std::unique_ptr<conversation>
        grok_provider::open(session::match const& match, std::string const& model, open_options const& options) {
            return std::make_unique<grok_conversation>(*this, match, model, options);
        }

    } // namespace


} // namespace dana::ai

[[maybe_unused]] bool const grok_registered = dana::ai::register_provider(std::make_unique<dana::ai::grok_provider>());
