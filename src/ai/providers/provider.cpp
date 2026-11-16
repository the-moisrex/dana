#include "provider.hpp"
#include "../keys.hpp"
#include <array>
#include <cstdlib>
#include <string>

namespace prompt::ai {

namespace {

std::string env_value(std::string_view name) {
    std::string key(name);
    char const* raw = std::getenv(key.c_str());
    return raw == nullptr ? std::string{} : std::string(raw);
}

} // namespace

std::string provider::base_url(std::string const& requested) { return requested; }

std::string provider::api_key(std::string const& requested) {
    return keys::resolve(info().name, std::array<std::string, 1>{info().key_env}, requested).key;
}

std::vector<std::pair<std::string, std::string>> provider::headers(std::string const& key,
                                                                   std::string const& /*unused*/) {
    std::vector<std::pair<std::string, std::string>> out;
    std::string const token = key.empty() ? api_key({}) : key;
    if (!token.empty()) out.emplace_back("Authorization", "Bearer " + token);
    return out;
}

std::string provider::chat_url(std::string const& base) { return base + "/chat/completions"; }

std::vector<model_record> provider::chat_models(std::string const& /*unused*/, long /*unused*/) { return {}; }

std::vector<model_record> provider::chat_models_with_key(std::string const& base, long timeout,
                                                         std::string const& /*unused*/) {
    return chat_models(base, timeout);
}

std::vector<std::string> provider::available_models() { return info().models; }

std::string provider::resolve_model(std::string const& requested, std::string const& /*unused*/) {
    if (!requested.empty()) return requested;
    if (!info().model_env.empty()) {
        std::string const value = env_value(info().model_env);
        if (!value.empty()) return value;
    }
    return info().default_model;
}

nlohmann::json provider::session_descriptor(open_options const& /*unused*/) {
    nlohmann::json report = nlohmann::json::object();
    report["provider"] = info().name;
    report["needs_session"] = info().needs_session;
    report["session_target"] = info().session_target;
    return report;
}

std::string provider::error_hint(std::string const& message, std::string const& /*unused*/) { return message; }

} // namespace prompt::ai
