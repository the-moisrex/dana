#include "state.hpp"

#include "../sdk/terminal.hpp"
#include "diagnostics.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

namespace dana::ai::state {

    namespace {

        namespace fs = std::filesystem;
        using nlohmann::json;

        std::string env_value(std::string_view name) {
            std::string key(name);
            char const* raw = std::getenv(key.c_str());
            return raw == nullptr ? std::string{} : std::string(raw);
        }

        double now_seconds() {
            return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        json record_to_json(conversation_record const& record) {
            json history = record.history.is_array() ? record.history : json::array();
            if (history.size() > kMaxHistory) {
                json trimmed = json::array();
                for (std::size_t i = history.size() - kMaxHistory; i < history.size(); ++i) {
                    trimmed.push_back(history[i]);
                }
                history = std::move(trimmed);
            }
            return json{
              {"conversation_id",                                     record.conversation_id},
              {          "model",                                               record.model},
              {        "updated",                                              now_seconds()},
              {        "history",                                         std::move(history)},
              {         "resume", record.resume.is_object() ? record.resume : json::object()},
            };
        }

        conversation_record record_from_json(json const& data) {
            conversation_record record;
            if (!data.is_object()) {
                return record;
            }
            record.conversation_id = data.value("conversation_id", "");
            record.model           = data.value("model", "");
            record.updated         = data.value("updated", 0.0);
            record.history         = data.contains("history") ? data["history"] : json::array();
            record.resume          = data.contains("resume") ? data["resume"] : json::object();
            return record;
        }

        bool write_json(fs::path const& file_path, json const& data) {
            std::error_code ec;
            fs::create_directories(file_path.parent_path(), ec);
            fs::path const temporary = file_path.string() + ".tmp";
            {
                std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
                if (!stream) {
                    warn("could not save " + file_path.filename().string() +
                         "; the conversation will not be available to -c/--continue");
                    return false;
                }
                stream << data.dump();
            }
            fs::rename(temporary, file_path, ec);
            if (ec) {
                warn("could not save " + file_path.filename().string() + ": " + ec.message());
                return false;
            }
            return true;
        }

        void write_text(fs::path const& file_path, std::string_view text) {
            std::error_code ec;
            fs::create_directories(file_path.parent_path(), ec);
            fs::path const temporary = file_path.string() + ".tmp";
            {
                std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
                if (!stream) {
                    warn("could not save " + file_path.filename().string());
                    return;
                }
                stream << text << '\n';
            }
            fs::rename(temporary, file_path, ec);
            if (ec) {
                warn("could not save " + file_path.filename().string() + ": " + ec.message());
            }
        }

        std::optional<conversation_record> read_record(fs::path const& file_path, double max_age) {
            std::ifstream stream(file_path);
            if (!stream) {
                return std::nullopt;
            }
            json data = json::parse(stream, nullptr, false);
            if (data.is_discarded() || !data.is_object()) {
                return std::nullopt;
            }
            auto record = record_from_json(data);
            if (max_age > 0 && record.updated > 0 && now_seconds() - record.updated > max_age) {
                return std::nullopt;
            }
            return record;
        }

        std::string debate_key(std::vector<std::string> names) {
            std::ranges::sort(names);
            std::string key = "debate";
            for (auto const& name : names) {
                key += '-';
                key += name;
            }
            return key;
        }

    } // namespace

    fs::path directory() {
        std::string const base = env_value("XDG_STATE_HOME");
        if (!base.empty()) {
            return fs::path(base) / "ai";
        }
        std::string const home = env_value("HOME");
        return fs::path(home.empty() ? "/" : home) / ".local" / "state" / "ai";
    }

    fs::path path(std::string_view provider) {
        return directory() / (std::string(provider) + ".json");
    }

    fs::path terminals_dir() {
        return directory() / "terminals";
    }

    fs::path terminal_path(std::string_view provider, std::string_view scope) {
        return terminals_dir() / std::string(scope.empty() ? terminal_scope() : scope) /
               (std::string(provider) + ".json");
    }

    fs::path last_path(std::string_view scope) {
        if (scope.empty()) {
            return directory() / "last";
        }
        return terminals_dir() / std::string(scope) / "last";
    }

    std::string terminal_scope() {
        return dana::terminal::scope(dana::terminal::current());
    }

    std::optional<conversation_record> load(std::string_view provider, double max_age) {
        return read_record(path(provider), max_age);
    }

    std::optional<conversation_record>
    load_terminal(std::string_view provider, std::string_view scope, double max_age) {
        return read_record(terminal_path(provider, scope), max_age);
    }

    conversation_record save(std::string_view provider, conversation_record const& record, std::string_view scope) {
        std::string const actual_scope = scope.empty() ? terminal_scope() : std::string(scope);
        json const        data         = record_to_json(record);
        write_json(path(provider), data);
        write_json(terminal_path(provider, actual_scope), data);
        write_text(last_path(actual_scope), provider);
        write_text(last_path(), provider);
        return record_from_json(data);
    }

    void clear(std::string_view provider, std::string_view scope) {
        std::string const actual_scope = scope.empty() ? terminal_scope() : std::string(scope);
        std::error_code   ec;
        fs::remove(path(provider), ec);
        fs::remove(terminal_path(provider, actual_scope), ec);
    }

    fs::path debate_path(std::vector<std::string> names) {
        return directory() / (debate_key(std::move(names)) + ".json");
    }

    std::optional<std::map<std::string, conversation_record>> load_debate(
      std::vector<std::string> const& names,
      double                          max_age) {
        std::ifstream stream(debate_path(names));
        if (!stream) {
            return std::nullopt;
        }
        json data = json::parse(stream, nullptr, false);
        if (data.is_discarded() || !data.is_object()) {
            return std::nullopt;
        }
        auto const updated = data.value("updated", 0.0);
        if (max_age > 0 && updated > 0 && now_seconds() - updated > max_age) {
            return std::nullopt;
        }
        if (!data.contains("participants") || !data["participants"].is_object()) {
            return std::nullopt;
        }
        std::map<std::string, conversation_record> out;
        for (auto const& [name, value] : data["participants"].items()) {
            if (value.is_object()) {
                out[name] = record_from_json(value);
            }
        }
        return out;
    }

    void save_debate(std::vector<std::string> const&                   names,
                     std::map<std::string, conversation_record> const& participants,
                     std::string_view                                  scope) {
        std::string const actual_scope = scope.empty() ? terminal_scope() : std::string(scope);
        json              records      = json::object();
        for (auto const& [name, record] : participants) {
            records[name] = record_to_json(record);
        }
        json data{
          {     "updated",      now_seconds()},
          {   "providers",              names},
          {"participants", std::move(records)},
        };
        write_json(debate_path(names), data);
        for (auto const& [name, record] : participants) {
            write_json(terminal_path(name, actual_scope), record_to_json(record));
        }
        write_text(last_path(actual_scope), "debate");
        write_text(last_path(), "debate");
    }

    void clear_debate(std::vector<std::string> const& names, std::string_view scope) {
        std::string const actual_scope = scope.empty() ? terminal_scope() : std::string(scope);
        std::error_code   ec;
        fs::remove(debate_path(names), ec);
        for (auto const& name : names) {
            fs::remove(terminal_path(name, actual_scope), ec);
        }
    }

} // namespace dana::ai::state
