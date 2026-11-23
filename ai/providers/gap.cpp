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
#include <map>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace dana::ai {

    namespace {

        using nlohmann::json;

        constexpr std::string_view kWsPath       = "/ws/salam";
        constexpr int              kEventVersion = 98;
        constexpr std::string_view kChatPath     = "/api/v1/get_chat/token/{token}";
        constexpr std::string_view kUploadPath   = "/api/v1/upload_file";

        /// The site's edge (ArvanCloud) intermittently stalls the upgrade, so the
        /// connect plus the first frame are retried and every wait has its own bound.
        constexpr double                kHeartbeatSeconds  = 12.0;
        constexpr int                   kRecvTimeoutMs     = 2000;
        constexpr double                kPollAfterSeconds  = 4.0;
        constexpr double                kAckTimeoutSeconds = 90.0;
        constexpr int                   kConnectAttempts   = 3;
        constexpr std::array<double, 2> kConnectBackoff{0.5, 1.5};

        /// Fragment types that carry the model's reasoning rather than the answer.
        constexpr std::array<std::string_view, 4> kThinkingTypes{"reasoning", "agent_turn", "thinking", "thoughts"};
        constexpr std::array<std::string_view, 1> kSkipTypes{"text-loading"};

        /// `message.extra_display.status` / `message.status` values that end a turn.
        constexpr std::array<std::string_view, 2> kDoneStatus{"completed", "stopped"};
        constexpr std::array<std::string_view, 2> kFailedStatus{"error", "failed"};

        std::string trim(std::string_view value) {
            auto const first = value.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos) {
                return {};
            }
            auto const last = value.find_last_not_of(" \t\r\n");
            return std::string(value.substr(first, last - first + 1));
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

        bool is_valid_utf8(std::string_view text) {
            std::size_t index = 0;
            while (index < text.size()) {
                auto const  lead  = static_cast<unsigned char>(text[index]);
                std::size_t extra = 0;
                if (lead < 0x80U) {
                    extra = 0;
                } else if ((lead & 0xE0U) == 0xC0U) {
                    extra = 1;
                } else if ((lead & 0xF0U) == 0xE0U) {
                    extra = 2;
                } else if ((lead & 0xF8U) == 0xF0U) {
                    extra = 3;
                } else {
                    return false;
                }
                if (extra > 0 && index + extra >= text.size()) {
                    return false;
                }
                for (std::size_t offset = 1; offset <= extra; ++offset) {
                    if ((static_cast<unsigned char>(text[index + offset]) & 0xC0U) != 0x80U) {
                        return false;
                    }
                }
                index += extra + 1;
            }
            return true;
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

        std::string error_message(json const& value, std::string_view fallback) {
            if (value.is_string() && !value.get<std::string>().empty()) {
                return head_codepoints(value.get<std::string>(), 300);
            }
            if (value.is_object() && !value.empty()) {
                return head_codepoints(value.dump(), 300);
            }
            return std::string(fallback);
        }

        bool contains(std::array<std::string_view, 2> const& values, std::string_view text) {
            return std::ranges::any_of(values, [text](std::string_view value) {
                return value == text;
            });
        }

        bool contains(std::array<std::string_view, 4> const& values, std::string_view text) {
            return std::ranges::any_of(values, [text](std::string_view value) {
                return value == text;
            });
        }

        bool contains(std::array<std::string_view, 1> const& values, std::string_view text) {
            return std::ranges::any_of(values, [text](std::string_view value) {
                return value == text;
            });
        }

        bool status_in(json const& message, std::array<std::string_view, 2> const& statuses) {
            if (!message.is_object()) {
                return false;
            }
            if (message.contains("extra_display") && message["extra_display"].is_object()) {
                auto const& display = message["extra_display"];
                if (display.contains("status") && display["status"].is_string() &&
                    contains(statuses, display["status"].get<std::string>()))
                {
                    return true;
                }
            }
            return message.contains("status") && message["status"].is_string() &&
                   contains(statuses, message["status"].get<std::string>());
        }

        bool completed(json const& message) {
            return status_in(message, kDoneStatus);
        }

        bool failed(json const& message) {
            return status_in(message, kFailedStatus);
        }

        json object_or(json const& value, std::string_view key) {
            if (!value.is_object() || !value.contains(key)) {
                return json::object();
            }
            auto const& found = value[std::string(key)];
            return found.is_object() ? found : json::object();
        }

        // The site's ids and statuses are usually strings but may arrive as numbers or
        // null; never let a shape change throw out of a turn.
        std::string text_of(json const& value, std::string_view key) {
            if (!value.is_object() || !value.contains(key)) {
                return {};
            }
            auto const& found = value[std::string(key)];
            if (found.is_string()) {
                return found.get<std::string>();
            }
            if (found.is_number_integer()) {
                return std::to_string(found.get<std::int64_t>());
            }
            if (found.is_number_unsigned()) {
                return std::to_string(found.get<std::uint64_t>());
            }
            return {};
        }

        /// Friendly labels accepted by `--model` (the site's own ids are also accepted).
        std::string alias_model(std::string_view requested) {
            std::string key   = trim(requested);
            std::string upper = key;
            std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) {
                return static_cast<char>(std::toupper(c));
            });
            if (upper.starts_with("A-")) {
                return upper;
            }
            std::string slug = lower(key);
            std::replace(slug.begin(), slug.end(), ' ', '-');
            if (auto const alias = models::resolve_alias("gap", slug)) {
                return std::string(*alias);
            }
            return key;
        }

        class gap_provider : public provider {
          public:
            gap_provider()
              : info_{
                  .name                = "gap",
                  .label               = "GapGPT",
                  .origin              = "https://gapgpt.ir",
                  .session_target      = "gapgpt.ir",
                  .default_model       = std::string(models::default_model("gap")),
                  .models              = models::list("gap"),
                  .inline_limit        = 24UL * 1024UL,
                  .needs_session       = true,
                  .api                 = false,
                  .local               = false,
                  .recordable          = true,
                  .prompt_file_upload  = true,
                  // GapGPT tries the prompt inline first and falls back to upload
                  // on token_limit, so the CLI must not pre-attach by size.
                  .auto_attach_by_size = false,
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

            std::string resolve_model(std::string_view requested, std::string_view /*base*/) override {
                if (trim(requested).empty()) {
                    return info_.default_model;
                }
                return alias_model(requested);
            }

            std::string upload(session::match const& match, attachment const& file);

            [[nodiscard]] std::vector<std::pair<std::string, std::string>> history_headers(
              session::match const& match) const {
                auto headers = session::build_headers(
                  match,
                  {
                    { "Accept", "application/json, text/plain, */*"},
                    {"Referer",                  info_.origin + "/"},
                });
                if (auto const token = session::cookie_value(match, "accessToken"); token && !token->empty()) {
                    headers.emplace_back("Authorization", "Bearer " + *token);
                }
                return headers;
            }

            [[nodiscard]] std::string ws_url() const {
                auto const        scheme = info_.origin.find("://");
                std::string const host   = scheme == std::string::npos ? info_.origin : info_.origin.substr(scheme + 3);
                return "wss://" + host + std::string(kWsPath);
            }

            [[nodiscard]] static std::string status_error(std::string_view status) {
                static std::map<std::string_view, std::string_view> const hints{
                  {"unauthorized", "the session is not signed in; open gapgpt.app in a browser"},
                  {"free_sub_upgrade", "this model needs a paid plan"},
                  {"sub_upgrade", "this model needs a higher plan"},
                  {"invalid_request", "the model name was not accepted; see --model"},
                  {
                   "token_limit", "the prompt is longer than the model's context window; re-run with --prompt-file to upload it as a "
                    "text file, lower --prompt-limit, or shorten/split the prompt", },
                  {"cancelled", "the request was cancelled"},
                };
                auto const message = std::format("GapGPT refused the message ({})", status);
                auto const it      = hints.find(status);
                return it == hints.end() ? message : message + ": " + std::string(it->second);
            }

            [[nodiscard]] std::string_view origin() const {
                return info_.origin;
            }

          private:
            provider_info info_;
        };

        std::string gap_provider::upload(session::match const& match, attachment const& file) {
            auto headers = history_headers(match);
            headers.emplace_back("Origin", info_.origin);
            std::vector<multipart_field> const fields{
              {
               .name         = "file",
               .value        = file.data,
               .filename     = file.name,
               .content_type = file.mime,
               },
            };
            std::string       content_type;
            std::string const body = encode_multipart(fields, content_type);

            http_request request;
            request.url             = info_.origin + std::string(kUploadPath);
            request.method          = "POST";
            request.body            = body;
            request.content_type    = content_type;
            request.timeout_seconds = 300;
            request.headers         = std::move(headers);

            json data;
            for (int attempt = 0; attempt < 3; ++attempt) {
                try {
                    data = request_json(request);
                    break;
                } catch (std::exception const& failure) {
                    if (attempt + 1 >= 3 || !is_transient(failure.what())) {
                        throw;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(500 * (attempt + 1)));
                }
            }
            if (!data.is_object()) {
                throw std::runtime_error("upload failed: " + data.dump());
            }
            if (data.contains("file_token")) {
                auto const& token = data["file_token"];
                if (token.is_string() && !token.get<std::string>().empty()) {
                    return token.get<std::string>();
                }
                if (token.is_array() && !token.empty() && token[0].is_string()) {
                    return token[0].get<std::string>();
                }
            }
            throw std::runtime_error("upload failed: " + data.dump());
        }

        class gap_conversation : public conversation {
          public:
            gap_conversation(gap_provider&       owner,
                             session::match      match,
                             std::string_view    model,
                             open_options const& options)
              : conversation(std::move(match), model, options),
                owner_(owner),
                headers_(owner_.history_headers(match_)) {
                if (model_.empty()) {
                    model_ = owner_.info().default_model;
                }
                if (resume_.is_object() && resume_.contains("title") && resume_["title"].is_string()) {
                    title_ = resume_["title"].get<std::string>();
                }
            }

            void send(std::string_view prompt, text::string_views files, event_sink const& sink) override {
                if (match_.cookie_header.empty()) {
                    sink(error("no cookies in the GapGPT session; log in to gapgpt.app"));
                    return;
                }
                std::vector<attachment> attachments;
                try {
                    attachments = read_attachments(files);
                } catch (file_error const& failure) {
                    sink(error(failure.what()));
                    return;
                }

                std::string const base_text =
                  (turns_ == 0 && !system_.empty()) ? system_ + "\n\n" + std::string(prompt) : std::string(prompt);
                std::vector<std::string> attempts;

                // Attempt 1: inline everything.
                std::string inline_text;
                bool        inline_ok = true;
                for (auto const& file : attachments) {
                    if (!is_valid_utf8(file.data)) {
                        inline_ok = false;
                        break;
                    }
                    inline_text += "\n\n" + file.data;
                }
                std::string reason;
                if (inline_ok) {
                    auto const result = attempt(base_text + inline_text, {}, sink);
                    if (result.ok) {
                        finish_turn(prompt, result.answer);
                        sink(done());
                        return;
                    }
                    if (!result.answer.empty()) [[unlikely]] {
                        sink(error("GapGPT stream failed after partial answer (" +
                                   (result.reason.empty() ? std::string("error") : result.reason) +
                                   "); not retrying to avoid duplicating output"));
                        return;
                    }
                    reason = result.reason;
                    attempts.push_back("inline: " + (reason.empty() ? std::string("error") : reason));
                }

                if (attachments.empty()) {
                    if (reason != "token_limit") {
                        sink(error("GapGPT inline attempt failed (" + (reason.empty() ? std::string("error") : reason) +
                                   "); no attachments to upload as a fallback"));
                        return;
                    }
                    // No --file attachments; the prompt itself was too big for inline.
                    // Synthesize a text attachment, upload it, retry.
                    std::string const        synth_body = base_text + inline_text;
                    std::vector<std::string> file_tokens;
                    try {
                        attachment synth;
                        synth.name = "prompt.txt";
                        synth.data = synth_body;
                        synth.mime = "text/plain";
                        file_tokens.push_back(owner_.upload(match_, synth));
                        sink(tool_call("upload", "done", "prompt.txt (auto)"));
                    } catch (std::exception const& failure) {
                        sink(error("GapGPT rejected the prompt inline (" + join(attempts, "; ") +
                                   ") and could not upload it as a file: " + failure.what()));
                        return;
                    }
                    std::string const pointer =
                      "The complete request is in the attached file `prompt.txt` (" +
                      std::to_string(synth_body.size()) +
                      " bytes). Read it and respond to its contents exactly as if it were this message; this message "
                      "is "
                      "only a pointer to it.";
                    auto result = attempt(pointer, file_tokens, sink);
                    if (result.ok) {
                        finish_turn(prompt, result.answer);
                        sink(done());
                        return;
                    }
                    if (!result.answer.empty()) {
                        sink(error("GapGPT stream failed after partial answer (" +
                                   (result.reason.empty() ? std::string("error") : result.reason) +
                                   "); not retrying to avoid duplicating output"));
                        return;
                    }
                    if (result.reason == "token_limit") {
                        sink(error(
                          "GapGPT could not fit the prompt in the model context window (inline and uploaded-file "
                          "attempts both hit token_limit). Try a different --model with a larger context, or split "
                          "the prompt."));
                    } else {
                        attempts.push_back("upload: " + (result.reason.empty() ? std::string("error") : result.reason));
                        sink(error("GapGPT rejected the prompt after inline + upload (" + join(attempts, "; ") + ")"));
                    }
                    return;
                }

                // Attempt 2: authenticated /api/v1/upload_file -> file_tokens.
                std::vector<std::string> file_tokens;
                std::string              upload_error;
                bool                     upload_failed = false;
                for (auto const& file : attachments) {
                    try {
                        file_tokens.push_back(owner_.upload(match_, file));
                        sink(tool_call("upload", "done", file.name));
                    } catch (std::exception const& failure) {
                        upload_failed = true;
                        upload_error  = failure.what();
                        sink(tool_call("upload", "error", file.name + ": " + failure.what()));
                        break;
                    }
                }
                if (!upload_failed && !file_tokens.empty()) {
                    auto result = attempt(base_text, file_tokens, sink);
                    if (result.ok) {
                        finish_turn(prompt, result.answer);
                        sink(done());
                        return;
                    }
                    if (!result.answer.empty()) {
                        sink(error("GapGPT stream failed after partial answer (" +
                                   (result.reason.empty() ? std::string("error") : result.reason) +
                                   "); not retrying to avoid duplicating output"));
                        return;
                    }
                    attempts.push_back("upload: " + (result.reason.empty() ? std::string("error") : result.reason));
                } else {
                    attempts.push_back("upload: " + (upload_failed ? upload_error : std::string("no file tokens")));
                }

                // The Python adapter falls back to a public termbin URL here; the native
                // port stops at the site's own upload endpoint (see the TODO in the
                // provider's header comment).
                sink(error("GapGPT could not accept the prompt after inline + upload: " + join(attempts, "; ")));
            }

            void close() override {
                close_socket();
            }

          private:
            struct attempt_result {
                bool        ok = false;
                std::string reason;
                std::string answer;
            };

            static std::string join(text::string_views parts, std::string_view separator) {
                std::string text;
                for (std::size_t i = 0; i < parts.size(); ++i) {
                    if (i != 0) {
                        text += separator;
                    }
                    text += parts[i];
                }
                return text;
            }

            void close_socket() {
                socket_.close();
            }

            void ensure() {
                if (socket_.connected()) {
                    return;
                }
                socket_.set_debug(debug_);
                std::vector<std::pair<std::string, std::string>> headers;
                if (!match_.user_agent.empty()) {
                    headers.emplace_back("User-Agent", match_.user_agent);
                }
                if (!match_.cookie_header.empty()) {
                    headers.emplace_back("Cookie", match_.cookie_header);
                }
                socket_.connect(owner_.ws_url(), headers, owner_.origin(), 20);
            }

            std::int64_t send_event(std::string_view event, json const& data) {
                static thread_local std::mt19937 rng{std::random_device{}()};
                auto const                       rid = static_cast<std::int64_t>(rng() % 100'000'000ULL);
                json const                       envelope{
                  {  "event",         event},
                  {    "rid",           rid},
                  {   "data",          data},
                  {"version", kEventVersion}
                };
                socket_.send_text(envelope.dump());
                return rid;
            }

            [[nodiscard]] json action(std::string_view text, text::string_views file_tokens) const {
                json mode_extra = json::object();
                if (model_ == owner_.info().default_model) {
                    mode_extra = json{
                      {               "model_mode",  "auto"},
                      { "model_routing_preference",  "auto"},
                      {"selected_simplified_model", "A-GAP"},
                      {        "routed_chat_model", "A-GAP"},
                    };
                }
                return json{
                  {                     "type",                               "text_message"},
                  {            "message_token",                                           ""},
                  {                 "bot_name",                                       model_},
                  {                  "version",                                            4},
                  {               "chat_token",                             conversation_id_},
                  {                     "text",                            std::string(text)},
                  {              "file_tokens", file_tokens | std::ranges::to<std::vector>()},
                  {          "allow_downgrade",                                        false},
                  {       "enable_user_memory",                                        false},
                  {                     "mode",                                           ""},
                  {               "mode_extra",                                   mode_extra},
                  {               "chat_extra",                               json::object()},
                  {             "is_temporary",                                        false},
                  {"messenger_browser_session",                                      uuid4()},
                };
            }

            std::optional<json> receive() {
                auto message = socket_.receive(kRecvTimeoutMs);
                if (!message) {
                    return std::nullopt;
                }
                if (message->close) {
                    close_socket();
                    return std::nullopt;
                }
                json frame = json::parse(message->data, nullptr, false);
                if (frame.is_discarded() || !frame.is_object()) {
                    note("ignoring a non-JSON websocket frame", debug_, "gap");
                    return {json::object()};
                }
                return {std::move(frame)};
            }

            attempt_result attempt(std::string_view text, text::string_views file_tokens, event_sink const& sink) {
                json const data{
                  {"action", action(text, file_tokens)},
                  {"chat_token", conversation_id_},
                  {"chat_model", model_},
                  {"message_token", ""},
                  {"allow_downgrade", false},
                  {"enable_user_memory", false},
                };

                std::optional<std::int64_t> rid;
                std::string                 failure;
                for (int index = 0; index < kConnectAttempts; ++index) {
                    try {
                        ensure();
                        rid = send_event("new_message", data);
                        break;
                    } catch (std::exception const& connect_failure) {
                        failure = connect_failure.what();
                        close_socket();
                        note("connect attempt " + std::to_string(index + 1) + "/" + std::to_string(kConnectAttempts) +
                               " failed: " + failure,
                             debug_,
                             "gap");
                        if (index + 1 < kConnectAttempts) {
                            std::this_thread::sleep_for(
                              std::chrono::milliseconds(static_cast<std::int64_t>(kConnectBackoff[index] * 1000)));
                        }
                    }
                }
                if (!rid) {
                    sink(error("could not open the GapGPT websocket: " + failure));
                    return {.ok = false, .reason = "websocket", .answer = {}};
                }

                blocks_.clear();
                std::string answer;
                try {
                    auto [ok, reason] = read_turn(*rid, answer, sink);
                    return {.ok = ok, .reason = std::move(reason), .answer = std::move(answer)};
                } catch (std::exception const& stream_failure) {
                    sink(error("GapGPT stream failed: " + std::string(stream_failure.what())));
                    close_socket();
                    return {.ok = false, .reason = "error", .answer = std::move(answer)};
                }
            }

            std::pair<bool, std::string> read_turn(std::int64_t rid, std::string& answer, event_sink const& sink) {
                using clock          = std::chrono::steady_clock;
                auto const started   = clock::now();
                auto       deadline  = started + std::chrono::seconds(timeout_);
                auto       last_beat = started;
                auto       last_seen = started;
                bool       acked     = false;

                while (clock::now() < deadline) {
                    std::optional<json> frame;
                    if (socket_.connected()) {
                        try {
                            frame = receive();
                        } catch (std::exception const& read_failure) {
                            note(std::string("socket read failed: ") + read_failure.what(), debug_, "gap");
                            close_socket();
                        }
                    }
                    auto const now = clock::now();
                    if (!frame) {
                        if (!socket_.connected() && !acked) {
                            sink(error("GapGPT websocket dropped before the message was acknowledged"));
                            return {false, "dropped"};
                        }
                        if (socket_.connected() &&
                            now - last_beat >= std::chrono::seconds(static_cast<long>(kHeartbeatSeconds)))
                        {
                            try {
                                send_event("heartbeat", json::object());
                            } catch (std::exception const& beat_failure) {
                                note(std::string("heartbeat failed: ") + beat_failure.what(), debug_, "gap");
                                close_socket();
                            }
                            last_beat = now;
                        }
                        if (!acked && now - started >= std::chrono::seconds(static_cast<long>(kAckTimeoutSeconds))) {
                            sink(error("GapGPT never acknowledged the message"));
                            return {false, "no_ack"};
                        }
                        if (acked && now - last_seen >= std::chrono::seconds(static_cast<long>(kPollAfterSeconds))) {
                            json snapshot;
                            bool done = false;
                            try {
                                auto polled = poll();
                                snapshot    = std::move(polled.first);
                                done        = polled.second;
                            } catch (std::exception const& poll_failure) {
                                note(std::string("poll failed: ") + poll_failure.what(), debug_, "gap");
                            }
                            last_seen = now;
                            if (!snapshot.is_null()) {
                                bool const progressed = apply(snapshot, answer, sink);
                                if (progressed) {
                                    deadline = now + std::chrono::seconds(timeout_);
                                }
                                if (failed(snapshot)) {
                                    sink(error(
                                      error_message(snapshot.value("error", json()), "the model reported an error")));
                                    return {false, "error"};
                                }
                            }
                            if (done) {
                                return {true, ""};
                            }
                        } else {
                            std::this_thread::sleep_for(std::chrono::milliseconds(socket_.connected() ? 100 : 300));
                        }
                        continue;
                    }

                    json const&       current = *frame;
                    std::string const event   = text_of(current, "event");
                    json const        data    = object_or(current, "data");
                    if (event == "heartbeat_ack" || event == "event_ack") {
                        continue;
                    }

                    if (!acked) {
                        if (event == "ack_new_message" && current.contains("rid") &&
                            current["rid"].is_number_integer() && current["rid"].get<std::int64_t>() == rid)
                        {
                            std::string const status = text_of(data, "status");
                            if (status != "ok") {
                                if (status == "token_limit") {
                                    return {false, "token_limit"};
                                }
                                sink(error(gap_provider::status_error(status)));
                                return {false, "error"};
                            }
                            std::string chat = text_of(data, "chat_token");
                            if (chat.empty()) {
                                chat = text_of(data, "conversation_id");
                            }
                            if (!chat.empty()) {
                                conversation_id_ = chat;
                            }
                            json const message = object_or(data, "message");
                            turn_token_        = text_of(message, "token");
                            acked              = true;
                            last_seen          = clock::now();
                            apply(message, answer, sink);
                        } else if (event == "error" || event == "session.error") {
                            sink(error(head_codepoints(data.empty() ? event : data.dump(), 300)));
                            return {false, "error"};
                        }
                        continue;
                    }

                    if (event == "new_message") {
                        json const message = object_or(data, "message");
                        if (!turn_token_.empty() && text_of(message, "token") != turn_token_) {
                            continue;
                        }
                        last_seen             = clock::now();
                        bool const progressed = apply(message, answer, sink);
                        if (progressed) {
                            deadline = last_seen + std::chrono::seconds(timeout_);
                        }
                        if (failed(message)) {
                            sink(error(error_message(message.value("error", json()), "the model reported an error")));
                            return {false, "error"};
                        }
                        if (completed(message)) {
                            return {true, ""};
                        }
                    } else if (event == "chat_title_update") {
                        std::string token = text_of(data, "chat_token");
                        if (token.empty()) {
                            token = text_of(data, "conversation_id");
                        }
                        if (token == conversation_id_ && data.contains("title") && data["title"].is_string() &&
                            !data["title"].get<std::string>().empty())
                        {
                            title_ = data["title"].get<std::string>();
                        }
                    } else if (event == "error" || event == "session.error") {
                        sink(error(head_codepoints(data.empty() ? event : data.dump(), 300)));
                        return {false, "error"};
                    }
                }

                // One last poll: the model may have finished while we were idle.
                if (acked) {
                    try {
                        auto polled = poll();
                        if (!polled.first.is_null()) {
                            apply(polled.first, answer, sink);
                            if (failed(polled.first)) {
                                sink(error(
                                  error_message(polled.first.value("error", json()), "the model reported an error")));
                                return {false, "error"};
                            }
                        }
                        if (polled.second) {
                            return {true, ""};
                        }
                    } catch (std::exception const& poll_failure) {
                        note(std::string("final poll failed: ") + poll_failure.what(), debug_, "gap");
                    }
                }
                sink(error("GapGPT timed out waiting for the answer"));
                return {false, "timeout"};
            }

            // Diff one message snapshot's fragments into text/thinking/tool deltas.
            bool apply(json const& message, std::string& answer, event_sink const& sink) {
                if (!message.is_object()) {
                    return false;
                }
                bool progressed = false;
                if (!message.contains("response") || !message["response"].is_array()) {
                    return false;
                }
                for (auto const& block : message["response"]) {
                    if (!block.is_object()) {
                        continue;
                    }
                    std::string const kind = text_of(block, "type");
                    if (!block.contains("content") || block["content"].is_null()) {
                        continue;
                    }
                    if (contains(kSkipTypes, kind)) {
                        continue;
                    }
                    auto const& content = block["content"];
                    if (!content.is_string()) {
                        note("skipping a non-text block " + kind, debug_, "gap");
                        continue;
                    }
                    std::string const body       = content.get<std::string>();
                    std::string       identifier = text_of(block, "block_id");
                    if (identifier.empty()) {
                        identifier = kind;
                    }
                    std::size_t const seen   = blocks_.contains(identifier) ? blocks_[identifier] : 0;
                    std::size_t const length = codepoint_length(body);
                    if (length <= seen) {
                        continue;
                    }
                    blocks_[identifier]      = length;
                    std::string const sliced = drop_codepoints(body, seen);
                    if (kind == "text") {
                        answer += sliced;
                        sink(text(sliced));
                    } else if (contains(kThinkingTypes, kind)) {
                        sink(thinking(sliced));
                    } else {
                        std::string const name = kind.empty() ? std::string("tool") : kind;
                        sink(tool_call(name, "delta", head_codepoints(sliced, 400)));
                    }
                    progressed = true;
                }
                return progressed;
            }

            std::pair<json, bool> poll() {
                if (conversation_id_.empty()) {
                    return {json(), false};
                }
                std::string path(kChatPath);
                auto const  marker = path.find("{token}");
                if (marker != std::string::npos) {
                    path.replace(marker, 7, conversation_id_);
                }
                json data;
                try {
                    http_request request;
                    request.url             = owner_.origin() + path;
                    request.timeout_seconds = 30;
                    request.headers         = headers_;
                    data                    = request_json(request);
                } catch (std::exception const& poll_failure) {
                    note(std::string("poll failed: ") + poll_failure.what(), debug_, "gap");
                    return {json(), false};
                }
                if (!data.is_object()) {
                    return {json(), false};
                }
                json chosen;
                if (data.contains("messages") && data["messages"].is_array()) {
                    for (auto const& message : data["messages"]) {
                        if (!message.is_object()) {
                            continue;
                        }
                        bool const matches =
                          (!turn_token_.empty() && text_of(message, "token") == turn_token_) ||
                          (turn_token_.empty() && message.contains("response") && message["response"].is_array() &&
                           !message["response"].empty());
                        if (matches) {
                            chosen = message;
                        }
                    }
                }
                if (!chosen.is_null() && turn_token_.empty()) {
                    turn_token_ = text_of(chosen, "token");
                }
                if (chosen.is_null()) {
                    return {json(), false};
                }
                return {chosen, completed(chosen)};
            }

            void finish_turn(std::string_view prompt, std::string_view answer) {
                ++turns_;
                if (!title_.empty()) {
                    resume_["title"] = title_;
                }
                record("user", prompt);
                record("assistant", answer);
            }

            gap_provider&                                    owner_;
            std::vector<std::pair<std::string, std::string>> headers_;
            websocket                                        socket_;
            std::string                                      turn_token_;
            std::string                                      title_;
            std::map<std::string, std::size_t>               blocks_;
        };

        std::unique_ptr<conversation>
        gap_provider::open(session::match const& match, std::string_view model, open_options const& options) {
            return std::make_unique<gap_conversation>(*this, match, model, options);
        }

    } // namespace

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization)
    bool const gap_registered = register_provider(std::make_unique<gap_provider>());

} // namespace dana::ai
