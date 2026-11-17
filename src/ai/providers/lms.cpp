#include "../../core/fs.hpp"
#include "../../core/process.hpp"
#include "../diagnostics.hpp"
#include "../http.hpp"
#include "../models.hpp"
#include "../state.hpp"
#include "openai_compat.hpp"
#include "registry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <future>
#include <nlohmann/json.hpp>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace dana::ai {

    namespace {

        using nlohmann::json;

        constexpr std::array<int, 5>              kProbePorts{1234, 1337, 8080, 8000, 11'434};
        constexpr std::array<std::string_view, 2> kProbeHosts{"127.0.0.1", "localhost"};
        constexpr long                            kProbeTimeout = 1; // seconds; Python uses 0.4s
        constexpr std::string_view                kCacheName    = "lms-endpoint";

        std::string env_value(std::string_view name) {
            std::string key(name);
            char const* raw = std::getenv(key.c_str());
            return raw == nullptr ? std::string{} : std::string(raw);
        }

        std::string trim(std::string_view value) {
            std::size_t first = value.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos) {
                return {};
            }
            std::size_t last = value.find_last_not_of(" \t\r\n");
            return std::string(value.substr(first, last - first + 1));
        }

        std::vector<std::string> probe_urls() {
            std::vector<std::string> urls;
            for (int port : kProbePorts) {
                for (auto host : kProbeHosts) {
                    urls.push_back("http://" + std::string(host) + ":" + std::to_string(port));
                }
            }
            return urls;
        }

        std::string cached_endpoint() {
            std::ifstream stream(state::path(kCacheName));
            if (!stream) {
                return {};
            }
            json data = json::parse(stream, nullptr, false);
            if (data.is_discarded() || !data.is_object()) {
                return {};
            }
            return data.value("base_url", "");
        }

        void remember_endpoint(std::string const& base) {
            std::error_code ec;
            std::filesystem::create_directories(state::path(kCacheName).parent_path(), ec);
            std::filesystem::path const path      = state::path(kCacheName);
            std::filesystem::path const temporary = path.string() + ".tmp";
            {
                std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
                if (!stream) {
                    warn("could not cache the LM Studio endpoint", "lms");
                    return;
                }
                stream << json{{"base_url", base},
                       {"updated",
                        std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(),},}
                      .dump();
            }
            std::filesystem::rename(temporary, path, ec);
            if (ec) {
                warn("could not cache the LM Studio endpoint: " + ec.message(), "lms");
            }
        }

        class lms_provider : public openai_compat {
          public:
            lms_provider()
              : openai_compat(provider_info{
                  .name                = "lms",
                  .label               = "LM Studio",
                  .origin              = "http://localhost:1234",
                  .session_target      = "",
                  .default_model       = std::string(models::default_model("lms")),
                  .models              = models::list("lms"),
                  .inline_limit        = 16UL * 1024UL * 1024UL,
                  .needs_session       = false,
                  .api                 = true,
                  .local               = true,
                  .recordable          = false,
                  .prompt_file_upload  = false,
                  .auto_attach_by_size = false,
                  .discovers_model     = true,
                  .base_url            = "",
                  .base_url_env        = "LMS_BASE_URL",
                  .key_env             = "LMS_API_KEY",
                  .model_env           = "LMS_MODEL",
                  .default_base_url    = "",
                }) {}

            std::string base_url(std::string const& requested) override {
                std::string explicit_url = trim(requested);
                if (explicit_url.empty()) {
                    explicit_url = trim(env_value(m_info.base_url_env));
                }
                if (!explicit_url.empty()) {
                    return normalise_base(explicit_url);
                }

                std::string const cached = cached_endpoint();
                if (!cached.empty() && alive(cached)) {
                    return normalise_base(cached);
                }

                std::string found = probe();
                if (found.empty()) {
                    std::string const from_cli = cli_endpoint();
                    if (!from_cli.empty() && alive(from_cli)) {
                        found = from_cli;
                    }
                }
                if (found.empty()) {
                    throw std::runtime_error(unreachable());
                }
                std::string const endpoint = normalise_base(found);
                remember_endpoint(endpoint);
                return endpoint;
            }

            std::vector<model_record> chat_models(std::string const& base, long timeout) override {
                return chat_models_with_key(base, timeout, {});
            }

            std::vector<model_record>
            chat_models_with_key(std::string const& base, long timeout, std::string const& key) override {
                // An empty base means "discover the endpoint", exactly like the Python
                // provider's `base or self.base_url()`.
                std::string const resolved = base.empty() ? base_url({}) : base;
                std::string const root = resolved.ends_with("/v1") ? resolved.substr(0, resolved.size() - 3) : resolved;
                http_request      request;
                request.url             = root + "/api/v0/models";
                request.timeout_seconds = timeout;
                for (auto const& header : headers(key, resolved)) {
                    request.headers.push_back(header);
                }
                json payload;
                try {
                    payload = request_json(request);
                } catch (std::exception const& error) {
                    note("the v0 model list failed (" + std::string(error.what()) + "); using the OpenAI list", true);
                    return openai_compat::chat_models_with_key(resolved, timeout, key);
                }
                std::vector<model_record> out;
                if (payload.is_object() && payload.contains("data") && payload["data"].is_array()) {
                    for (auto const& item : payload["data"]) {
                        if (!item.is_object() || !item.contains("id") || !item["id"].is_string()) {
                            continue;
                        }
                        model_record record;
                        record.id    = item["id"].get<std::string>();
                        record.type  = item.value("type", "");
                        record.state = item.value("state", "");
                        if (item.contains("max_context_length") && item["max_context_length"].is_number()) {
                            record.context = item["max_context_length"].get<std::int64_t>();
                        }
                        out.push_back(std::move(record));
                    }
                }
                if (out.empty()) {
                    return openai_compat::chat_models_with_key(base, timeout, key);
                }
                return out;
            }

            std::string resolve_model(std::string const& requested, std::string const& base) override {
                std::string wanted = trim(requested);
                if (wanted.empty()) {
                    wanted = trim(env_value(m_info.model_env));
                }
                if (!wanted.empty()) {
                    return wanted;
                }
                auto                      models = chat_models(base, 5);
                std::vector<model_record> chat;
                for (auto const& model : models) {
                    std::string const type      = model.type;
                    std::string const id        = model.id;
                    auto              has_embed = [](std::string const& value) {
                        std::string lowered = value;
                        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
                            return static_cast<char>(std::tolower(c));
                        });
                        return lowered.contains("embed");
                    };
                    if (has_embed(type) || has_embed(id)) {
                        continue;
                    }
                    chat.push_back(model);
                }
                for (auto const& model : chat) {
                    if (model.state == "loaded") {
                        return model.id;
                    }
                }
                if (!chat.empty()) {
                    return chat.front().id;
                }
                throw std::runtime_error(
                  "no chat model is available on the LM Studio server; load one with `lms load "
                  "<model>` (or in the app) or pass -m/--model");
            }

            std::vector<std::string> available_models() override {
                try {
                    auto                     models = chat_models(base_url({}), 5);
                    std::vector<std::string> out;
                    for (auto const& model : models) {
                        std::string lowered = model.type;
                        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
                            return static_cast<char>(std::tolower(c));
                        });
                        if (lowered.contains("embed")) {
                            continue;
                        }
                        out.push_back(model.state == "loaded" ? model.id + " (loaded)" : model.id);
                    }
                    if (out.empty()) {
                        out.emplace_back("(no chat models loaded)");
                    }
                    return out;
                } catch (std::exception const& error) {
                    return {"(unavailable: " + std::string(error.what()) + ")"};
                }
            }

            std::string error_hint(std::string const& message, std::string const& base) override {
                std::string text    = openai_compat::error_hint(message, base);
                std::string lowered = text;
                std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                if (lowered.contains("connect") || lowered.contains("refused") || lowered.contains("timed out") ||
                    lowered.contains("timeout") || lowered.contains("unreachable"))
                {
                    text += "\nCheck that LM Studio's server is running (`lms server start`) and that " + base +
                            " is the right endpoint; --base-url and " + m_info.base_url_env + " set it.";
                }
                return text;
            }

          private:
            bool alive(std::string const& base) {
                try {
                    http_request request;
                    request.url             = normalise_base(base) + "/models";
                    request.timeout_seconds = kProbeTimeout;
                    for (auto const& header : headers({})) {
                        request.headers.push_back(header);
                    }
                    (void) request_json(request);
                    return true;
                } catch (std::exception const&) {
                    return false;
                }
            }

            std::string probe() {
                auto                           urls = probe_urls();
                std::vector<std::future<bool>> futures;
                futures.reserve(urls.size());
                for (auto const& url : urls) {
                    futures.push_back(std::async(std::launch::async, [this, url] {
                        return alive(url);
                    }));
                }
                for (std::size_t i = 0; i < urls.size(); ++i) {
                    try {
                        if (futures[i].get()) {
                            return urls[i];
                        }
                    } catch (std::exception const& error) {
                        note("probe " + urls[i] + ": " + error.what(), true);
                    }
                }
                return {};
            }

            static std::string cli_endpoint() {
                auto const executable = dana::fs::bin_tool(
                  "lms",
                  dana::fs::self_path().value_or(std::filesystem::path{}),
                  dana::fs::find_git_root());
                if (executable.empty()) {
                    return {};
                }
                auto const result = process::run_command(
                  std::vector<std::string>{executable.string(), "status"},
                  {},
                  {},
                  std::chrono::milliseconds(10'000));
                std::string             text = result.stdout_data + result.stderr_data;
                static std::regex const ansi(R"(\x1b\[[0-9;]*m)");
                text = std::regex_replace(text, ansi, "");
                static std::regex const port(R"(port\s*:?\s*(\d+))", std::regex::icase);
                std::smatch             match;
                if (!std::regex_search(text, match, port)) {
                    return {};
                }
                return "http://127.0.0.1:" + match[1].str() + "/v1";
            }

            static std::string unreachable() {
                std::string text = "no LM Studio server found: probed ";
                auto        urls = probe_urls();
                for (std::size_t i = 0; i < urls.size(); ++i) {
                    if (i != 0) {
                        text += ", ";
                    }
                    text += urls[i];
                }
                text +=
                  " and `lms status`.\nStart it with `lms server start` (or the app's Developer → Local Server), or "
                  "pass --base-url / set LMS_BASE_URL.";
                return text;
            }
        };

    } // namespace

    // Registration runs at static-init time; a failed allocation aborts the
    // provider catalog rather than silently dropping it.
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization)
    bool const lms_registered = register_provider(std::make_unique<lms_provider>());

} // namespace dana::ai
