#pragma once

// Normalised conversation history shared by the providers and the native
// history bridge the Python ai.save uses.  A provider's chat list yields
// ChatSummary objects and fetch_chat returns a Chat with its Message list.

#include <cstdint>
#include <string>
#include <vector>

namespace prompt::ai::history {

inline constexpr std::string_view kUser = "user";
inline constexpr std::string_view kAssistant = "assistant";
inline constexpr std::string_view kSystem = "system";
inline constexpr std::string_view kTool = "tool";

inline constexpr std::string_view kText = "text";
inline constexpr std::string_view kThinking = "thinking";
inline constexpr std::string_view kToolCall = "tool_call";
inline constexpr std::string_view kAttachment = "attachment";

struct message {
    std::string role;
    std::string kind = std::string(kText);
    std::string text;
    std::string name;
    std::string status;
    std::string args;
    std::string created;
};

struct chat {
    std::string id;
    std::string title;
    std::string url;
    std::string created;
    std::string updated;
    std::vector<message> messages;
    std::string provider;
};

struct chat_summary {
    std::string id;
    std::string title;
    std::string created;
    std::string updated;
    std::string url;
    bool pinned = false;
};

// Convert a Unix timestamp (seconds or milliseconds) to ISO 8601 UTC.
std::string iso_from_epoch(double value);

// Parse an ISO 8601 timestamp to seconds (0 when it cannot be parsed).
double epoch_of(std::string_view value);

} // namespace prompt::ai::history
