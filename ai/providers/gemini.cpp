#include "../diagnostics.hpp"
#include "../files.hpp"
#include "../http.hpp"
#include "../models.hpp"
#include "../uuid.hpp"
#include "provider.hpp"
#include "registry.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <optional>
#include <random>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dana::ai {

    namespace {

        using nlohmann::json;

        constexpr std::string_view kInitPath = "/app";
        constexpr std::string_view kGenerateUrl =
          "https://gemini.google.com/_/BardChatUi/data/assistant.lamda.BardFrontendService/StreamGenerate";
        constexpr std::string_view kUploadUrl = "https://content-push.googleapis.com/upload";

        /// The model selector header and the (id, capacity, number) triple each model
        /// needs.  The id is Google's internal model id; the number is mirrored into
        /// the request body.  ``auto`` sends no header and lets Google pick.
        constexpr std::string_view kModelHeaderKey = "x-goog-ext-525001261-jspb";

        std::string trim(std::string_view value) {
            std::size_t first = value.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos) {
                return {};
            }
            std::size_t last = value.find_last_not_of(" \t\r\n");
            return std::string(value.substr(first, last - first + 1));
        }

        std::string lower(std::string_view value) {
            std::string out(value);
            for (char& c : out) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return out;
        }

        std::string upper(std::string value) {
            for (char& c : value) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            return value;
        }

        void lstrip(std::string& text) {
            std::size_t first = text.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) {
                text.clear();
                return;
            }
            if (first > 0) {
                text.erase(0, first);
            }
        }

        bool json_truthy(json const& value) {
            if (value.is_null()) {
                return false;
            }
            if (value.is_boolean()) {
                return value.get<bool>();
            }
            if (value.is_string()) {
                return !value.get<std::string>().empty();
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
            if (value.is_array() || value.is_object()) {
                return !value.empty();
            }
            return true;
        }

        std::string string_of(json const& value) {
            return value.is_string() ? value.get<std::string>() : std::string{};
        }

        // Read one path out of nested JSON arrays, or nullptr on a miss.
        json const* nested(json const& data, std::initializer_list<int> path) {
            json const* current = &data;
            for (int key : path) {
                if (!current->is_array()) {
                    return nullptr;
                }
                auto const size = static_cast<int>(current->size());
                if (key < -size || key >= size) {
                    return nullptr;
                }
                if (key < 0) {
                    key += size;
                }
                current = &(*current)[static_cast<std::size_t>(key)];
            }
            if (current->is_null()) {
                return nullptr;
            }
            return current;
        }

        // Percent-encode like Python's urllib.parse.urlencode (quote_plus).
        std::string url_encode(std::string_view value) {
            static char const* hex = "0123456789ABCDEF";
            std::string        out;
            out.reserve(value.size());
            for (char c : value) {
                auto const byte = static_cast<unsigned char>(c);
                if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
                    c == '_' || c == '.' || c == '-' || c == '~')
                {
                    out += c;
                } else if (c == ' ') {
                    out += '+';
                } else {
                    out += '%';
                    out += hex[(static_cast<unsigned>(byte) >> 4U) & 0xFU];
                    out += hex[static_cast<unsigned>(byte) & 0xFU];
                }
            }
            return out;
        }

        std::string form_body(std::vector<std::pair<std::string, std::string>> const& fields) {
            std::string out;
            for (auto const& [name, value] : fields) {
                if (!out.empty()) {
                    out += '&';
                }
                out += url_encode(name);
                out += '=';
                out += url_encode(value);
            }
            return out;
        }

        /// The part of `next` that is not already in `previous`.  The common prefix
        /// and suffix are kept, so an insertion in the middle emits just the inserted
        /// text while an ordinary streaming extension emits the new tail.
        std::string_view delta_of(std::string_view const next, std::string_view const previous) {
            if (next == previous) {
                return {};
            }
            if (previous.empty()) {
                return next;
            }
            std::size_t prefix = 0;
            std::size_t limit  = std::min(next.size(), previous.size());
            while (prefix < limit && next[prefix] == previous[prefix]) {
                ++prefix;
            }
            std::size_t suffix  = 0;
            limit              -= prefix;
            while (suffix < limit && next[next.size() - 1 - suffix] == previous[previous.size() - 1 - suffix]) {
                ++suffix;
            }
            return next.substr(prefix, next.size() - suffix - prefix);
        }

        struct utf8_unit {
            std::size_t  bytes = 0; // 0 means the sequence is incomplete
            std::int64_t units = 0;
        };

        // One UTF-8 code point's byte length and UTF-16 code-unit length (Google's
        // frame lengths count UTF-16 units, so astral characters count as two).
        utf8_unit decode_unit(std::string_view buffer, std::size_t index) {
            auto const   byte  = static_cast<unsigned char>(buffer[index]);
            std::size_t  need  = 0;
            std::int64_t units = 1;
            if (byte < 0x80) {
                return {.bytes = 1, .units = 1};
            }
            if (byte >= 0xC2 && byte <= 0xDF) {
                need = 1;
            } else if (byte >= 0xE0 && byte <= 0xEF) {
                need = 2;
            } else if (byte >= 0xF0 && byte <= 0xF4) {
                need  = 3;
                units = 2;
            } else {
                return {.bytes = 1, .units = 1}; // an invalid byte is one replacement character
            }
            if (index + need >= buffer.size()) {
                return {.bytes = 0, .units = 0};
            }
            return {.bytes = need + 1, .units = units};
        }

        // Incrementally split Google's `<length>\n<json>` stream frames.
        class frame_parser {
          public:
            std::vector<json> feed(std::string_view text) {
                if (!text.empty()) {
                    buffer_.append(text);
                }
                if (!prefix_checked_) {
                    static constexpr std::string_view prefix = ")]}'";
                    if (buffer_.size() < prefix.size() && prefix.starts_with(buffer_)) {
                        return {};
                    }
                    if (buffer_.starts_with(prefix)) {
                        buffer_.erase(0, prefix.size());
                        lstrip(buffer_);
                    }
                    prefix_checked_ = true;
                }

                std::vector<json> frames;
                for (;;) {
                    if (!expected_) {
                        lstrip(buffer_);
                        std::size_t digits = 0;
                        while (
                          digits < buffer_.size() && std::isdigit(static_cast<unsigned char>(buffer_[digits])) != 0)
                        {
                            ++digits;
                        }
                        if (digits == 0 || digits == buffer_.size() || buffer_[digits] != '\n') {
                            break;
                        }
                        expected_      = std::stoll(buffer_.substr(0, digits));
                        // The declared length counts from the newline after the digits
                        // through the frame's own trailing newline, so the payload
                        // slice starts right after the digits (the newline included).
                        start_         = digits;
                        scanned_chars_ = 0;
                        scanned_units_ = 0;
                    }

                    std::size_t index = start_ + scanned_chars_;
                    while (scanned_units_ < *expected_ && index < buffer_.size()) {
                        auto const unit = decode_unit(buffer_, index);
                        if (unit.bytes == 0) {
                            break;
                        }
                        if (scanned_units_ + unit.units > *expected_) {
                            break;
                        }
                        scanned_units_ += unit.units;
                        scanned_chars_ += unit.bytes;
                        index          += unit.bytes;
                    }
                    if (scanned_units_ < *expected_) {
                        break;
                    }

                    std::size_t const end   = start_ + scanned_chars_;
                    std::string       chunk = buffer_.substr(start_, end - start_);
                    buffer_.erase(0, end);
                    expected_.reset();
                    start_         = 0;
                    scanned_chars_ = 0;
                    scanned_units_ = 0;
                    if (trim(chunk).empty()) {
                        continue;
                    }
                    json parsed = json::parse(chunk, nullptr, false);
                    if (parsed.is_discarded()) {
                        continue;
                    }
                    if (parsed.is_array()) {
                        for (auto& entry : parsed) {
                            frames.push_back(std::move(entry));
                        }
                    } else {
                        frames.push_back(std::move(parsed));
                    }
                }
                return frames;
            }

            std::vector<json> flush() {
                return feed({});
            }

          private:
            std::string                 buffer_;
            std::optional<std::int64_t> expected_;
            std::size_t                 start_          = 0;
            std::size_t                 scanned_chars_  = 0;
            std::int64_t                scanned_units_  = 0;
            bool                        prefix_checked_ = false;
        };

        std::string error_message(std::int64_t code) {
            switch (code) {
                case 1013: return "Gemini hit a temporary error (1013); try again";
                case 1037: return "Gemini usage limit exceeded; switch model or wait for the quota to reset";
                case 1050: return "the selected model is inconsistent with this conversation";
                case 1052: return "the selected model is unavailable or the request shape is outdated";
                case 1060: return "this IP is temporarily blocked by Google; try another network or wait";
                default: return "Gemini returned error code " + std::to_string(code);
            }
        }

        // The answer and reasoning text of one candidate.
        std::pair<std::string, std::string> candidate_text(json const& candidate) {
            static std::regex const card_content(R"(^https?://googleusercontent\.com/card_content/\d+)");
            static std::regex const artifacts(R"(https?://googleusercontent\.com/(?:\w+/)+\d+\n*)");

            std::string text;
            if (auto const* value = nested(candidate, {1, 0}); value != nullptr && value->is_string()) {
                text = value->get<std::string>();
            }
            if (std::regex_search(text, card_content)) {
                if (auto const* value = nested(candidate, {22, 0});
                    value != nullptr && value->is_string() && !value->get<std::string>().empty())
                {
                    text = value->get<std::string>();
                }
            }
            text = std::regex_replace(text, artifacts, "");

            std::string thoughts;
            if (auto const* value = nested(candidate, {37, 0, 0}); value != nullptr && value->is_string()) {
                thoughts = value->get<std::string>();
            }
            return {text, thoughts};
        }

        // Turn StreamGenerate frames into text/thinking deltas.
        class gemini_parser {
          public:
            void feed(std::string_view text, event_sink const& sink) {
                for (auto& part : frames_.feed(text)) {
                    handle_part(part, sink);
                }
            }

            void flush(event_sink const& sink) {
                for (auto& part : frames_.flush()) {
                    handle_part(part, sink);
                }
            }

            std::string full_text;
            std::string full_thoughts;
            std::string cid;
            std::string rid;
            std::string rcid;
            json        metadata = nullptr;
            bool        done     = false;
            std::string error_text;

          private:
            void handle_part(json const& part, event_sink const& sink) {
                if (!part.is_array()) {
                    return;
                }
                if (auto const* code = nested(part, {5, 2, 0, 1, 0});
                    code != nullptr && code->is_number_integer() && code->get<std::int64_t>() != 0)
                {
                    error_text = error_message(code->get<std::int64_t>());
                    sink(error(error_text));
                    return;
                }

                auto const* inner = nested(part, {2});
                if (inner == nullptr || !inner->is_string() || inner->get_ref<std::string const&>().empty()) {
                    return;
                }
                json data = json::parse(inner->get<std::string>(), nullptr, false);
                if (data.is_discarded()) {
                    return;
                }

                if (
                  auto const* turn_metadata = nested(data, {1}); turn_metadata != nullptr && turn_metadata->is_array())
                {
                    if (!turn_metadata->empty() && json_truthy((*turn_metadata)[0])) {
                        cid = string_of((*turn_metadata)[0]);
                    }
                    if (turn_metadata->size() > 1 && json_truthy((*turn_metadata)[1])) {
                        rid = string_of((*turn_metadata)[1]);
                    }
                    this->metadata = *turn_metadata;
                }

                if (auto const* context = nested(data, {25}); context != nullptr && context->is_string()) {
                    json context_metadata = json::array();
                    for (int i = 0; i < 9; ++i) {
                        context_metadata.push_back(nullptr);
                    }
                    context_metadata.push_back(context->get<std::string>());
                    this->metadata = std::move(context_metadata);
                    done           = true;
                }

                auto const* candidates = nested(data, {4});
                if (candidates == nullptr || !candidates->is_array()) {
                    return;
                }
                for (std::size_t index = 0; index < candidates->size(); ++index) {
                    auto const& candidate = (*candidates)[index];
                    if (!candidate.is_array()) {
                        continue;
                    }
                    auto const* candidate_id = nested(candidate, {0});
                    if (candidate_id == nullptr || !json_truthy(*candidate_id)) {
                        continue;
                    }
                    rcid = string_of(*candidate_id);

                    auto const [text, thoughts] = candidate_text(candidate);
                    auto const* indicator       = nested(candidate, {8, 0});
                    bool const  is_final =
                      indicator != nullptr && indicator->is_number_integer() && indicator->get<std::int64_t>() == 2;

                    std::string const key   = rcid.empty() ? "idx_" + std::to_string(index) : rcid;
                    auto const        delta = delta_of(std::string_view{text}, std::string_view{last_text_[key]});
                    auto const        thought_delta =
                      delta_of(std::string_view{thoughts}, std::string_view{last_thoughts_[key]});
                    last_text_[key]     = text;
                    last_thoughts_[key] = thoughts;
                    if (index == 0) {
                        full_text     = text;
                        full_thoughts = thoughts;
                    }
                    if (!delta.empty()) {
                        sink(text_event(delta));
                    }
                    if (!thought_delta.empty()) {
                        sink(thinking(thought_delta));
                    }
                    if (is_final && !text.empty()) {
                        done = true;
                    }
                }
            }

            // Disambiguate the event factory from the local `text` variable.
            static stream_event text_event(std::string_view value) {
                return text(value);
            }

            frame_parser                       frames_;
            std::map<std::string, std::string> last_text_;
            std::map<std::string, std::string> last_thoughts_;
        };

        struct model_spec {
            json header;
            int  number = 1;
        };

        std::optional<std::string> model_alias(std::string_view requested) {
            if (auto const alias = models::resolve_alias("gemini", requested)) {
                return std::string(*alias);
            }
            return std::nullopt;
        }

        model_spec resolve_model(std::string_view requested) {
            std::string const cleaned = lower(trim(requested.empty() ? std::string("auto") : requested));
            std::string       key     = cleaned;
            if (auto const alias = model_alias(cleaned)) {
                key = *alias;
            }
            if (key.empty() || key == "auto") {
                return {.header = nullptr, .number = 1};
            }

            struct triple {
                std::string_view ident;
                int              capacity;
                int              number;
            };

            static std::array<std::pair<std::string_view, triple>, 3> const specs{
              {
               {"gemini-pro", {.ident = "9d8ca3786ebdfbea", .capacity = 1, .number = 3}},
               {"gemini-flash", {.ident = "fbb127bbb056c959", .capacity = 1, .number = 1}},
               {"gemini-flash-lite", {.ident = "cf41b0e0dd7d53e5", .capacity = 1, .number = 6}},
               },
            };
            for (auto const& [name, spec] : specs) {
                if (key != name) {
                    continue;
                }
                json header = json::array({
                  1,
                  nullptr,
                  nullptr,
                  nullptr,
                  std::string(spec.ident),
                  nullptr,
                  nullptr,
                  0,
                  json::array({4, 5, 6, 8}),
                  nullptr,
                  nullptr,
                  spec.capacity,
                  nullptr,
                  nullptr,
                  spec.number,
                });
                return {.header = std::move(header), .number = spec.number};
            }
            return {.header = nullptr, .number = 1};
        }

        void apply_headers(http_request& request, std::vector<std::pair<std::string, std::string>> const& headers) {
            for (auto const& header : headers) {
                request.headers.push_back(header);
            }
        }

        void set_header(std::vector<std::pair<std::string, std::string>>& headers,
                        std::string_view                                  name,
                        std::string                                       value) {
            for (auto& header : headers) {
                if (header.first == name) {
                    header.second = std::move(value);
                    return;
                }
            }
            headers.emplace_back(std::string(name), std::move(value));
        }

        // Read the page's access token, build label, session id, language and push id.
        struct init_tokens {
            std::string at;
            std::string bl;
            std::string sid;
            std::string language;
            std::string push_id;
        };

        // The value of `"key": "value"` in the page's inline JSON.
        std::string_view page_value(std::string_view page, std::string_view key) {
            std::string const marker = "\"" + std::string(key) + "\":";
            auto const        start  = page.find(marker);
            if (start == std::string::npos) {
                return {};
            }
            auto const quote = page.find('"', start + marker.size());
            if (quote == std::string::npos) {
                return {};
            }
            auto const end = page.find('"', quote + 1);
            if (end == std::string::npos) {
                return {};
            }
            return page.substr(quote + 1, end - quote - 1);
        }

        class gemini_provider : public provider {
          public:
            gemini_provider()
              : info_{
                  .name                = "gemini",
                  .label               = "Gemini",
                  .origin              = "https://gemini.google.com",
                  .session_target      = "gemini.google.com",
                  .default_model       = std::string(models::default_model("gemini")),
                  .models              = models::list("gemini"),
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

            [[nodiscard]] std::vector<std::pair<std::string, std::string>> base_headers(
              session::match const& match) const {
                return session::build_headers(
                  match,
                  {
                    { "Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"},
                    {"Referer",                                                info_.origin + "/"},
                });
            }

            init_tokens init(session::match const& match) {
                std::string const key = match.path + "|" + match.profile + "|" + match.cookie_header;
                if (init_cached_ && init_key_ == key) {
                    return init_tokens_;
                }
                std::string const page = request_text(make_init_request(match));
                init_tokens       tokens;
                tokens.at       = page_value(page, "SNlM0e");
                tokens.bl       = page_value(page, "cfb2h");
                tokens.sid      = page_value(page, "FdrFJe");
                tokens.language = page_value(page, "TuX5cc");
                tokens.push_id  = page_value(page, "qKIAYe");
                init_key_       = key;
                init_tokens_    = tokens;
                init_cached_    = true;
                return tokens;
            }

            // Upload one attachment and return Google's file identifier.
            [[nodiscard]] std::string upload(
              std::vector<std::pair<std::string, std::string>> const& headers,
              attachment const&                                       file,
              std::string_view                                        push_id) const {
                std::vector<multipart_field> fields{
                  {.name = "file", .value = file.data, .filename = file.name, .content_type = file.mime},
                };
                std::string       content_type;
                std::string const body = encode_multipart(fields, content_type);

                http_request request;
                request.url             = std::string(kUploadUrl);
                request.method          = "POST";
                request.body            = body;
                request.content_type    = content_type;
                request.timeout_seconds = 300;
                for (auto const& header : headers) {
                    std::string const name = lower(header.first);
                    if (name == "content-type" || name == "content-length" || name == "accept") {
                        continue;
                    }
                    request.headers.push_back(header);
                }
                set_header(request.headers, "Content-Type", content_type);
                set_header(request.headers, "X-Tenant-Id", "bard-storage");
                set_header(request.headers,
                           "Push-ID",
                           push_id.empty() ? std::string("feeds/mcudyrk2a4khkz") : std::string(push_id));
                set_header(request.headers, "Origin", info_.origin);
                set_header(request.headers, "Referer", info_.origin + "/");

                auto const [status, response] = request_raw(request);
                if (status >= 400) {
                    std::string preview = response.substr(0, 200);
                    throw std::runtime_error("upload failed: HTTP " + std::to_string(status) + ": " + preview);
                }
                std::string const identifier = trim(response);
                if (identifier.empty()) {
                    throw std::runtime_error("upload failed: the server returned no file id");
                }
                return identifier;
            }

          private:
            [[nodiscard]] http_request make_init_request(session::match const& match) const {
                http_request request;
                request.url             = info_.origin + std::string(kInitPath);
                request.timeout_seconds = 60;
                apply_headers(request, base_headers(match));
                return request;
            }

            provider_info info_;
            bool          init_cached_ = false;
            std::string   init_key_;
            init_tokens   init_tokens_;
        };

        class gemini_conversation : public conversation {
          public:
            gemini_conversation(gemini_provider&    owner,
                                session::match      match,
                                std::string_view         model,
                                open_options const& options)
              : conversation(std::move(match), model, options),
                owner_(owner) {
                if (options.resume.is_object()) {
                    if (auto const it = options.resume.find("metadata");
                        it != options.resume.end() && it->is_array() && !it->empty())
                    {
                        metadata_ = *it;
                    }
                    if (auto const it = options.resume.find("cid"); it != options.resume.end() && it->is_string()) {
                        cid_ = it->get<std::string>();
                    }
                    if (auto const it = options.resume.find("rid"); it != options.resume.end() && it->is_string()) {
                        rid_ = it->get<std::string>();
                    }
                    if (auto const it = options.resume.find("rcid"); it != options.resume.end() && it->is_string()) {
                        rcid_ = it->get<std::string>();
                    }
                }
                if (metadata_.is_null()) {
                    metadata_ = default_metadata();
                }
                if (cid_.empty()) {
                    cid_ = conversation_id_;
                }
                session_id_ = upper(uuid4());
                reqid_      = random_reqid();
            }

            void send(std::string_view prompt, text::string_views files, event_sink const& sink) override {
                init_tokens tokens;
                try {
                    tokens = owner_.init(match_);
                } catch (std::exception const& failure) {
                    sink(error(failure.what()));
                    return;
                }
                if (tokens.at.empty()) {
                    sink(error(
                      "no Gemini access token found; log in to gemini.google.com in the browser this session comes "
                      "from"));
                    return;
                }

                json                    file_data = nullptr;
                std::vector<attachment> attachments;
                try {
                    attachments = read_attachments(files);
                } catch (file_error const& failure) {
                    sink(error(failure.what()));
                    return;
                }
                if (!attachments.empty()) {
                    json       uploaded = json::array();
                    auto const headers  = owner_.base_headers(match_);
                    for (auto const& file : attachments) {
                        std::string identifier;
                        try {
                            identifier = owner_.upload(headers, file, tokens.push_id);
                        } catch (std::exception const& failure) {
                            sink(error(failure.what()));
                            return;
                        }
                        uploaded.push_back(json::array({json::array({identifier}), file.name}));
                        sink(tool_call("upload", "done", file.name));
                    }
                    file_data = std::move(uploaded);
                }

                std::string const content =
                  (!system_.empty() && turns_ == 0) ? system_ + "\n\n" + std::string(prompt) : std::string(prompt);
                auto const        spec         = resolve_model(model_);
                std::string const request_uuid = upper(uuid4());
                std::string const language     = tokens.language.empty() ? std::string("en") : tokens.language;

                json inner = json::array();
                for (int i = 0; i < 81; ++i) {
                    inner.push_back(nullptr);
                }
                inner[0]  = json::array({content, 0, nullptr, file_data, nullptr, nullptr, 0});
                inner[1]  = json::array({language});
                inner[2]  = metadata_;
                inner[6]  = json::array({1});
                inner[7]  = 1;
                inner[10] = 1;
                inner[11] = 0;
                inner[17] = json::array({json::array({0})});
                inner[18] = 0;
                inner[27] = 1;
                inner[30] = json::array({4});
                inner[41] = json::array({1});
                inner[53] = 0;
                inner[59] = request_uuid;
                inner[61] = json::array();
                inner[68] = 1;
                inner[79] = spec.number;
                inner[80] = 1;

                auto headers = owner_.base_headers(match_);
                set_header(headers, "Accept", "*/*");
                set_header(headers, "Content-Type", "application/x-www-form-urlencoded;charset=utf-8");
                set_header(headers, "Origin", std::string(owner_.origin()));
                set_header(headers, "Referer", owner_.origin() + std::string(kInitPath));
                set_header(headers, "X-Same-Domain", "1");
                set_header(headers, "x-goog-ext-525005358-jspb", "[\"" + request_uuid + "\",1]");
                if (!spec.header.is_null()) {
                    json header = spec.header;
                    header.push_back(1);
                    header.push_back(session_id_);
                    set_header(headers, kModelHeaderKey, header.dump());
                    set_header(headers, "x-goog-ext-73010989-jspb", "[0]");
                    set_header(headers, "x-goog-ext-73010990-jspb", "[0,0,0]");
                }

                std::vector<std::pair<std::string, std::string>> params{
                  {    "hl",               language},
                  {"_reqid", std::to_string(reqid_)},
                  {    "rt",                    "c"},
                };
                reqid_ += 100'000;
                if (!tokens.bl.empty()) {
                    params.emplace_back("bl", tokens.bl);
                }
                if (!tokens.sid.empty()) {
                    params.emplace_back("f.sid", tokens.sid);
                }

                std::string const body = form_body({
                  {   "at",                                   tokens.at},
                  {"f.req", json::array({nullptr, inner.dump()}).dump()}
                });

                http_request request;
                request.url             = std::string(kGenerateUrl) + "?" + form_body(params);
                request.method          = "POST";
                request.body            = body;
                request.content_type    = "application/x-www-form-urlencoded;charset=utf-8";
                request.timeout_seconds = timeout_;
                request.debug           = debug_;
                apply_headers(request, headers);

                gemini_parser parser;
                bool          stopped = false;
                try {
                    std::ignore = stream_chunks(request, [&](std::string_view chunk) {
                        if (stopped) {
                            return;
                        }
                        parser.feed(chunk, sink);
                        if (parser.done && !parser.full_text.empty()) {
                            stopped = true;
                        }
                    });
                    parser.flush(sink);
                } catch (std::exception const& failure) {
                    sink(error(failure.what()));
                    return;
                }
                if (!parser.error_text.empty()) {
                    return;
                }
                if (trim(parser.full_text).empty()) {
                    sink(error("Gemini returned no answer" +
                               (parser.cid.empty() ? std::string{} : " (chat " + parser.cid + ")")));
                    return;
                }

                if (!parser.cid.empty()) {
                    cid_ = parser.cid;
                }
                if (!parser.rid.empty()) {
                    rid_ = parser.rid;
                }
                if (!parser.rcid.empty()) {
                    rcid_ = parser.rcid;
                }
                if (!parser.metadata.is_null()) {
                    metadata_ = parser.metadata;
                }
                if (!cid_.empty()) {
                    conversation_id_ = cid_;
                }
                resume_ = json{
                  {"metadata", metadata_},
                  {     "cid",      cid_},
                  {     "rid",      rid_},
                  {    "rcid",     rcid_},
                };
                ++turns_;
                record("user", prompt);
                record("assistant", parser.full_text);
                sink(done());
            }

          private:
            static json default_metadata() {
                return json::array({"", "", "", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, ""});
            }

            static std::int64_t random_reqid() {
                static thread_local std::mt19937   rng{std::random_device{}()};
                std::uniform_int_distribution<int> distribution(10'000, 99'999);
                return distribution(rng);
            }

            gemini_provider& owner_;
            json             metadata_;
            std::string      cid_;
            std::string      rid_;
            std::string      rcid_;
            std::string      session_id_;
            std::int64_t     reqid_ = 0;
        };

        std::unique_ptr<conversation>
        gemini_provider::open(session::match const& match, std::string_view model, open_options const& options) {
            return std::make_unique<gemini_conversation>(*this, match, model, options);
        }

    } // namespace

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization)
    bool const gemini_registered = register_provider(std::make_unique<gemini_provider>());

} // namespace dana::ai
