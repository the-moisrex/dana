#include "../diagnostics.hpp"
#include "../files.hpp"
#include "../http.hpp"
#include "../models.hpp"
#include "../sentinel.hpp"
#include "../sse.hpp"
#include "../uuid.hpp"
#include "provider.hpp"
#include "registry.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace dana::ai {

    namespace {

        using nlohmann::json;

        constexpr std::string_view kFilesPath                = "/backend-api/files";
        constexpr std::string_view kSessionPath              = "/api/auth/session";
        constexpr std::string_view kConduitPreparePath       = "/backend-api/f/conversation/prepare";
        constexpr std::string_view kConduitPath              = "/backend-api/f/conversation";
        constexpr std::string_view kRequirementsPreparePath  = "/backend-api/sentinel/chat-requirements/prepare";
        constexpr std::string_view kRequirementsFinalizePath = "/backend-api/sentinel/chat-requirements/finalize";
        constexpr std::string_view kSdkPath                  = "/backend-api/sentinel/sdk.js";
        constexpr std::string_view kRootMessage              = "client-created-root";
        constexpr long             kPollIntervalMillis       = 500;

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

        // A string/number field as text (the sentinel's `difficulty` can be either).
        std::string scalar_text(json const& object, std::string_view key) {
            if (!object.is_object() || !object.contains(key)) {
                return {};
            }
            auto const& value = object[std::string(key)];
            if (value.is_string()) {
                return value.get<std::string>();
            }
            if (value.is_number_integer()) {
                return std::to_string(value.get<std::int64_t>());
            }
            if (value.is_number_unsigned()) {
                return std::to_string(value.get<std::uint64_t>());
            }
            if (value.is_number_float()) {
                return std::format("{}", value.get<double>());
            }
            return {};
        }

        bool flag_or(json const& object, std::string_view key) {
            if (!object.is_object() || !object.contains(key)) {
                return false;
            }
            auto const& value = object[std::string(key)];
            return value.is_boolean() && value.get<bool>();
        }

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

        // Python's `str()` of a JSON scalar (used for tool-call arguments).
        std::string json_text(json const& value) {
            if (value.is_string()) {
                return value.get<std::string>();
            }
            if (value.is_null()) {
                return "None";
            }
            if (value.is_boolean()) {
                return value.get<bool>() ? "True" : "False";
            }
            if (value.is_number_integer()) {
                return std::to_string(value.get<std::int64_t>());
            }
            if (value.is_number_unsigned()) {
                return std::to_string(value.get<std::uint64_t>());
            }
            if (value.is_number_float()) {
                return std::format("{}", value.get<double>());
            }
            return value.dump();
        }

        bool truthy(json const& value) {
            if (value.is_null()) {
                return false;
            }
            if (value.is_boolean()) {
                return value.get<bool>();
            }
            if (value.is_number_integer()) {
                return value.get<std::int64_t>() != 0;
            }
            if (value.is_number_unsigned()) {
                return value.get<std::uint64_t>() != 0;
            }
            if (value.is_number_float()) {
                return value.get<double>() != 0.0;
            }
            if (value.is_string()) {
                return !value.get<std::string>().empty();
            }
            if (value.is_array() || value.is_object()) {
                return !value.empty();
            }
            return true;
        }

        // `content.get(key) or ""` as a string.
        std::string content_text(json const& content, std::string_view key) {
            if (!content.is_object() || !content.contains(key) || !truthy(content[std::string(key)])) {
                return {};
            }
            return json_text(content[std::string(key)]);
        }

        // `a or b or ""` over JSON values.
        json first_truthy(json const& object, std::string_view first, std::string_view second) {
            if (object.is_object() && object.contains(first) && truthy(object[std::string(first)])) {
                return object[std::string(first)];
            }
            if (object.is_object() && object.contains(second) && truthy(object[std::string(second)])) {
                return object[std::string(second)];
            }
            return "";
        }

        // Python's `a // b` (floor division).
        std::int64_t floor_div(std::int64_t value, std::int64_t divisor) {
            std::int64_t quotient = value / divisor;
            if ((value % divisor != 0) && ((value < 0) != (divisor < 0))) {
                --quotient;
            }
            return quotient;
        }

        std::pair<std::int64_t, std::string> local_timezone() {
            std::int64_t const offset = floor_div(static_cast<std::int64_t>(::timezone), 60);
            std::string     zone   = tzname[0] != nullptr ? tzname[0] : "UTC";
            return {offset, zone};
        }

        double epoch_seconds() {
            return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        std::string asset_pointer(std::string const& file_id) {
            return (file_id.starts_with("file-") ? "file-service://" : "sediment://") + file_id;
        }

        std::vector<std::pair<std::string, std::string>> with_target(
          std::vector<std::pair<std::string, std::string>> headers,
          std::string_view                                 path) {
            set_header(headers, "X-OpenAI-Target-Path", std::string(path));
            set_header(headers, "X-OpenAI-Target-Route", std::string(path));
            return headers;
        }

        // `_part_text`: a message part's text (string, list of parts, or {text}).
        std::string part_text(json const& part) {
            if (part.is_string()) {
                return part.get<std::string>();
            }
            if (part.is_array()) {
                std::string out;
                for (auto const& item : part) {
                    out += part_text(item);
                }
                return out;
            }
            if (part.is_object() && part.contains("text") && truthy(part["text"])) {
                return json_text(part["text"]);
            }
            return "";
        }

        // The conversation's messages in create-time order (the poll fallback).
        std::vector<json> walk_messages(json const& data) {
            std::vector<json> out;
            if (!data.is_object() || !data.contains("mapping") || !data["mapping"].is_object()) {
                return out;
            }
            std::vector<std::pair<double, json>> ordered;
            for (auto const& [key, node] : data["mapping"].items()) {
                if (!node.is_object() || !node.contains("message") || !node["message"].is_object()) {
                    continue;
                }
                auto const& message = node["message"];
                double      created = 0;
                if (message.contains("create_time") && message["create_time"].is_number()) {
                    created = message["create_time"].get<double>();
                }
                ordered.emplace_back(created, message);
            }
            std::stable_sort(ordered.begin(), ordered.end(), [](auto const& left, auto const& right) {
                return left.first < right.first;
            });
            for (auto& entry : ordered) {
                out.push_back(std::move(entry.second));
            }
            return out;
        }

        bool is_finished(json const& message) {
            if (!message.is_object()) {
                return false;
            }
            if (!message.contains("author") || !message["author"].is_object()) {
                return false;
            }
            if (string_or(message["author"], "role") != "assistant") {
                return false;
            }
            if (string_or(message, "status") == "finished_successfully") {
                return true;
            }
            if (message.contains("metadata") && message["metadata"].is_object() &&
                message["metadata"].contains("finish_details") && truthy(message["metadata"]["finish_details"]))
            {
                return true;
            }
            return false;
        }

        // Turn cumulative conversation messages into deltas (deduplicated by id).
        //
        // The site interleaves whole `message` frames with JSON-patch frames
        // (`{"o": "patch", "v": [{"p": "/message/content/parts/0", …}]}`) that carry
        // the rest of a streaming answer; both are applied here, keyed by message id so
        // a fresh message starts from an empty part list.
        class message_stream {
          public:
            // A frame may nest another patch frame under `v`, so this one call is
            // recursive.
            // NOLINTNEXTLINE(misc-no-recursion)
            std::vector<stream_event> feed(json const& event) {
                std::vector<stream_event> out;
                if (!event.is_object()) {
                    return out;
                }
                if (event.contains("message") && event["message"].is_object()) {
                    auto events = feed_message(event["message"]);
                    out.insert(out.end(), events.begin(), events.end());
                }
                if (event.contains("v")) {
                    auto const& value = event["v"];
                    if (value.is_object()) {
                        auto events = feed(value);
                        out.insert(out.end(), events.begin(), events.end());
                    } else if (value.is_array() && event.value("o", "") == "patch") {
                        auto events = apply_patches(value);
                        out.insert(out.end(), events.begin(), events.end());
                    }
                }
                return out;
            }

            std::vector<stream_event> feed_message(json const& message) {
                std::vector<stream_event> out;
                if (!message.is_object()) {
                    return out;
                }
                if (!message.contains("author") || !message["author"].is_object()) {
                    return out;
                }
                if (string_or(message["author"], "role") != "assistant") {
                    return out;
                }

                if (message.contains("id") && truthy(message["id"])) {
                    std::string identifier = id_text(message["id"]);
                    if (!identifier.empty()) {
                        last_assistant_id_ = identifier;
                        if (identifier != current_id_) {
                            current_id_ = identifier;
                            parts_.clear();
                            text_ = 0;
                        }
                    }
                }

                json const content =
                  message.contains("content") && message["content"].is_object() ? message["content"] : json::object();
                std::string const ctype     = string_or(content, "content_type");
                std::string const recipient = string_or(message, "recipient", "all");
                if (recipient != "all" && !recipient.empty() && (!tool_ || recipient != *tool_)) {
                    tool_ = recipient;
                    out.push_back(tool_call(recipient, "start"));
                }

                if (ctype == "text" || ctype == "multimodal_text") {
                    parts_.clear();
                    if (content.contains("parts") && content["parts"].is_array()) {
                        for (auto const& part : content["parts"]) {
                            parts_.push_back(part);
                        }
                    }
                    auto events = sync_text();
                    out.insert(out.end(), events.begin(), events.end());
                } else if (ctype == "thoughts" || ctype == "reasoning_recap") {
                    json parts = json::array();
                    if (content.contains("thoughts") && truthy(content["thoughts"]) && content["thoughts"].is_array()) {
                        parts = content["thoughts"];
                    } else if (content.contains("parts") && content["parts"].is_array()) {
                        parts = content["parts"];
                    }
                    std::string full;
                    bool        first = true;
                    for (auto const& part : parts) {
                        if (!first) {
                            full += "\n";
                        }
                        full  += part_text(part);
                        first  = false;
                    }
                    if (full.size() > thinking_) {
                        out.push_back(thinking(full.substr(thinking_)));
                        thinking_ = full.size();
                    }
                } else if (ctype == "code") {
                    out.push_back(tool_call("code", "delta", content_text(content, "text")));
                } else if (ctype == "execution_output") {
                    std::string const args = content_text(content, "text");
                    out.push_back(tool_call(tool_.value_or("code"), "done", args.substr(0, 200)));
                    tool_.reset();
                } else if (ctype == "tether_browsing_display") {
                    std::string const args = json_text(first_truthy(content, "result", "summary"));
                    out.push_back(tool_call("search", "done", args.substr(0, 200)));
                } else if (ctype == "tether_quote") {
                    std::string const args = json_text(first_truthy(content, "title", "url"));
                    out.push_back(tool_call("source", "delta", args.substr(0, 200)));
                }

                if (!out.empty()) {
                    emitted_ = true;
                }
                return out;
            }

            [[nodiscard]] std::string answer() const {
                std::string out;
                for (auto const& part : answer_parts_) {
                    out += part;
                }
                return out;
            }

            [[nodiscard]] bool emitted() const {
                return emitted_;
            }

            [[nodiscard]] std::string const& last_assistant_id() const {
                return last_assistant_id_;
            }

          private:
            std::vector<stream_event> apply_patches(json const& patches) {
                std::vector<stream_event> out;
                for (auto const& patch : patches) {
                    if (!patch.is_object()) {
                        continue;
                    }
                    std::string const path = string_or(patch, "p");
                    if (!path.starts_with("/message/content/parts/")) {
                        continue;
                    }
                    auto const        slash      = path.rfind('/');
                    std::string const index_text = path.substr(slash + 1);
                    std::size_t       index      = 0;
                    try {
                        index = static_cast<std::size_t>(std::stoul(index_text));
                    } catch (std::exception const&) {
                        continue;
                    }
                    if (index > 10'000) {
                        continue;
                    }
                    while (parts_.size() <= index) {
                        parts_.push_back("");
                    }
                    json const value = patch.contains("v") ? patch["v"] : json();
                    if (patch.value("o", "") == "append" && parts_[index].is_string() && value.is_string()) {
                        parts_[index] = parts_[index].get<std::string>() + value.get<std::string>();
                    } else {
                        parts_[index] = value;
                    }
                    auto events = sync_text();
                    out.insert(out.end(), events.begin(), events.end());
                }
                return out;
            }

            std::vector<stream_event> sync_text() {
                std::string full;
                for (auto const& part : parts_) {
                    full += part_text(part);
                }
                if (full.size() <= text_) {
                    return {};
                }
                std::string const delta = full.substr(text_);
                text_                   = full.size();
                answer_parts_.push_back(delta);
                return {dana::ai::text(delta)};
            }

            std::size_t                text_     = 0;
            std::size_t                thinking_ = 0;
            std::optional<std::string> tool_;
            bool                       emitted_ = false;
            std::vector<std::string>   answer_parts_;
            std::string                last_assistant_id_;
            std::string                current_id_;
            std::vector<json>          parts_;
        };

        class chatgpt_provider : public provider {
          public:
            chatgpt_provider()
              : info_{
                  .name                = "chatgpt",
                  .label               = "ChatGPT",
                  .origin              = "https://chatgpt.com",
                  .session_target      = "chatgpt.com",
                  .default_model       = std::string(models::default_model("chatgpt")),
                  .models              = models::list("chatgpt"),
                  .inline_limit        = 48UL * 1024UL,
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

            [[nodiscard]] std::string const& origin() const {
                return info_.origin;
            }

            [[nodiscard]] std::vector<std::pair<std::string, std::string>> build_headers(
              session::match const& match) const {
                std::string device;
                if (auto const stored = session::storage_value(match, {"oai-did"}); stored && !stored->empty()) {
                    device = *stored;
                } else {
                    device = uuid4();
                }
                return session::build_headers(
                  match,
                  {
                    {       "Accept",              "*/*"},
                    { "Content-Type", "application/json"},
                    {       "Origin",       info_.origin},
                    {      "Referer", info_.origin + "/"},
                    {      "oai-did",             device},
                    {"OAI-Device-Id",             device},
                    { "oai-language",            "en-US"},
                    { "OAI-Language",            "en-US"},
                });
            }

            // Resolve the access token and account id into `headers`.
            void authorize(std::vector<std::pair<std::string, std::string>>& headers) const {
                http_request request;
                request.url             = info_.origin + std::string(kSessionPath);
                request.timeout_seconds = 60;
                apply_headers(request, headers);
                json              session = request_json(request);
                std::string const token   = string_or(session, "accessToken");
                if (token.empty()) {
                    throw sentinel::error(
                      "the session is not signed in (no access token); open chatgpt.com once in the browser");
                }
                set_header(headers, "Authorization", "Bearer " + token);
                if (session.is_object() && session.contains("account") && session["account"].is_object()) {
                    std::string const account = string_or(session["account"], "id");
                    if (!account.empty()) {
                        set_header(headers, "chatgpt-account-id", account);
                        set_header(headers, "ChatGPT-Account-ID", account);
                    }
                }
            }

            // Return (script_source, data_build) from the app shell.
            std::pair<std::optional<std::string>, std::string> bootstrap(
              std::vector<std::pair<std::string, std::string>> const& headers,
              bool                                                    debug) const {
                std::string html;
                try {
                    auto request_headers = headers;
                    set_header(request_headers, "Accept", "text/html,application/xhtml+xml");
                    http_request request;
                    request.url             = info_.origin + "/";
                    request.timeout_seconds = 60;
                    apply_headers(request, request_headers);
                    html = request_text(request);
                } catch (std::exception const& failure) {
                    note("could not read the app shell for the sentinel fingerprint: " + std::string(failure.what()),
                         debug,
                         "chatgpt");
                    return {std::nullopt, ""};
                }

                std::string       build;
                std::string const marker = "data-build=";
                auto const        start  = html.find(marker);
                if (start != std::string::npos) {
                    auto const value = start + marker.size();
                    if (value < html.size()) {
                        char const quote = html[value];
                        if (quote == '"' || quote == '\'') {
                            auto const end = html.find(quote, value + 1);
                            if (end != std::string::npos) {
                                build = html.substr(value + 1, end - value - 1);
                            }
                        }
                    }
                }
                std::optional<std::string> script;
                if (html.find(kSdkPath) != std::string::npos) {
                    script = info_.origin + std::string(kSdkPath);
                }
                return {script, build};
            }

            // Run the sentinel gate and return the header map it hands out.
            json sentinel_headers(std::vector<std::pair<std::string, std::string>> const& headers,
                                  json const&                                             fingerprint,
                                  bool                                                    debug) const {
                std::string const p_token = sentinel::requirements_token(fingerprint);
                std::string const prepare_path(kRequirementsPreparePath);
                json const        prepared = post_json(
                  with_target(headers, prepare_path),
                  info_.origin + prepare_path,
                  json{
                    {"p", p_token}
                },
                  60);
                std::string const prepare_token = string_or(prepared, "prepare_token");

                std::string proof;
                if (prepared.is_object() && prepared.contains("proofofwork") && prepared["proofofwork"].is_object()) {
                    auto const& info = prepared["proofofwork"];
                    if (flag_or(info, "required")) {
                        note("solving proof-of-work (difficulty " + scalar_text(info, "difficulty") + ")",
                             debug,
                             "sentinel");
                        proof =
                          sentinel::solve_proof(string_or(info, "seed"), scalar_text(info, "difficulty"), fingerprint);
                        note("proof-of-work solved", debug, "sentinel");
                    }
                }

                std::string turnstile;
                if (prepared.is_object() && prepared.contains("turnstile") && prepared["turnstile"].is_object()) {
                    auto const& info = prepared["turnstile"];
                    if (flag_or(info, "required") && info.contains("dx") && !info["dx"].is_null()) {
                        note("solving turnstile", debug, "sentinel");
                        std::string const dx =
                          info["dx"].is_string() ? info["dx"].get<std::string>() : info["dx"].dump();
                        turnstile = sentinel::solve_turnstile(dx, p_token).value_or("");
                    }
                }

                std::string const finalize_path(kRequirementsFinalizePath);
                json const        finalized = post_json(
                  with_target(headers, finalize_path),
                  info_.origin + finalize_path,
                  json{
                    {"prepare_token", prepare_token},
                    {  "proofofwork",         proof},
                    {    "turnstile",     turnstile}
                },
                  60);
                std::string const token = string_or(finalized, "token");
                if (token.empty()) {
                    throw sentinel::error("the sentinel gate returned no chat-requirements token");
                }

                json result_headers{
                  {"OpenAI-Sentinel-Chat-Requirements-Token", token}
                };
                if (!proof.empty()) {
                    result_headers["OpenAI-Sentinel-Proof-Token"] = proof;
                }
                if (!turnstile.empty()) {
                    result_headers["OpenAI-Sentinel-Turnstile-Token"] = turnstile;
                }
                if (finalized.is_object() && finalized.contains("so_token") && finalized["so_token"].is_string()) {
                    result_headers["OpenAI-Sentinel-SO-Token"] = finalized["so_token"];
                }
                return result_headers;
            }

            // Upload one attachment; return the descriptor the message needs.
            json upload(std::vector<std::pair<std::string, std::string>> const& headers, attachment const& file) const {
                auto const [offset, zone] = local_timezone();
                (void) zone;
                std::string const path(kFilesPath);
                json const        prepared = post_json(
                  with_target(headers, path),
                  info_.origin + path,
                  json{
                    {                "file_name",                           file.name},
                    {                "file_size", static_cast<std::int64_t>(file.size())},
                    {                "mime_type",                           file.mime},
                    {"client_resolved_mime_type",                           file.mime},
                    {   "mime_resolution_source",                            "client"},
                    {                 "use_case",                        "multimodal"},
                    {      "timezone_offset_min",                              offset},
                    {        "reset_rate_limits",                               false},
                },
                  120);
                std::string const file_id    = string_or(prepared, "file_id");
                std::string const upload_url = string_or(prepared, "upload_url");
                if (file_id.empty() || upload_url.empty()) {
                    throw std::runtime_error("upload failed: " + prepared.dump().substr(0, 300));
                }

                http_request put;
                put.url             = upload_url;
                put.method          = "PUT";
                put.body            = file.data;
                put.content_type    = file.mime;
                put.timeout_seconds = 600;
                put.headers         = {
                  {          "Content-Type",    file.mime},
                  {        "x-ms-blob-type",  "BlockBlob"},
                  {"x-ms-blob-content-type",    file.mime},
                  {          "x-ms-version", "2020-04-08"},
                };
                auto const [status, body] = request_raw(put);
                if (status >= 400) {
                    std::string const detail = body.substr(0, 200);
                    throw std::runtime_error(
                      "uploading " + file.name + " failed: HTTP " + std::to_string(status) + " " + detail);
                }

                std::string const uploaded_path = path + "/" + file_id + "/uploaded";
                post_json(with_target(headers, uploaded_path), info_.origin + uploaded_path, json::object(), 120);
                return json{
                  {   "id",                             file_id},
                  { "name",                           file.name},
                  { "mime",                           file.mime},
                  { "size", static_cast<std::int64_t>(file.size())},
                  {"image",     file.mime.starts_with("image/")}
                };
            }

            // The conduit token the `f/conversation` request must carry.
            std::string conduit_token(std::vector<std::pair<std::string, std::string>> const& headers,
                                      std::string const&                                      prompt,
                                      std::string const&                                      model) const {
                auto const [offset, zone] = local_timezone();
                json const body{
                  {                "action","next"                                            },
                  { "fork_from_shared_post",                                             false},
                  {     "parent_message_id",                         std::string(kRootMessage)},
                  {                 "model",                                             model},
                  {  "client_prepare_state",                                            "none"},
                  {   "timezone_offset_min",                                            offset},
                  {              "timezone",                                              zone},
                  {     "conversation_mode",                   {{"kind", "primary_assistant"}}},
                  {          "system_hints",                                     json::array()},
                  {         "partial_query",
                   {{"id", uuid4()},
                   {"author", {{"role", "user"}}},
                   {"content", {{"content_type", "text"}, {"parts", json::array({prompt})}}}} },
                  {    "supports_buffering",                                              true},
                  {   "supported_encodings",                               json::array({"v1"})},
                  {"client_contextual_info",                     {{"app_name", "chatgpt.com"}}},
                };
                std::string const path(kConduitPreparePath);
                auto              request_headers = with_target(headers, path);
                set_header(request_headers, "X-Conduit-Token", "no-token");
                json const        prepared = post_json(request_headers, info_.origin + path, body, 60);
                std::string const conduit  = string_or(prepared, "conduit_token");
                if (conduit.empty()) {
                    throw sentinel::error("the conduit prepare returned no token: " + prepared.dump().substr(0, 200));
                }
                return conduit;
            }

            json stream_body(json const&        messages,
                             std::string const& model,
                             std::string const& conversation_id,
                             std::string const& parent) const {
                auto const [offset, zone] = local_timezone();
                json body{
                  {                              "action","next"                                                          },
                  {                            "messages",                        messages},
                  {                   "parent_message_id",                          parent},
                  {                               "model",                           model},
                  {                "client_prepare_state",                       "success"},
                  {                 "timezone_offset_min",                          offset},
                  {                            "timezone",                            zone},
                  {                   "conversation_mode", {{"kind", "primary_assistant"}}},
                  {            "enable_message_followups",                            true},
                  {                        "system_hints",                   json::array()},
                  {                  "supports_buffering",                            true},
                  {                 "supported_encodings",             json::array({"v1"})},
                  {              "client_contextual_info",
                   {{"is_dark_mode", false},
                   {"time_since_loaded", 120},
                   {"page_height", 900},
                   {"page_width", 1400},
                   {"pixel_ratio", 2},
                   {"screen_height", 1440},
                   {"screen_width", 2560},
                   {"app_name", "chatgpt.com"}}                                           },
                  {"paragen_cot_summary_display_override",                         "allow"},
                  {               "force_parallel_switch",                          "auto"},
                };
                if (!conversation_id.empty()) {
                    body["conversation_id"] = conversation_id;
                }
                return body;
            }

            json message_body(std::string const&       text,
                              std::string const&       role,
                              std::vector<json> const& attachments) const {
                json message{
                  {         "id",                   uuid4()                                 },
                  {     "author",                                           {{"role", role}}},
                  {"create_time",                                            epoch_seconds()},
                  {    "content", {{"content_type", "text"}, {"parts", json::array({text})}}},
                  {   "metadata",
                   {{"developer_mode_connector_ids", json::array()},
                   {"selected_sources", json::array()},
                   {"selected_github_repos", json::array()},
                   {"selected_all_github_repos", false},
                   {"serialization_metadata", {{"custom_symbol_offsets", json::array()}}}}  },
                };
                if (attachments.empty()) {
                    return message;
                }

                json parts = json::array();
                if (!text.empty()) {
                    parts.push_back(text);
                }
                for (auto const& item : attachments) {
                    if (item.value("image", false)) {
                        parts.push_back(json{
                          {"content_type", "image_asset_pointer"},
                          {"asset_pointer", asset_pointer(item.value("id", ""))},
                          {"size_bytes", item.value("size", 0LL)}
                        });
                    }
                }
                if (parts != json::array({text})) {
                    message["content"] = json{
                      {"content_type", "multimodal_text"},
                      {       "parts",             parts}
                    };
                }

                json list = json::array();
                for (auto const& item : attachments) {
                    list.push_back(json{
                      {"id", item.value("id", "")},
                      {"size", item.value("size", 0LL)},
                      {"name", item.value("name", "")},
                      {"mime_type", item.value("mime", "")},
                      {"width", nullptr},
                      {"height", nullptr}
                    });
                }
                message["metadata"]["attachments"] = list;
                return message;
            }

          private:
            json post_json(std::vector<std::pair<std::string, std::string>> const& headers,
                           std::string const&                                      url,
                           json const&                                             body,
                           long                                                    timeout) const {
                http_request request;
                request.url             = url;
                request.method          = "POST";
                request.json_body       = body.dump();
                request.timeout_seconds = timeout;
                apply_headers(request, headers);
                return request_json(request);
            }

            provider_info info_;
        };

        class chatgpt_conversation : public conversation {
          public:
            chatgpt_conversation(chatgpt_provider&   owner,
                                 session::match      match,
                                 std::string         model,
                                 open_options const& options)
              : conversation(std::move(match), std::move(model), options),
                owner_(owner),
                headers_(owner.build_headers(match_)) {
                parent_message_id_ = std::string(kRootMessage);
                if (options.resume.is_object() && options.resume.contains("parent_message_id") &&
                    options.resume["parent_message_id"].is_string())
                {
                    parent_message_id_ = options.resume["parent_message_id"].get<std::string>();
                }
            }

            void send(std::string const&              prompt,
                      std::vector<std::string> const& files,
                      event_sink const&               sink) override {
                if (!ready_) {
                    try {
                        prepare();
                    } catch (std::exception const& failure) {
                        sink(error(failure.what()));
                        return;
                    }
                }

                std::vector<attachment> attachments;
                try {
                    attachments = read_attachments(files);
                } catch (file_error const& failure) {
                    sink(error(failure.what()));
                    return;
                }
                std::vector<json> uploaded;
                for (auto const& file : attachments) {
                    try {
                        uploaded.push_back(owner_.upload(headers_, file));
                    } catch (std::exception const& failure) {
                        sink(error(failure.what()));
                        return;
                    }
                    sink(tool_call("upload", "done", file.name));
                }

                std::string const message = (!system_.empty() && turns_ == 0) ? system_ + "\n\n" + prompt : prompt;

                json gate;
                try {
                    gate = owner_.sentinel_headers(headers_, fingerprint_, debug_);
                } catch (std::exception const& failure) {
                    sink(error("sentinel: " + std::string(failure.what())));
                    return;
                }
                std::string conduit;
                try {
                    conduit = owner_.conduit_token(headers_, message, model_);
                } catch (std::exception const& failure) {
                    sink(error(failure.what()));
                    return;
                }

                std::string const path(kConduitPath);
                auto              stream_headers = with_target(headers_, path);
                for (auto const& [name, value] : gate.items()) {
                    set_header(stream_headers, name, value.is_string() ? value.get<std::string>() : value.dump());
                }
                set_header(stream_headers, "Accept", "text/event-stream");
                set_header(stream_headers, "X-Conduit-Token", conduit);

                message_stream parser;
                bool           emitted = false;

                http_request request;
                request.url    = owner_.origin() + path;
                request.method = "POST";
                request.json_body =
                  owner_
                    .stream_body(json::array({owner_.message_body(message, "user", uploaded)}),
                                 model_,
                                 conversation_id_,
                                 parent_message_id_)
                    .dump();
                request.timeout_seconds = timeout_;
                request.debug           = debug_;
                apply_headers(request, stream_headers);

                sse_decoder decoder;
                try {
                    std::ignore = stream_lines(request, [&](std::string_view line) {
                        for (auto const& payload : decoder.feed(line)) {
                            json event = json::parse(payload, nullptr, false);
                            if (event.is_discarded()) {
                                sink(raw(payload));
                                continue;
                            }
                            if (!event.is_object()) {
                                continue;
                            }
                            std::string const kind = event.value("type", "");
                            if (kind == "stream_handoff" || kind == "resume_conversation_token") {
                                if (event.contains("conversation_id") && event["conversation_id"].is_string()) {
                                    conversation_id_ = event["conversation_id"].get<std::string>();
                                }
                                continue;
                            }
                            if (event.contains("conversation_id") && event["conversation_id"].is_string()) {
                                conversation_id_ = event["conversation_id"].get<std::string>();
                            }
                            for (auto const& item : parser.feed(event)) {
                                if (item.kind == event_kind::text || item.kind == event_kind::thinking ||
                                    item.kind == event_kind::tool_call)
                                {
                                    emitted = true;
                                }
                                sink(item);
                            }
                        }
                    });
                } catch (std::exception const& failure) {
                    sink(error(failure.what()));
                    return;
                }

                if (!conversation_id_.empty() && !emitted) {
                    poll(sink, parser);
                }

                ++turns_;
                if (!parser.last_assistant_id().empty()) {
                    parent_message_id_ = parser.last_assistant_id();
                    if (!resume_.is_object()) {
                        resume_ = json::object();
                    }
                    resume_["parent_message_id"] = parent_message_id_;
                }
                record("user", prompt);
                record("assistant", parser.answer());
                sink(done());
            }

          private:
            void prepare() {
                owner_.authorize(headers_);
                auto const [script, build] = owner_.bootstrap(headers_, debug_);
                std::string user_agent;
                for (auto const& header : headers_) {
                    if (header.first == "User-Agent") {
                        user_agent = header.second;
                    }
                }
                fingerprint_ = sentinel::build_fingerprint(user_agent, script, build);
                ready_       = true;
            }

            // Stream the turn from `GET /backend-api/conversation/<id>`.
            void poll(event_sink const& sink, message_stream& parser) {
                auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_);
                while (std::chrono::steady_clock::now() < deadline) {
                    json data;
                    try {
                        http_request request;
                        request.url             = owner_.origin() + "/backend-api/conversation/" + conversation_id_;
                        request.timeout_seconds = 60;
                        apply_headers(request, headers_);
                        data = request_json(request);
                    } catch (std::exception const& failure) {
                        sink(error(failure.what()));
                        return;
                    }
                    bool finished = false;
                    for (auto const& message : walk_messages(data)) {
                        for (auto const& item : parser.feed_message(message)) {
                            sink(item);
                        }
                        if (is_finished(message)) {
                            finished = true;
                        }
                    }
                    if (finished && parser.emitted()) {
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMillis));
                }
                if (!parser.emitted()) {
                    sink(error("the turn never produced a message to read"));
                }
            }

            chatgpt_provider&                                owner_;
            std::vector<std::pair<std::string, std::string>> headers_;
            std::string                                      parent_message_id_;
            json                                             fingerprint_ = json::array();
            bool                                             ready_       = false;
        };

        std::unique_ptr<conversation>
        chatgpt_provider::open(session::match const& match, std::string const& model, open_options const& options) {
            return std::make_unique<chatgpt_conversation>(*this, match, model, options);
        }

    } // namespace

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization)
    bool const chatgpt_registered = register_provider(std::make_unique<chatgpt_provider>());

} // namespace dana::ai
