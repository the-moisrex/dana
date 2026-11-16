#include "openai_compat.hpp"
#include "../diagnostics.hpp"
#include "../files.hpp"
#include "../http.hpp"
#include "../keys.hpp"
#include "../promptfile.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <map>
#include <nlohmann/json.hpp>
#include <regex>
#include <set>
#include <string>

namespace prompt::ai {

namespace {

using nlohmann::json;

constexpr std::size_t kMaxReplay = 40;
constexpr std::size_t kMaxInline = 16UL * 1024UL * 1024UL;

std::string env_value(std::string_view name) {
    std::string key(name);
    char const* raw = std::getenv(key.c_str());
    return raw == nullptr ? std::string{} : std::string(raw);
}

std::string lower(std::string_view value) {
    std::string out(value);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string trim(std::string_view value) {
    std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    std::size_t last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

bool is_text_extension(std::string_view name) {
    static const std::set<std::string> extensions{
        ".c",   ".h",     ".hpp", ".cpp",    ".cc",   ".cxx", ".py",    ".sh",   ".bash",  ".md",    ".txt",
        ".rst", ".toml",  ".ini", ".cfg",    ".conf", ".csv", ".tsv",   ".json", ".jsonl", ".yaml",  ".yml",
        ".xml", ".html",  ".css", ".js",     ".ts",   ".tsx", ".jsx",   ".sql",  ".diff",  ".patch", ".log",
        ".env", ".cmake", ".mk",  ".gradle", ".rs",   ".go",  ".java",  ".kt",   ".rb",    ".pl",    ".lua",
        ".php", ".vim",   ".el",  ".fish",   ".zsh",  ".nix", ".proto",
    };
    auto const dot = name.rfind('.');
    if (dot == std::string_view::npos) return false;
    return extensions.contains(lower(name.substr(dot)));
}

bool is_text_mime(std::string_view mime) {
    static const std::set<std::string> types{
        "application/json", "application/xml",        "application/yaml", "application/x-yaml",
        "application/toml", "application/javascript", "application/x-sh", "application/x-python",
        "application/sql",  "application/ld+json",    "image/svg+xml",
    };
    return mime.starts_with("text/") || types.contains(std::string(mime));
}

std::optional<std::string> as_text(attachment const& value) {
    std::string mime = value.mime;
    auto const semicolon = mime.find(';');
    if (semicolon != std::string::npos) mime = mime.substr(0, semicolon);
    mime = lower(trim(mime));
    if (!is_text_mime(mime) && !is_text_extension(value.name)) return std::nullopt;
    if (value.data.contains('\0')) return std::nullopt;
    return value.data;
}

std::string error_text(json const& error) {
    if (error.is_object()) {
        if (error.contains("message") && error["message"].is_string()) return error["message"].get<std::string>();
        if (error.contains("error") && error["error"].is_string()) return error["error"].get<std::string>();
        return error.dump();
    }
    return error.is_string() ? error.get<std::string>() : error.dump();
}

std::string normalise_base_impl(std::string_view url) {
    std::string text = trim(url);
    while (!text.empty() && text.back() == '/') text.pop_back();
    if (text.empty()) throw std::runtime_error("no API endpoint configured");
    if (!text.contains("://")) text = "http://" + text;
    static std::regex const version(R"(/v\d+$)");
    if (!std::regex_search(text, version)) text += "/v1";
    return text;
}

class openai_conversation : public conversation {
  public:
    openai_conversation(provider& owner, session::match match, std::string model, const open_options& options)
        : conversation(std::move(match), std::move(model), options), owner_(owner),
          base_url_(owner.base_url(options.base_url)), api_key_(owner.api_key(options.api_key)),
          params_(parse_params(options.params)) {}

    void send(std::string const& prompt, std::vector<std::string> const& files, event_sink const& sink) override {
        std::vector<attachment> attachments;
        try {
            for (auto const& path : files) attachments.push_back(read_attachment(path));
        } catch (file_error const& failure) {
            sink(prompt::ai::error(failure.what()));
            return;
        }

        json content = json::array();
        std::string message = prompt;
        std::vector<std::string> announced;
        bool has_parts = false;
        for (auto const& attachment : attachments) {
            announced.push_back(attachment.name);
            if (attachment.mime.starts_with("image/")) {
                has_parts = true;
                content.push_back({
                    {"type", "image_url"},
                    {
                        "image_url",
                        {{"url", "data:" + attachment.mime + ";base64," + base64_encode(attachment.data)}},
                    },
                });
                continue;
            }
            auto const text = as_text(attachment);
            if (!text) {
                sink(error(attachment.name + ": " + owner_.info().label +
                           " takes text and image attachments inline; convert it to text, embed it in the "
                           "chain with `.files`, or mention its path"));
                return;
            }
            if (text->size() > kMaxInline) {
                sink(error(attachment.name + ": larger than 16 MiB and this provider has no upload endpoint; "
                                             "split or trim it first"));
                return;
            }
            std::string block = "File: " + attachment.name + "\n```\n" + *text;
            while (!block.empty() && (block.back() == '\n' || block.back() == '\r')) block.pop_back();
            block += "\n```";
            if (!message.empty()) message += "\n\n";
            message += block;
        }
        if (has_parts) {
            content.insert(content.begin(), {{"type", "text"}, {"text", message}});
        } else {
            content = message;
        }

        for (auto const& name : announced) sink(tool_call("attach", "done", name));

        json messages = json::array();
        if (turns_ == 0 && !system_.empty()) messages.push_back({{"role", "system"}, {"content", system_}});
        std::size_t start = history_.size() > kMaxReplay ? history_.size() - kMaxReplay : 0;
        for (std::size_t i = start; i < history_.size(); ++i) {
            auto const& turn = history_[i];
            if (!turn.is_object()) continue;
            std::string const role = turn.value("role", "");
            std::string const text = turn.value("text", "");
            if ((role == "user" || role == "assistant") && !text.empty())
                messages.push_back({{"role", role}, {"content", text}});
        }
        messages.push_back({{"role", "user"}, {"content", content}});

        json body{{"model", model_}, {"stream", true}, {"messages", std::move(messages)}};
        for (auto const& [key, value] : params_.items()) body[key] = value;

        http_request request;
        request.url = owner_.chat_url(base_url_);
        request.method = "POST";
        request.json_body = body.dump();
        request.timeout_seconds = timeout_;
        request.debug = debug_;
        for (auto const& header : owner_.headers(api_key_)) request.headers.push_back(header);

        std::string answer;
        try {
            stream_lines(request, [&](std::string_view line) {
                if (line.empty() || line.front() == ':') return;
                if (!line.starts_with("data:")) return;
                std::string payload = trim(line.substr(5));
                if (payload.empty()) return;
                if (payload == "[DONE]") return;
                json data = json::parse(payload, nullptr, false);
                if (data.is_discarded() || !data.is_object()) {
                    sink(raw(std::string(line)));
                    return;
                }
                if (data.contains("error") && !data["error"].is_null()) {
                    sink(error(owner_.error_hint(error_text(data["error"]), base_url_)));
                    return;
                }
                if (data.contains("choices") && data["choices"].is_array()) {
                    for (auto const& choice : data["choices"]) {
                        if (!choice.is_object() || !choice.contains("delta")) continue;
                        auto const& delta = choice["delta"];
                        std::string reasoning;
                        if (delta.contains("reasoning_content") && delta["reasoning_content"].is_string())
                            reasoning = delta["reasoning_content"].get<std::string>();
                        else if (delta.contains("reasoning") && delta["reasoning"].is_string())
                            reasoning = delta["reasoning"].get<std::string>();
                        if (!reasoning.empty()) sink(thinking(reasoning));
                        if (delta.contains("content")) {
                            auto const& value = delta["content"];
                            std::string text;
                            if (value.is_string()) {
                                text = value.get<std::string>();
                            } else if (value.is_array()) {
                                for (auto const& part : value) {
                                    if (part.is_object() && part.contains("text") && part["text"].is_string())
                                        text += part["text"].get<std::string>();
                                }
                            }
                            if (!text.empty()) {
                                answer += text;
                                sink(prompt::ai::text(text));
                            }
                        }
                        if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                            for (auto const& call : delta["tool_calls"]) {
                                if (!call.is_object() || !call.contains("function")) continue;
                                auto const& function = call["function"];
                                std::string name = function.value("name", "");
                                std::string args = function.value("arguments", "");
                                sink(tool_call(name, "delta", args));
                            }
                        }
                    }
                }
                if (data.contains("usage") && data["usage"].is_object() && !data["usage"].empty())
                    sink(usage(data["usage"]));
            });
        } catch (std::exception const& failure) {
            sink(prompt::ai::error(owner_.error_hint(failure.what(), base_url_)));
            return;
        }
        ++turns_;
        record("user", prompt);
        record("assistant", answer);
        sink(done());
    }

  private:
    provider& owner_;
    std::string base_url_;
    std::string api_key_;
    json params_;
};

} // namespace

std::string normalise_base(std::string_view url) { return normalise_base_impl(url); }

json parse_params(std::vector<std::string> const& pairs) {
    json params = json::object();
    for (auto const& pair : pairs) {
        auto const equals = pair.find('=');
        if (equals == std::string::npos || equals == 0)
            throw std::runtime_error("--param wants KEY=VALUE, got " + pair);
        std::string const name = trim(pair.substr(0, equals));
        std::string const value = trim(pair.substr(equals + 1));
        json decoded = json::parse(value, nullptr, false);
        params[name] = decoded.is_discarded() ? json(value) : decoded;
    }
    return params;
}

std::unique_ptr<conversation> openai_compat::open(session::match const& match, std::string const& model,
                                                  open_options const& options) {
    std::string const base = base_url(options.base_url);
    std::string resolved;
    try {
        resolved = resolve_model(model, base);
    } catch (std::exception const& error) {
        throw std::runtime_error(error_hint(error.what(), base));
    }
    return std::make_unique<openai_conversation>(*this, match, resolved, options);
}

std::string openai_compat::base_url(std::string const& requested) {
    std::string explicit_url = trim(requested);
    if (explicit_url.empty() && !info_.base_url_env.empty()) explicit_url = trim(env_value(info_.base_url_env));
    std::string const url = explicit_url.empty() ? info_.default_base_url : explicit_url;
    if (url.empty())
        throw std::runtime_error("no API endpoint configured; pass --base-url or set " + info_.base_url_env);
    return normalise_base(url);
}

std::string openai_compat::api_key(std::string const& requested) {
    return keys::resolve(info_.name, std::array<std::string, 1>{info_.key_env}, requested).key;
}

std::vector<std::pair<std::string, std::string>> openai_compat::headers(std::string const& key,
                                                                        std::string const& /*base*/) {
    std::vector<std::pair<std::string, std::string>> out{
        {"Content-Type", "application/json"},
        {"Accept", "application/json, text/event-stream"},
    };
    std::string const token = key.empty() ? api_key({}) : key;
    if (!token.empty()) out.emplace_back("Authorization", "Bearer " + token);
    return out;
}

std::string openai_compat::chat_url(std::string const& base) { return base + "/chat/completions"; }

std::vector<model_record> openai_compat::chat_models(std::string const& base, long timeout) {
    return chat_models_with_key(base, timeout, {});
}

std::vector<model_record> openai_compat::chat_models_with_key(std::string const& base, long timeout,
                                                              std::string const& key) {
    std::vector<model_record> out;
    std::string const resolved = base.empty() ? base_url({}) : base;
    http_request request;
    request.url = resolved + "/models";
    request.timeout_seconds = timeout;
    for (auto const& header : headers(key, resolved)) request.headers.push_back(header);
    json payload;
    payload = request_json(request);
    if (!payload.is_object() || !payload.contains("data") || !payload["data"].is_array()) return out;
    for (auto const& item : payload["data"]) {
        if (!item.is_object() || !item.contains("id") || !item["id"].is_string()) continue;
        model_record record;
        record.id = item["id"].get<std::string>();
        record.name = item.value("name", "");
        record.type = item.value("type", "");
        record.state = item.value("state", "");
        if (item.contains("context_length") && item["context_length"].is_number())
            record.context = item["context_length"].get<long long>();
        else if (item.contains("max_context_length") && item["max_context_length"].is_number())
            record.context = item["max_context_length"].get<long long>();
        if (item.contains("pricing") && item["pricing"].is_object()) {
            record.prompt_price = item["pricing"].value("prompt", "");
            record.completion_price = item["pricing"].value("completion", "");
        }
        out.push_back(std::move(record));
    }
    std::erase_if(out, [](model_record const& record) {
        return lower(record.type).contains("embed") || lower(record.id).contains("embed");
    });
    return out;
}

std::vector<std::string> openai_compat::available_models() {
    try {
        auto models = chat_models(base_url({}), 5);
        std::vector<std::string> out;
        for (auto const& model : models) {
            if (lower(model.type).contains("embed")) continue;
            if (lower(model.id).contains("embed")) continue;
            out.push_back(model.id);
        }
        if (out.empty()) out.emplace_back("(none)");
        return out;
    } catch (std::exception const& error) {
        return {"(unavailable: " + std::string(error.what()) + ")"};
    }
}

std::string openai_compat::resolve_model(std::string const& requested, std::string const& /*base*/) {
    std::string wanted = trim(requested);
    if (wanted.empty() && !info_.model_env.empty()) wanted = trim(env_value(info_.model_env));
    if (wanted.empty()) wanted = trim(info_.default_model);
    if (!wanted.empty()) return wanted;
    throw std::runtime_error("choose a model with -m/--model or " + info_.model_env + "=… (`ai." + info_.name +
                             " --list` shows what the endpoint offers)");
}

json openai_compat::session_descriptor(open_options const& options) {
    json report{
        {"provider", info_.name}, {"base_url", ""}, {"model", ""}, {"reachable", false}, {"models", json::array()},
    };
    try {
        std::string const base = base_url(options.base_url);
        report["base_url"] = base;
        auto models = chat_models_with_key(base, 2, options.api_key);
        report["reachable"] = true;
        for (auto const& model : models) report["models"].push_back(model.id);
        report["model"] = resolve_model(options.model, base);
    } catch (std::exception const& error) {
        report["error"] = error.what();
    }
    return report;
}

std::string openai_compat::error_hint(std::string const& message, std::string const& base) {
    std::string text = message;
    std::string const lowered = lower(text);
    bool const model_problem = lowered.contains("not found") || lowered.contains("not loaded") ||
                               lowered.contains("does not exist") || lowered.contains("invalid model") ||
                               lowered.contains("no such model") || lowered.contains("model_not_found") ||
                               lowered.contains("unknown model");
    if (model_problem && lowered.contains("model")) {
        auto models = chat_models(base, 2);
        std::string names;
        for (auto const& model : models) {
            if (!names.empty()) names += ", ";
            names += model.id;
        }
        if (!names.empty()) text += "\nAvailable models: " + names;
    }
    return text;
}

} // namespace prompt::ai
