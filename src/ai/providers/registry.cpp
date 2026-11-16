#include "registry.hpp"

#include <algorithm>
#include <map>
#include <string>

namespace prompt::ai {

    namespace {

        std::map<std::string, std::unique_ptr<provider>>& store() {
            static std::map<std::string, std::unique_ptr<provider>> providers;
            return providers;
        }

    } // namespace

    bool register_provider(std::unique_ptr<provider> value) {
        if (!value) {
            return false;
        }
        std::string name = value->info().name;
        store()[name]    = std::move(value);
        return true;
    }

    provider* find_provider(std::string_view name) {
        auto const it = store().find(std::string(name));
        return it == store().end() ? nullptr : it->second.get();
    }

    std::vector<provider*> all_providers() {
        std::vector<provider*> out;
        out.reserve(store().size());
        for (auto const& [name, value] : store()) {
            out.push_back(value.get());
        }
        return out;
    }

    std::optional<std::string> canonical_provider(std::string_view name) {
        if (name.starts_with("ai.")) {
            name.remove_prefix(3);
        }
        if (name == "deepseek") {
            return std::string{"dseek"};
        }
        if (name == "lmstudio") {
            return std::string{"lms"};
        }
        if (name == "oai") {
            return std::string{"openai"};
        }
        if (name == "pplx") {
            return std::string{"perplexity"};
        }
        if (find_provider(name) != nullptr) {
            return std::string(name);
        }
        return std::nullopt;
    }

} // namespace prompt::ai
