#pragma once

// Saved conversations for -c / -i / --reply.  Every provider keeps the id of
// its most recent conversation (plus an opaque resume object) and a trimmed
// transcript under $XDG_STATE_HOME/ai/<provider>.json.  The same record is
// mirrored per terminal under .../terminals/<scope>/<provider>.json, and a
// plain-text `last` file names the provider that talked most recently there.

#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace prompt::ai::state {

//: How many transcript entries to keep alongside the id.
inline constexpr std::size_t kMaxHistory = 20;

//: A saved conversation older than this is ignored by --continue.
inline constexpr double kMaxAge = 7 * 24 * 3600;

struct conversation_record {
    std::string conversation_id;
    std::string model;
    double updated = 0;
    nlohmann::json history = nlohmann::json::array();
    nlohmann::json resume = nlohmann::json::object();
};

std::filesystem::path directory();
std::filesystem::path path(std::string_view provider);
std::filesystem::path terminals_dir();
std::filesystem::path terminal_path(std::string_view provider, std::string_view scope = {});
std::filesystem::path last_path(std::string_view scope = {});

// The current terminal's state scope (see prompt::terminal::scope).
std::string terminal_scope();

std::optional<conversation_record> load(std::string_view provider, double max_age = kMaxAge);
std::optional<conversation_record> load_terminal(std::string_view provider, std::string_view scope = {},
                                                 double max_age = kMaxAge);

// Persist a conversation (best effort) and return the stored record.
conversation_record save(std::string_view provider, conversation_record const& record, std::string_view scope = {});

// Forget a provider's conversation (global and this terminal).
void clear(std::string_view provider, std::string_view scope = {});

// The state file for the last debate between `names` (order-insensitive).
std::filesystem::path debate_path(std::vector<std::string> names);
std::optional<std::map<std::string, conversation_record>> load_debate(std::vector<std::string> const& names,
                                                                      double max_age = kMaxAge);
void save_debate(std::vector<std::string> const& names, std::map<std::string, conversation_record> const& participants,
                 std::string_view scope = {});
void clear_debate(std::vector<std::string> const& names, std::string_view scope = {});

} // namespace prompt::ai::state
