#include "../diagnostics.hpp"
#include "../files.hpp"
#include "../http.hpp"
#include "../models.hpp"
#include "../sse.hpp"
#include "../uuid.hpp"
#include "provider.hpp"
#include "registry.hpp"
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace prompt::ai {

namespace {

using nlohmann::json;

constexpr std::string_view kAskPath = "/rest/sse/perplexity_ask";
constexpr std::string_view kUploadPath = "/rest/uploads/batch_create_upload_urls";
constexpr std::string_view kUploadSubscribePath = "/rest/sse/attachment_processing/subscribe";
constexpr std::string_view kApiVersion = "2.18";

//: How many stored turns a fresh process replays when it cannot reattach to
//: the thread id (the site keeps the transcript, so this is only a fallback).
constexpr std::size_t kMaxReplay = 40;

//: The block use-cases the current web client advertises.  The schematized API
//: validates this list, so it mirrors the browser's request body.
constexpr std::array<std::string_view, 32> kSupportedBlockUseCases{
    {
        "answer_modes",
        "media_items",
        "knowledge_cards",
        "inline_entity_cards",
        "place_widgets",
        "finance_widgets",
        "sports_widgets",
        "news_widgets",
        "shopping_widgets",
        "jobs_widgets",
        "search_result_widgets",
        "inline_images",
        "inline_assets",
        "placeholder_cards",
        "diff_blocks",
        "inline_knowledge_cards",
        "entity_group_v2",
        "refinement_filters",
        "canvas_mode",
        "maps_preview",
        "answer_tabs",
        "price_comparison_widgets",
        "preserve_latex",
        "generic_onboarding_widgets",
        "in_context_suggestions",
        "pending_followups",
        "inline_claims",
        "unified_assets",
        "workflow_steps",
        "workflow_widgets",
        "navigation_results",
        "background_agents",
    },
};

std::string trim(std::string_view value) {
    std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
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
    if (zone != nullptr && *zone != '\0') return zone;
    std::error_code ec;
    auto const target = std::filesystem::read_symlink("/etc/localtime", ec);
    if (ec) return "UTC";
    std::string const text = target.string();
    auto const marker = text.find("/zoneinfo/");
    if (marker == std::string::npos) return "UTC";
    return text.substr(marker + 10);
}

// Friendly names accepted by --model; the site's own ids also work.
std::string model_alias(std::string const& requested) {
    if (auto const alias = models::resolve_alias("perplexity", requested)) return std::string(*alias);
    return requested;
}

// The string member `key` of an object, or "" when it is absent or not a string.
std::string string_value(json const& object, std::string_view key) {
    if (!object.is_object()) return {};
    auto const it = object.find(std::string(key));
    if (it == object.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

json object_or_empty(json const& object, std::string_view key) {
    if (!object.is_object()) return json::object();
    auto const it = object.find(std::string(key));
    if (it == object.end() || !it->is_object()) return json::object();
    return *it;
}

json array_or_empty(json const& object, std::string_view key) {
    if (!object.is_object()) return json::array();
    auto const it = object.find(std::string(key));
    if (it == object.end() || !it->is_array()) return json::array();
    return *it;
}

// Python truthiness for a JSON member (the stream flags are booleans, but an
// older frame may carry a non-empty string or a non-zero number).
bool truthy(json const& object, std::string_view key) {
    if (!object.is_object()) return false;
    auto const it = object.find(std::string(key));
    if (it == object.end() || it->is_null()) return false;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_string()) return !it->get<std::string>().empty();
    if (it->is_number_integer()) return it->get<long long>() != 0;
    if (it->is_number_unsigned()) return it->get<unsigned long long>() != 0;
    if (it->is_number_float()) return it->get<double>() != 0.0;
    if (it->is_array() || it->is_object()) return !it->empty();
    return true;
}

json to_chunks(json const& value) {
    json out = json::array();
    if (!value.is_array()) return out;
    for (auto const& chunk : value) {
        if (chunk.is_string()) out.push_back(chunk.get<std::string>());
        else
            out.push_back(chunk.dump());
    }
    return out;
}

std::string join_chunks(json const& acc) {
    if (!acc.is_object()) return {};
    auto const it = acc.find("chunks");
    if (it == acc.end() || !it->is_array()) return {};
    std::string out;
    for (auto const& chunk : *it) {
        if (chunk.is_string()) out += chunk.get<std::string>();
    }
    return out;
}

bool is_answer_usage(std::string const& usage) {
    static std::regex const pattern(R"(^ask_text(?:_\d+(?:_markdown)?)?$)");
    return std::regex_match(usage, pattern);
}

bool is_answer_item(json const& item) {
    if (!item.is_object()) return false;
    json const payload = object_or_empty(item, "payload");
    json const text_payload = object_or_empty(payload, "text_payload");
    std::string variant = string_value(text_payload, "variant");
    if (variant.empty()) variant = string_value(item, "variant");
    return variant == "answer";
}

void seed_item(json& acc, json const& item) {
    json const payload = object_or_empty(item, "payload");
    json const text_payload = object_or_empty(payload, "text_payload");
    if (text_payload.contains("chunks") && text_payload["chunks"].is_array() && !text_payload["chunks"].empty()) {
        acc["chunks"] = to_chunks(text_payload["chunks"]);
    } else {
        std::string const text = string_value(text_payload, "text");
        if (!text.empty()) acc["chunks"] = json::array({text});
    }
}

void apply_markdown_diff(json& acc, json const& patches) {
    static std::regex const chunk_path(R"(^/chunks/(\d+)$)");
    for (auto const& patch : patches) {
        if (!patch.is_object()) continue;
        std::string const path = string_value(patch, "path");
        json const value = patch.contains("value") ? patch["value"] : json();
        if (path.empty()) {
            if (value.is_string()) {
                acc["chunks"] = json::array({value});
            } else if (value.is_array()) {
                acc["chunks"] = to_chunks(value);
            } else if (value.is_object()) {
                if (value.contains("chunks") && value["chunks"].is_array()) {
                    acc["chunks"] = to_chunks(value["chunks"]);
                } else if (value.contains("answer") && value["answer"].is_string()) {
                    acc["chunks"] = json::array({value["answer"]});
                } else {
                    acc["chunks"] = json::array();
                }
            } else {
                acc["chunks"] = json::array();
            }
            continue;
        }
        std::smatch match;
        if (std::regex_match(path, match, chunk_path) && value.is_string()) {
            auto const index = static_cast<std::size_t>(std::stoul(match[1].str()));
            if (!acc.contains("chunks") || !acc["chunks"].is_array()) acc["chunks"] = json::array();
            auto& chunks = acc["chunks"];
            while (chunks.size() <= index) chunks.push_back("");
            chunks[index] = value;
        }
    }
}

void apply_workflow_block(json& state, json const& workflow) {
    json const steps = array_or_empty(workflow, "steps");
    for (std::size_t step_index = 0; step_index < steps.size(); ++step_index) {
        json const items = array_or_empty(steps[step_index], "items");
        for (std::size_t item_index = 0; item_index < items.size(); ++item_index) {
            if (!is_answer_item(items[item_index])) continue;
            std::string const key = "workflow_root:" + std::to_string(step_index) + ":" + std::to_string(item_index);
            if (!state.contains(key)) state[key] = json{{"chunks", json::array()}};
            seed_item(state[key], items[item_index]);
        }
    }
}

void apply_workflow_diff(json& state, json const& patches) {
    static std::regex const step_path(R"(^/steps/(\d+)$)");
    static std::regex const item_path(R"(^/steps/(\d+)/items/(\d+)$)");
    static std::regex const chunk_path(R"(^/steps/(\d+)/items/(\d+)/payload/text_payload/chunks/(\d+)$)");
    static std::regex const text_path(R"(^/steps/(\d+)/items/(\d+)/payload/text_payload/text$)");
    for (auto const& patch : patches) {
        if (!patch.is_object()) continue;
        std::string const path = string_value(patch, "path");
        json const value = patch.contains("value") ? patch["value"] : json();
        std::smatch match;
        if (std::regex_match(path, match, step_path)) {
            auto const step_index = static_cast<std::size_t>(std::stoul(match[1].str()));
            json const items = array_or_empty(value, "items");
            for (std::size_t item_index = 0; item_index < items.size(); ++item_index) {
                if (!is_answer_item(items[item_index])) continue;
                std::string const key =
                    "workflow_root:" + std::to_string(step_index) + ":" + std::to_string(item_index);
                if (!state.contains(key)) state[key] = json{{"chunks", json::array()}};
                seed_item(state[key], items[item_index]);
            }
            continue;
        }
        if (std::regex_match(path, match, item_path)) {
            if (is_answer_item(value)) {
                std::string const key = "workflow_root:" + match[1].str() + ":" + match[2].str();
                if (!state.contains(key)) state[key] = json{{"chunks", json::array()}};
                seed_item(state[key], value);
            }
            continue;
        }
        if (std::regex_match(path, match, chunk_path) && value.is_string()) {
            std::string const key = "workflow_root:" + match[1].str() + ":" + match[2].str();
            if (!state.contains(key)) continue;
            auto& acc = state[key];
            auto const index = static_cast<std::size_t>(std::stoul(match[3].str()));
            if (!acc.contains("chunks") || !acc["chunks"].is_array()) acc["chunks"] = json::array();
            auto& chunks = acc["chunks"];
            while (chunks.size() <= index) chunks.push_back("");
            chunks[index] = value;
            continue;
        }
        if (std::regex_match(path, match, text_path) && value.is_string() && !value.get<std::string>().empty()) {
            std::string const key = "workflow_root:" + match[1].str() + ":" + match[2].str();
            if (!state.contains(key)) continue;
            auto& acc = state[key];
            if (join_chunks(acc).empty()) acc["chunks"] = json::array({value});
        }
    }
}

void plan_goals(json const& block, std::vector<std::string>& out) {
    json const plan = object_or_empty(block, "plan_block");
    for (auto const& goal : array_or_empty(plan, "goals")) {
        std::string const text = string_value(goal, "description");
        if (!text.empty()) out.push_back(text);
    }
    json const diff = object_or_empty(block, "diff_block");
    for (auto const& patch : array_or_empty(diff, "patches")) {
        json const value = patch.is_object() && patch.contains("value") ? patch["value"] : json();
        if (!value.is_object()) continue;
        for (auto const& goal : array_or_empty(value, "goals")) {
            std::string const text = string_value(goal, "description");
            if (!text.empty()) out.push_back(text);
        }
    }
}

void search_steps(json const& block, std::vector<std::string>& out) {
    json const plan = object_or_empty(block, "plan_block");
    for (auto const& step : array_or_empty(plan, "steps")) {
        std::string const step_type = string_value(step, "step_type");
        if (step_type == "SEARCH_WEB") {
            json const content = object_or_empty(step, "search_web_content");
            for (auto const& query : array_or_empty(content, "queries")) {
                std::string const text = string_value(query, "query");
                if (!text.empty()) out.push_back("Searching: " + text);
            }
        } else if (step_type == "READ_RESULTS") {
            json const content = object_or_empty(step, "read_results_content");
            for (auto const& url : array_or_empty(content, "urls")) {
                if (url.is_string() && !url.get<std::string>().empty())
                    out.push_back("Reading: " + url.get<std::string>());
            }
        }
    }
}

// Recover the answer from the COMPLETED frame's double-encoded step blob.
std::optional<std::string> answer_from_final_text(std::string const& text) {
    std::string trimmed = trim(text);
    if (trimmed.empty()) return std::nullopt;
    if (!trimmed.starts_with('{') && !trimmed.starts_with('[')) return trimmed;
    json const parsed = json::parse(trimmed, nullptr, false);
    if (parsed.is_discarded()) return std::nullopt;
    json const steps = parsed.is_array() ? parsed : json::array({parsed});
    for (auto const& step : steps) {
        if (!step.is_object()) continue;
        std::string step_type = string_value(step, "step_type");
        if (step_type.empty()) step_type = string_value(step, "stepType");
        if (!step_type.empty() && step_type != "FINAL") continue;
        std::optional<std::string> raw;
        bool skip = false;
        if (step.contains("content")) {
            auto const& content = step["content"];
            if (content.is_string()) {
                raw = content.get<std::string>();
            } else if (content.is_object() && content.contains("answer")) {
                if (content["answer"].is_string()) raw = content["answer"].get<std::string>();
                else
                    skip = true;
            }
        }
        if (skip) continue;
        if (!raw && step.contains("answer") && step["answer"].is_string()) raw = step["answer"].get<std::string>();
        if (!raw) continue;
        std::string inner = trim(*raw);
        if (inner.empty()) continue;
        if (inner.starts_with('{') || inner.starts_with('[')) {
            json const object = json::parse(inner, nullptr, false);
            if (object.is_discarded()) return inner;
            if (object.is_object()) {
                std::string answer = string_value(object, "answer");
                if (!trim(answer).empty()) return answer;
                if (object.contains("chunks") && object["chunks"].is_array() && !object["chunks"].empty()) {
                    std::string joined;
                    for (auto const& chunk : object["chunks"]) {
                        if (chunk.is_string()) joined += chunk.get<std::string>();
                    }
                    return joined;
                }
                if (object.contains("structured_answer") && object["structured_answer"].is_array()) {
                    std::string joined;
                    for (auto const& block : object["structured_answer"]) {
                        if (block.is_object()) joined += string_value(block, "text");
                    }
                    if (!trim(joined).empty()) return joined;
                }
            }
        }
        return inner;
    }
    return std::nullopt;
}

void apply_headers(http_request& request, std::vector<std::pair<std::string, std::string>> const& headers) {
    for (auto const& header : headers) request.headers.push_back(header);
}

void set_header(std::vector<std::pair<std::string, std::string>>& headers, std::string_view name, std::string value) {
    for (auto& header : headers) {
        if (std::string_view(header.first) == name) {
            header.second = std::move(value);
            return;
        }
    }
    headers.emplace_back(std::string(name), std::move(value));
}

// Reconstruct the cumulative answer from Perplexity's block stream.
class perplexity_parser {
  public:
    void feed(json const& event, event_sink const& sink);

    [[nodiscard]] bool done() const { return done_; }
    [[nodiscard]] bool failed() const { return !error_.empty(); }

    std::string full_answer;
    std::string backend_uuid;
    std::string read_write_token;
    std::string thread_slug;
    std::string thread_title;

  private:
    void block(json const& value, event_sink const& sink);
    [[nodiscard]] std::string longest() const;

    json markdown_ = json::object();
    std::string primary_usage_;
    std::size_t seen_len_ = 0;
    std::string status_;
    std::string error_;
    std::string last_text_;
    bool done_ = false;
    std::set<std::string> seen_thinking_;
};

void perplexity_parser::feed(json const& event, event_sink const& sink) {
    if (!event.is_object()) return;
    std::string const error_code = string_value(event, "error_code");
    std::string const error_message = string_value(event, "error_message");
    if (!error_code.empty() || !error_message.empty()) {
        std::string const message = !error_message.empty() ? error_message : "Perplexity error: " + error_code;
        error_ = message;
        done_ = true;
        sink(ai::error(message));
        return;
    }
    if (std::string value = string_value(event, "backend_uuid"); !value.empty()) backend_uuid = std::move(value);
    if (std::string value = string_value(event, "read_write_token"); read_write_token.empty() && !value.empty())
        read_write_token = std::move(value);
    if (std::string value = string_value(event, "thread_url_slug"); thread_slug.empty() && !value.empty())
        thread_slug = std::move(value);
    if (std::string value = string_value(event, "thread_title"); !value.empty()) thread_title = std::move(value);
    if (std::string value = string_value(event, "text"); !value.empty()) last_text_ = std::move(value);
    if (std::string value = string_value(event, "status"); !value.empty()) status_ = std::move(value);

    json const blocks = array_or_empty(event, "blocks");
    for (auto const& value : blocks) block(value, sink);

    std::string const current = longest();
    if (current.size() > seen_len_) {
        std::string const delta = current.substr(seen_len_);
        full_answer = current;
        seen_len_ = current.size();
        sink(text(delta));
    }

    // Legacy frames carried the answer as a plain `text` field.
    if (primary_usage_.empty() && markdown_.empty() && blocks.empty()) {
        std::string const plain = trim(string_value(event, "text"));
        if (!plain.empty() && !plain.starts_with('{') && !plain.starts_with('[') && plain.size() > seen_len_) {
            std::string const delta = plain.substr(seen_len_);
            full_answer = plain;
            seen_len_ = plain.size();
            sink(text(delta));
        }
    }

    if (status_ == "COMPLETED" || truthy(event, "final") || truthy(event, "final_sse_message") ||
        truthy(event, "text_completed")) {
        done_ = true;
        if (trim(full_answer).empty()) {
            std::string source = string_value(event, "text");
            if (source.empty()) source = last_text_;
            if (auto const recovered = answer_from_final_text(source)) {
                std::string const delta = recovered->size() > seen_len_ ? recovered->substr(seen_len_) : std::string{};
                full_answer = *recovered;
                seen_len_ = recovered->size();
                if (!delta.empty()) sink(text(delta));
            }
        }
    }
}

void perplexity_parser::block(json const& value, event_sink const& sink) {
    std::string const usage = string_value(value, "intended_usage");
    if (usage == "pro_search_steps") {
        std::vector<std::string> steps;
        search_steps(value, steps);
        for (auto const& text_value : steps) {
            if (seen_thinking_.insert(text_value).second) sink(thinking(text_value));
        }
    }
    if (usage == "plan") {
        std::vector<std::string> goals;
        plan_goals(value, goals);
        for (auto const& text_value : goals) {
            if (seen_thinking_.insert(text_value).second) sink(thinking(text_value));
        }
    }

    if (truthy(value, "workflow_block")) {
        apply_workflow_block(markdown_, value["workflow_block"]);
        return;
    }
    json const diff = object_or_empty(value, "diff_block");
    std::string const field = string_value(diff, "field");
    if (field == "workflow_block") {
        apply_workflow_diff(markdown_, array_or_empty(diff, "patches"));
        return;
    }
    if (!is_answer_usage(usage)) return;
    if (!field.empty() && field != "markdown_block") return;

    if (!markdown_.contains(usage)) markdown_[usage] = json{{"chunks", json::array()}};
    auto& acc = markdown_[usage];
    if (truthy(diff, "patches")) {
        apply_markdown_diff(acc, diff["patches"]);
    } else {
        json const markdown = object_or_empty(value, "markdown_block");
        if (markdown.contains("chunks") && markdown["chunks"].is_array() && !markdown["chunks"].empty()) {
            acc["chunks"] = to_chunks(markdown["chunks"]);
        } else {
            std::string const answer = string_value(markdown, "answer");
            if (!answer.empty()) acc["chunks"] = json::array({answer});
        }
    }
    if (usage == "ask_text") primary_usage_ = "ask_text";
    else if (primary_usage_.empty())
        primary_usage_ = usage;
}

std::string perplexity_parser::longest() const {
    std::string best;
    if (!primary_usage_.empty()) {
        auto const it = markdown_.find(primary_usage_);
        if (it != markdown_.end()) best = join_chunks(*it);
    }
    for (auto const& [key, acc] : markdown_.items()) {
        std::string const joined = join_chunks(acc);
        if (joined.size() > best.size()) best = joined;
    }
    return best;
}

class perplexity_provider : public provider {
  public:
    perplexity_provider()
        : info_{
              .name = "perplexity",
              .label = "Perplexity",
              .origin = "https://www.perplexity.ai",
              .session_target = "www.perplexity.ai",
              .default_model = std::string(models::default_model("perplexity")),
              .models = models::list("perplexity"),
              .inline_limit = 256UL * 1024UL,
              .needs_session = true,
              .api = false,
              .local = false,
              .recordable = true,
              .prompt_file_upload = true,
              .auto_attach_by_size = true,
              .discovers_model = false,
              .base_url = "",
              .base_url_env = "",
              .key_env = "",
              .model_env = "",
              .default_base_url = "",
          } {}

    [[nodiscard]] provider_info const& info() const override { return info_; }

    std::unique_ptr<conversation> open(session::match const& match, std::string const& model,
                                       open_options const& options) override;

    [[nodiscard]] std::string const& origin() const { return info_.origin; }

    [[nodiscard]] std::vector<std::pair<std::string, std::string>> base_headers(session::match const& match) const {
        auto headers = session::build_headers(match, {
                                                         {"Accept", "application/json"},
                                                         {"Content-Type", "application/json"},
                                                         {"Origin", info_.origin},
                                                         {"Referer", info_.origin + "/"},
                                                     });
        auto const token = session::storage_value(match, {"accessToken", "access_token", "userToken"});
        bool has_authorization = false;
        for (auto const& header : headers) {
            if (header.first == "Authorization") has_authorization = true;
        }
        if (token && !token->empty() && !has_authorization) headers.emplace_back("Authorization", "Bearer " + *token);
        return headers;
    }

    [[nodiscard]] std::vector<std::pair<std::string, std::string>> ask_headers(session::match const& match) const {
        auto headers = base_headers(match);
        set_header(headers, "Accept", "text/event-stream, application/json");
        set_header(headers, "x-perplexity-request-endpoint", info_.origin + std::string(kAskPath));
        set_header(headers, "x-perplexity-request-reason", "ask-query-state-provider");
        set_header(headers, "x-perplexity-request-try-number", "1");
        return headers;
    }

    // Upload one attachment and return its S3 object URL.
    [[nodiscard]] std::string upload(std::vector<std::pair<std::string, std::string>> const& headers,
                                     attachment const& file, bool debug) const {
        std::string const file_uuid = uuid4();
        json const entry{
            {"filename", file.name}, {"content_type", file.mime},
            {"source", "default"},   {"file_size", static_cast<long long>(file.data.size())},
            {"force_image", false},  {"skip_parsing", false},
        };
        json const body{{"files", json{{file_uuid, entry}}}};

        http_request create;
        create.url =
            info_.origin + std::string(kUploadPath) + "?version=" + std::string(kApiVersion) + "&source=default";
        create.method = "POST";
        create.json_body = body.dump();
        create.timeout_seconds = 120;
        apply_headers(create, headers);
        json const data = request_json(create);
        json const results = object_or_empty(data, "results");
        auto const result_it = results.find(file_uuid);
        if (result_it == results.end() || !result_it->is_object() || !result_it->contains("s3_bucket_url") ||
            !(*result_it)["s3_bucket_url"].is_string())
            throw std::runtime_error("upload failed: " + data.dump());
        json const& result = *result_it;

        std::vector<multipart_field> fields;
        if (result.contains("fields") && result["fields"].is_object()) {
            for (auto const& [name, value] : result["fields"].items()) {
                fields.push_back({.name = name, .value = value.is_string() ? value.get<std::string>() : value.dump()});
            }
        }
        fields.push_back({.name = "file", .value = file.data, .filename = file.name, .content_type = file.mime});
        std::string content_type;
        std::string const payload = encode_multipart(fields, content_type);

        http_request store;
        store.url = result["s3_bucket_url"].get<std::string>();
        store.method = "POST";
        store.body = payload;
        store.content_type = content_type;
        store.timeout_seconds = 300;
        store.headers.emplace_back("Content-Type", content_type);
        auto const [status, response] = request_raw(store);
        if (status >= 400)
            throw std::runtime_error("upload failed: HTTP " + std::to_string(status) + ": " + response.substr(0, 200));

        // The processing subscription is what lets documents be read; images
        // are usable without it, so a refusal is reported but not fatal.
        try {
            http_request subscribe;
            subscribe.url = info_.origin + std::string(kUploadSubscribePath);
            subscribe.method = "POST";
            subscribe.json_body = json{{"file_uuids", json::array({file_uuid})}}.dump();
            subscribe.timeout_seconds = 120;
            apply_headers(subscribe, headers);
            request_json(subscribe);
        } catch (std::exception const& failure) {
            note(file.name + ": attachment processing subscription failed (" + failure.what() +
                     "); the site may not read this document",
                 debug, "perplexity");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4000));
        std::string url = string_value(result, "s3_object_url");
        if (url.contains("image/upload/")) url = replace_all(url, "image/upload/", "");
        return url;
    }

  private:
    provider_info info_;
};

class perplexity_conversation : public conversation {
  public:
    perplexity_conversation(perplexity_provider& owner, session::match match, std::string model,
                            open_options const& options)
        : conversation(std::move(match), std::move(model), options), owner_(owner),
          headers_(owner_.ask_headers(match_)) {
        std::string const requested = model_.empty() ? owner_.info().default_model : model_;
        model_preference_ = model_alias(requested);
        backend_uuid_ = resume_string(options.resume, "backend_uuid");
        read_write_token_ = resume_string(options.resume, "read_write_token");
        thread_slug_ = resume_string(options.resume, "thread_slug");
        if (thread_slug_.empty()) thread_slug_ = conversation_id_;
    }

    void send(std::string const& prompt, std::vector<std::string> const& files, event_sink const& sink) override {
        bool has_cookie = false;
        bool has_authorization = false;
        for (auto const& header : headers_) {
            if (header.first == "Cookie" && !header.second.empty()) has_cookie = true;
            if (header.first == "Authorization" && !header.second.empty()) has_authorization = true;
        }
        if (!has_cookie && !has_authorization) {
            sink(error("no signed-in session found for www.perplexity.ai; log in once"));
            return;
        }
        auto headers = headers_;
        set_header(headers, "x-request-id", uuid4());

        std::vector<std::string> attachments;
        std::vector<attachment> uploaded;
        try {
            uploaded = read_attachments(files);
        } catch (file_error const& failure) {
            sink(error(failure.what()));
            return;
        }
        for (auto const& file : uploaded) {
            std::string url;
            try {
                url = owner_.upload(owner_.base_headers(match_), file, debug_);
            } catch (std::exception const& failure) {
                sink(error(failure.what()));
                return;
            }
            if (!url.empty()) attachments.push_back(url);
            sink(tool_call("upload", "done", file.name));
        }

        std::string const query = build_query(prompt);
        json const body = build_body(query, prompt, attachments);
        perplexity_parser parser;
        bool stopped = false;
        try {
            http_request request;
            request.url = owner_.origin() + std::string(kAskPath);
            request.method = "POST";
            request.json_body = body.dump();
            request.timeout_seconds = timeout_;
            request.debug = debug_;
            apply_headers(request, headers);
            sse_decoder decoder;
            stream_lines(request, [&](std::string_view line) {
                if (stopped) return;
                for (auto const& payload : decoder.feed(line)) {
                    json const event = json::parse(payload, nullptr, false);
                    if (event.is_discarded()) {
                        sink(raw(payload));
                        continue;
                    }
                    parser.feed(event, sink);
                    if (parser.done()) {
                        stopped = true;
                        return;
                    }
                }
            });
        } catch (std::exception const& failure) {
            sink(error(failure.what()));
            return;
        }
        if (parser.failed()) return;
        if (trim(parser.full_answer).empty()) {
            sink(error("Perplexity returned no answer" +
                       (parser.thread_slug.empty() ? std::string{} : " (thread " + parser.thread_slug + ")")));
            return;
        }

        if (!parser.backend_uuid.empty()) backend_uuid_ = parser.backend_uuid;
        if (!parser.read_write_token.empty()) read_write_token_ = parser.read_write_token;
        if (!parser.thread_slug.empty()) thread_slug_ = parser.thread_slug;
        if (!thread_slug_.empty() || !backend_uuid_.empty())
            conversation_id_ = !thread_slug_.empty() ? thread_slug_ : backend_uuid_;
        resume_ = json{
            {"backend_uuid", backend_uuid_},
            {"read_write_token", read_write_token_},
            {"thread_slug", thread_slug_},
        };
        ++turns_;
        record("user", prompt);
        record("assistant", parser.full_answer);
        sink(done());
    }

  private:
    static std::string resume_string(json const& resume, std::string_view key) {
        if (!resume.is_object()) return {};
        auto const it = resume.find(std::string(key));
        if (it == resume.end() || !it->is_string()) return {};
        return it->get<std::string>();
    }

    // The text the site searches/answers, folding in a system prompt.
    [[nodiscard]] std::string build_query(std::string const& prompt) const {
        if (!backend_uuid_.empty()) {
            if (!system_.empty() && turns_ == 0) return system_ + "\n\n" + prompt;
            return prompt;
        }
        if (system_.empty() && history_.empty()) return prompt;
        std::vector<std::string> parts;
        if (turns_ == 0 && !system_.empty()) parts.push_back(system_);
        if (!history_.empty()) {
            std::string lines =
                "This is a continuation of the conversation so far. Reply to the final user message below.\n\n";
            std::size_t const start = history_.size() > kMaxReplay ? history_.size() - kMaxReplay : 0;
            for (std::size_t i = start; i < history_.size(); ++i) {
                auto const& turn = history_[i];
                if (!turn.is_object()) continue;
                std::string const who = turn.value("role", "") == "user" ? "User" : "Assistant";
                lines += who + ": " + turn.value("text", "") + "\n";
            }
            lines += "\nUser: " + prompt;
            parts.push_back(lines);
        } else {
            parts.push_back(prompt);
        }
        std::string query;
        for (auto const& part : parts) {
            if (part.empty()) continue;
            if (!query.empty()) query += "\n\n";
            query += part;
        }
        return query;
    }

    [[nodiscard]] json build_body(std::string const& query, std::string const& prompt,
                                  std::vector<std::string> const& attachments) const {
        json cases = json::array();
        for (auto const& value : kSupportedBlockUseCases) cases.push_back(std::string(value));
        json params{
            {"attachments", attachments},
            {"language", "en-US"},
            {"timezone", timezone_name()},
            {"search_focus", "internet"},
            {"sources", json::array({"web"})},
            {"frontend_uuid", uuid4()},
            {"mode", "copilot"},
            {"model_preference", model_preference_},
            {"is_related_query", false},
            {"is_sponsored", false},
            {"frontend_context_uuid", uuid4()},
            {"prompt_source", "user"},
            {"query_source", backend_uuid_.empty() ? "home" : "followup"},
            {"is_incognito", false},
            {"local_search_enabled", false},
            {"use_schematized_api", true},
            {"send_back_text_in_streaming_api", false},
            {"supported_block_use_cases", std::move(cases)},
            {"client_coordinates", nullptr},
            {"mentions", json::array()},
            {"dsl_query", prompt},
            {"skip_search_enabled", true},
            {"is_nav_suggestions_disabled", false},
            {"source", "default"},
            {"always_search_override", false},
            {"override_no_search", false},
            {"client_search_results_cache_key", uuid4()},
            {"should_ask_for_mcp_tool_confirmation", true},
            {"supports_tool_approval_modal", true},
            {"browser_agent_allow_once_from_toggle", false},
            {"force_enable_browser_agent", false},
            {"supported_features", json::array({"browser_agent_permission_banner_v1.1"})},
            {"extended_context", false},
            {"version", std::string(kApiVersion)},
            {"rum_session_id", uuid4()},
        };
        if (!backend_uuid_.empty()) {
            params["last_backend_uuid"] = backend_uuid_;
            if (!read_write_token_.empty()) params["read_write_token"] = read_write_token_;
        }
        return json{{"query_str", query}, {"params", std::move(params)}};
    }

    perplexity_provider& owner_;
    std::vector<std::pair<std::string, std::string>> headers_;
    std::string model_preference_;
    std::string backend_uuid_;
    std::string read_write_token_;
    std::string thread_slug_;
};

std::unique_ptr<conversation> perplexity_provider::open(session::match const& match, std::string const& model,
                                                        open_options const& options) {
    return std::make_unique<perplexity_conversation>(*this, match, model, options);
}

} // namespace

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
bool const perplexity_registered = register_provider(std::make_unique<perplexity_provider>());

} // namespace prompt::ai
