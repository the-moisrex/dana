#include "provider.hpp"

#include "../keys.hpp"

#include <array>
#include <cstdlib>
#include <string>

namespace dana::ai {

    namespace {

        std::string env_value(std::string_view name) {
            std::string key(name);
            char const* raw = std::getenv(key.c_str());
            return raw == nullptr ? std::string{} : std::string(raw);
        }

    } // namespace

    std::string provider::base_url(std::string_view requested) {
        return std::string(requested);
    }

    std::string provider::api_key(std::string_view requested) {
        std::array<std::string, 1> const stuff{info().key_env};
        return keys::resolve(info().name, stuff, requested).key;
    }

    std::vector<std::pair<std::string, std::string>> provider::headers(
      std::string_view key,
      std::string_view /*unused*/) {
        std::vector<std::pair<std::string, std::string>> out;
        std::string const                                token = key.empty() ? api_key({}) : std::string(key);
        if (!token.empty()) {
            out.emplace_back("Authorization", "Bearer " + token);
        }
        return out;
    }

    std::string provider::chat_url(std::string_view base) {
        return std::format("{}/chat/completions", base);
    }

    std::vector<model_record> provider::chat_models(std::string_view /*unused*/, long /*unused*/) {
        return {};
    }

    std::vector<model_record>
    provider::chat_models_with_key(std::string_view base, long timeout, std::string_view /*unused*/) {
        return chat_models(base, timeout);
    }

    std::vector<std::string> provider::available_models() {
        return info().models;
    }

    std::string provider::resolve_model(std::string_view requested, std::string_view /*unused*/) {
        if (!requested.empty()) {
            return std::string(requested);
        }
        if (!info().model_env.empty()) {
            std::string const value = env_value(info().model_env);
            if (!value.empty()) {
                return value;
            }
        }
        return info().default_model;
    }

    nlohmann::json provider::session_descriptor(open_options const& /*unused*/) {
        nlohmann::json report    = nlohmann::json::object();
        report["provider"]       = info().name;
        report["needs_session"]  = info().needs_session;
        report["session_target"] = info().session_target;
        return report;
    }

    std::string provider::error_hint(std::string_view message, std::string_view /*unused*/) {
        return std::string(message);
    }

} // namespace dana::ai
