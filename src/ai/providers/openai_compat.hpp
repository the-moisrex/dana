#pragma once

#include "provider.hpp"
#include <string>
#include <string_view>
#include <vector>

namespace prompt::ai {

// Normalise an API root: a scheme, no trailing slash, an explicit /v1.
std::string normalise_base(std::string_view url);

// --param KEY=VALUE pairs as a JSON request body (values JSON-decoded).
nlohmann::json parse_params(std::vector<std::string> const& pairs);

// The shared OpenAI-compatible engine (lms/openai/openrouter).
class openai_compat : public provider {
  public:
    [[nodiscard]] provider_info const& info() const override { return info_; }

    std::unique_ptr<conversation> open(session::match const& match, std::string const& model,
                                       open_options const& options) override;

    std::string base_url(std::string const& requested) override;
    std::string api_key(std::string const& requested) override;
    std::vector<std::pair<std::string, std::string>> headers(std::string const& key,
                                                             std::string const& base = {}) override;
    std::string chat_url(std::string const& base) override;
    std::vector<model_record> chat_models(std::string const& base, long timeout) override;
    std::vector<model_record> chat_models_with_key(std::string const& base, long timeout,
                                                   std::string const& key) override;
    std::vector<std::string> available_models() override;
    std::string resolve_model(std::string const& requested, std::string const& base) override;
    nlohmann::json session_descriptor(open_options const& options) override;
    std::string error_hint(std::string const& message, std::string const& base) override;

  protected:
    explicit openai_compat(provider_info info) : info_(std::move(info)) {}

    provider_info info_;
};

} // namespace prompt::ai
