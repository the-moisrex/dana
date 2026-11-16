#include "../keys.hpp"
#include "../models.hpp"
#include "openai_compat.hpp"
#include "registry.hpp"

#include <string>

namespace prompt::ai {

    namespace {

        class openai_provider : public openai_compat {
          public:
            openai_provider()
              : openai_compat(provider_info{
                  .name                = "openai",
                  .label               = "OpenAI",
                  .origin              = "https://api.openai.com",
                  .session_target      = "",
                  .default_model       = std::string(models::default_model("openai")),
                  .models              = models::list("openai"),
                  .inline_limit        = 16UL * 1024UL * 1024UL,
                  .needs_session       = false,
                  .api                 = true,
                  .local               = false,
                  .recordable          = false,
                  .prompt_file_upload  = false,
                  .auto_attach_by_size = false,
                  .discovers_model     = false,
                  .base_url            = "",
                  .base_url_env        = "OPENAI_BASE_URL",
                  .key_env             = "OPENAI_API_KEY",
                  .model_env           = "OPENAI_MODEL",
                  .default_base_url    = "https://api.openai.com/v1",
                }) {}

            std::string resolve_model(std::string const& requested, std::string const& /*base*/) override {
                std::string wanted = requested;
                if (wanted.empty()) {
                    char const* raw = std::getenv("OPENAI_MODEL");
                    if (raw != nullptr) {
                        wanted = raw;
                    }
                }
                if (!wanted.empty()) {
                    return wanted;
                }
                throw std::runtime_error(
                  "choose a model with -m/--model or OPENAI_MODEL=… (`ai.openai --list` shows what "
                  "the endpoint offers)");
            }

            std::vector<std::pair<std::string, std::string>> headers(std::string const& key,
                                                                     std::string const& base = {}) override {
                auto              out  = openai_compat::headers(key, base);
                std::string const host = base.empty() ? base_url({}) : base;
                if (api_key(key).empty() && host.contains("api.openai.com")) {
                    throw std::runtime_error("no API key: set OPENAI_API_KEY or pass --api-key");
                }
                return out;
            }
        };

    } // namespace

    // Registration runs at static-init time; a failed allocation aborts the
    // provider catalog rather than silently dropping it.
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization)
    bool const openai_registered = register_provider(std::make_unique<openai_provider>());

} // namespace prompt::ai
