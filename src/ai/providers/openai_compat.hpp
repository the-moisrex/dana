#pragma once

#include "provider.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace dana::ai {

    // Normalise an API root: a scheme, no trailing slash, an explicit /v1.
    std::string normalise_base(std::string_view url);

    // --param KEY=VALUE pairs as a JSON request body (values JSON-decoded).
    nlohmann::json parse_params(text::string_views pairs);

    // The shared OpenAI-compatible engine (lms/openai/openrouter).
    struct [[nodiscard]] openai_compat : provider {
        [[nodiscard]] provider_info const& info() const override {
            return m_info;
        }

        std::unique_ptr<conversation>
        open(session::match const& match, std::string_view model, open_options const& options) override;

        std::string                                      base_url(std::string_view requested) override;
        std::string                                      api_key(std::string_view requested) override;
        std::vector<std::pair<std::string, std::string>> headers(std::string_view key,
                                                                 std::string_view base = {}) override;
        std::string                                      chat_url(std::string_view base) override;
        std::vector<model_record>                        chat_models(std::string_view base, long timeout) override;
        std::vector<model_record>
        chat_models_with_key(std::string_view base, long timeout, std::string_view key) override;
        std::vector<std::string> available_models() override;
        std::string              resolve_model(std::string_view requested, std::string_view base) override;
        nlohmann::json           session_descriptor(open_options const& options) override;
        std::string              error_hint(std::string_view message, std::string_view base) override;

      protected:
        explicit openai_compat(provider_info info) : m_info(std::move(info)) {}

        provider_info m_info;
    };

} // namespace dana::ai
