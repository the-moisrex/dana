#include "../models.hpp"
#include "openai_compat.hpp"
#include "registry.hpp"

#include <cstdlib>
#include <string>

namespace dana::ai {

    namespace {

        struct openrouter_provider : openai_compat {
            openrouter_provider()
              : openai_compat(provider_info{
                  .name                = "openrouter",
                  .label               = "OpenRouter",
                  .origin              = "https://openrouter.ai",
                  .session_target      = "",
                  .default_model       = std::string(models::default_model("openrouter")),
                  .models              = models::list("openrouter"),
                  .inline_limit        = 16UL * 1024UL * 1024UL,
                  .needs_session       = false,
                  .api                 = true,
                  .local               = false,
                  .recordable          = false,
                  .prompt_file_upload  = false,
                  .auto_attach_by_size = false,
                  .discovers_model     = false,
                  .base_url            = "",
                  .base_url_env        = "OPENROUTER_BASE_URL",
                  .key_env             = "OPENROUTER_API_KEY",
                  .model_env           = "OPENROUTER_MODEL",
                  .default_base_url    = "https://openrouter.ai/api/v1",
                }) {}

            std::string resolve_model(std::string_view requested, std::string_view /*base*/) override {
                std::string_view wanted = requested;
                char const*      model  = std::getenv("OPENROUTER_MODEL");
                if (wanted.empty() && model != nullptr) {
                    wanted = model;
                }
                if (wanted.empty()) {
                    char const* fallback = std::getenv("OPENAI_MODEL");
                    if (fallback != nullptr) {
                        wanted = fallback;
                    }
                }
                if (wanted.empty()) {
                    wanted = m_info.default_model;
                }
                if (!wanted.empty()) {
                    return std::string(wanted);
                }
                throw std::runtime_error(
                  "choose a model with -m/--model, OPENROUTER_MODEL=… or OPENAI_MODEL=… "
                  "(`ai.openrouter --list` shows what OpenRouter offers)");
            }
        };

    } // namespace

    // Registration runs at static-init time; a failed allocation aborts the
    // provider catalog rather than silently dropping it.
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization)
    bool const openrouter_registered = register_provider(std::make_unique<openrouter_provider>());

} // namespace dana::ai
