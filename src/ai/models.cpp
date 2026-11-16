#include "models.hpp"

#include <array>

namespace prompt::ai::models {

namespace {

// ChatGPT
constexpr std::array<std::string_view, 7> kChatgptModels{
    "auto", "gpt-5", "gpt-5-mini", "gpt-4o", "gpt-4o-mini", "o3", "o4-mini",
};

// Claude
constexpr std::array<std::string_view, 4> kClaudeModels{"auto", "sonnet", "opus", "haiku"};
constexpr std::array<alias, 8> kClaudeAliases{
    {
        {.from = "auto", .to = ""},
        {.from = "default", .to = ""},
        {.from = "sonnet", .to = "claude-sonnet-4-5-20250929"},
        {.from = "sonnet-4-5", .to = "claude-sonnet-4-5-20250929"},
        {.from = "opus", .to = "claude-opus-4-1-20250805"},
        {.from = "opus-4-1", .to = "claude-opus-4-1-20250805"},
        {.from = "haiku", .to = "claude-haiku-4-5-20251001"},
        {.from = "haiku-4-5", .to = "claude-haiku-4-5-20251001"},
    },
};

// DeepSeek
constexpr std::array<std::string_view, 2> kDeepseekModels{"deepseek_reasoner", "deepseek_chat"};

// GapGPT. The friendly labels are accepted by `--model`; every `A-…` id is
// accepted verbatim too. Some aliases still point at ids the site keeps
// accepting even though they are not in the current picker list.
constexpr std::array<std::string_view, 16> kGapModels{
    "A-GAP",          "A-GPT-6-ASTRA", "A-GPT-5.5",      "A-GPT4O",   "A-GPT4O-MINI", "A-GPT-4.1",
    "A-O1",           "A-O4-MINI",     "A-GEMINI-3-PRO", "A-CLAUDE",  "A-SONNET-4.6", "A-OPUS-4.5",
    "A-DEEPSEEK-PRO", "A-DEEPSEEK-R1", "A-GROK-4",       "A-KIMI-K3",
};
constexpr std::array<alias, 22> kGapAliases{
    {
        {.from = "gap", .to = "A-GAP"},
        {.from = "gpt-6-astra", .to = "A-GPT-6-ASTRA"},
        {.from = "gpt6", .to = "A-GPT-6-ASTRA"},
        {.from = "gpt-4o", .to = "A-GPT4O"},
        {.from = "gpt-4o-mini", .to = "A-GPT4O-MINI"},
        {.from = "gpt-4.1", .to = "A-GPT-4.1"},
        {.from = "gpt-5", .to = "A-GPT-5"},
        {.from = "o1", .to = "A-O1"},
        {.from = "o4-mini", .to = "A-O4-MINI"},
        {.from = "gemini", .to = "A-GEMINI"},
        {.from = "gemini-pro", .to = "A-GEMINI-3-PRO"},
        {.from = "claude", .to = "A-CLAUDE"},
        {.from = "sonnet", .to = "A-SONNET"},
        {.from = "opus", .to = "A-OPUS"},
        {.from = "deepseek", .to = "A-DEEPSEEK"},
        {.from = "deepseek-r1", .to = "A-DEEPSEEK-R1"},
        {.from = "deepseek-v3", .to = "A-DEEPSEEK-V3"},
        {.from = "grok", .to = "A-GROK"},
        {.from = "grok-3", .to = "A-GROK-3"},
        {.from = "grok-4", .to = "A-GROK-4"},
        {.from = "kimi", .to = "A-KIMI"},
        {.from = "kimi3", .to = "A-KIMI-K3"},
    },
};

// Gemini
constexpr std::array<std::string_view, 4> kGeminiModels{"auto", "gemini-pro", "gemini-flash", "gemini-flash-lite"};
constexpr std::array<alias, 7> kGeminiAliases{
    {
        {.from = "auto", .to = "auto"},
        {.from = "default", .to = "auto"},
        {.from = "gemini", .to = "gemini-pro"},
        {.from = "pro", .to = "gemini-pro"},
        {.from = "flash", .to = "gemini-flash"},
        {.from = "lite", .to = "gemini-flash-lite"},
        {.from = "flash-lite", .to = "gemini-flash-lite"},
    },
};

// Grok
constexpr std::array<std::string_view, 6> kGrokModels{
    "fast", "expert", "grok-4", "grok-3", "grok-3-mini", "grok-3-thinking",
};

// Kimi
constexpr std::array<std::string_view, 1> kKimiModels{"kimi"};

// Perplexity
constexpr std::array<std::string_view, 18> kPerplexityModels{
    "auto",   "sonar",           "sonar-pro",     "gpt-5",         "gpt-5-thinking", "gemini",        "gemini-thinking",
    "sonnet", "sonnet-thinking", "opus",          "opus-thinking", "grok",           "grok-thinking", "kimi",
    "glm",    "nemotron",        "deep-research", "council",
};
constexpr std::array<alias, 28> kPerplexityAliases{
    {
        {.from = "auto", .to = "pplx_pro"},
        {.from = "best", .to = "pplx_pro"},
        {.from = "sonar", .to = "turbo"},
        {.from = "sonar-pro", .to = "experimental"},
        {.from = "experimental", .to = "experimental"},
        {.from = "gpt-5", .to = "gpt56_terra"},
        {.from = "gpt-5-thinking", .to = "gpt56_terra_thinking"},
        {.from = "gpt-5-sol", .to = "gpt56_sol"},
        {.from = "gpt-5-sol-thinking", .to = "gpt56_sol_thinking"},
        {.from = "gemini", .to = "gemini37flash"},
        {.from = "gemini-thinking", .to = "gemini37flashthinking"},
        {.from = "sonnet", .to = "claude50sonnet"},
        {.from = "sonnet-thinking", .to = "claude50sonnetthinking"},
        {.from = "opus", .to = "claude50opus"},
        {.from = "opus-thinking", .to = "claude50opusthinking"},
        {.from = "grok", .to = "grok46low"},
        {.from = "grok-thinking", .to = "grok46medium"},
        {.from = "kimi", .to = "kimik3thinking"},
        {.from = "glm", .to = "glm_5_2"},
        {.from = "nemotron", .to = "nv_nemotron_3_ultra"},
        {.from = "deep-research", .to = "pplx_alpha"},
        {.from = "research", .to = "pplx_alpha"},
        {.from = "council", .to = "pplx_agentic_research"},
        {.from = "agentic-research", .to = "pplx_agentic_research"},
        {.from = "turbo", .to = "turbo"},
        {.from = "pplx_pro", .to = "pplx_pro"},
        {.from = "pplx_alpha", .to = "pplx_alpha"},
        {.from = "pplx_agentic_research", .to = "pplx_agentic_research"},
    },
};

// The API providers discover their models from the endpoint (`--list`, the
// server's `/models`), so their static lists are empty.
constexpr std::array<entry, 11> kEntries{
    {
        {.provider = "chatgpt", .default_model = "auto", .models = kChatgptModels, .aliases = {}},
        {.provider = "claude", .default_model = "auto", .models = kClaudeModels, .aliases = kClaudeAliases},
        {.provider = "dseek", .default_model = "deepseek_reasoner", .models = kDeepseekModels, .aliases = {}},
        {.provider = "gap", .default_model = "A-GAP", .models = kGapModels, .aliases = kGapAliases},
        {.provider = "gemini", .default_model = "auto", .models = kGeminiModels, .aliases = kGeminiAliases},
        {.provider = "grok", .default_model = "fast", .models = kGrokModels, .aliases = {}},
        {.provider = "kimi", .default_model = "kimi", .models = kKimiModels, .aliases = {}},
        {.provider = "perplexity", .default_model = "auto", .models = kPerplexityModels, .aliases = kPerplexityAliases},
        {.provider = "lms", .default_model = "", .models = {}, .aliases = {}},
        {.provider = "openai", .default_model = "", .models = {}, .aliases = {}},
        {.provider = "openrouter", .default_model = "openrouter/auto-code", .models = {}, .aliases = {}},
    },
};

std::string_view canonical(std::string_view name) {
    if (name.starts_with("ai.")) name.remove_prefix(3);
    if (name == "deepseek") return "dseek";
    if (name == "lmstudio") return "lms";
    if (name == "oai") return "openai";
    if (name == "pplx") return "perplexity";
    return name;
}

} // namespace

std::span<entry const> all() { return kEntries; }

entry const* find(std::string_view provider) {
    std::string_view const name = canonical(provider);
    for (auto const& item : kEntries) {
        if (item.provider == name) return &item;
    }
    return nullptr;
}

std::vector<std::string> list(std::string_view provider) {
    entry const* found = find(provider);
    if (found == nullptr) return {};
    return {found->models.begin(), found->models.end()};
}

std::string_view default_model(std::string_view provider) {
    entry const* found = find(provider);
    return found == nullptr ? std::string_view{} : found->default_model;
}

std::optional<std::string_view> resolve_alias(std::string_view provider, std::string_view name) {
    entry const* found = find(provider);
    if (found == nullptr) return std::nullopt;
    for (auto const& item : found->aliases) {
        if (item.from == name) return item.to;
    }
    return std::nullopt;
}

} // namespace prompt::ai::models
