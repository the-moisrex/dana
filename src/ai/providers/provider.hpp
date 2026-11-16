#pragma once

// The provider contract: every adapter opens a conversation whose send()
// yields normalised stream events, so the CLI, renderer and sinks never need
// to know which site is behind them.

#include "../events.hpp"
#include "../session.hpp"

#include <cstddef>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dana::ai {

    struct provider_info {
        std::string              name;
        std::string              label;
        std::string              origin;
        std::string              session_target;
        std::string              default_model;
        std::vector<std::string> models;
        std::size_t              inline_limit        = 64UL * 1024UL;
        bool                     needs_session       = true;
        bool                     api                 = false;
        bool                     local               = false;
        bool                     recordable          = true;
        bool                     prompt_file_upload  = true;
        bool                     auto_attach_by_size = true;
        bool                     discovers_model     = false;
        std::string              base_url;
        std::string              base_url_env;
        std::string              key_env;
        std::string              model_env;
        std::string              default_base_url;
    };

    struct model_record {
        std::string id;
        std::string name;
        std::string type;
        std::string state;
        long long   context = 0;
        std::string prompt_price;
        std::string completion_price;
    };

    struct open_options {
        std::string    system;
        std::string    model;
        bool           debug   = false;
        long           timeout = 600;
        std::string    conversation_id;
        nlohmann::json history = nlohmann::json::array();
        nlohmann::json resume  = nlohmann::json::object();
        // API providers only.
        std::string              base_url;
        std::string              api_key;
        std::vector<std::string> params;
    };

    class conversation {
      public:
        conversation(session::match match, std::string model, open_options options)
          : match_(std::move(match)),
            model_(std::move(model)),
            system_(options.system),
            debug_(options.debug),
            timeout_(options.timeout),
            conversation_id_(std::move(options.conversation_id)),
            history_(std::move(options.history)),
            resume_(std::move(options.resume)) {}

        virtual ~conversation() = default;

        virtual void send(std::string const& prompt, std::vector<std::string> const& files, event_sink const& sink) = 0;

        virtual void close() {}

        [[nodiscard]] std::string const& conversation_id() const {
            return conversation_id_;
        }

        [[nodiscard]] std::string const& model() const {
            return model_;
        }

        [[nodiscard]] nlohmann::json const& history() const {
            return history_;
        }

        [[nodiscard]] nlohmann::json const& resume() const {
            return resume_;
        }

        [[nodiscard]] int turns() const {
            return turns_;
        }

      protected:
        void record(std::string_view role, std::string_view text) {
            if (!text.empty()) {
                history_.push_back({
                  {"role", role},
                  {"text", text}
                });
            }
        }

        session::match match_;
        std::string    model_;
        std::string    system_;
        bool           debug_   = false;
        long           timeout_ = 600;
        std::string    conversation_id_;
        nlohmann::json history_ = nlohmann::json::array();
        nlohmann::json resume_  = nlohmann::json::object();
        int            turns_   = 0;
    };

    class provider {
      public:
        virtual ~provider() = default;

        [[nodiscard]] virtual provider_info const& info() const = 0;
        virtual std::unique_ptr<conversation>
        open(session::match const& match, std::string const& model, open_options const& options) = 0;

        // API providers override these; the web providers use the defaults.
        virtual std::string                                      base_url(std::string const& requested);
        virtual std::string                                      api_key(std::string const& requested);
        virtual std::vector<std::pair<std::string, std::string>> headers(
          std::string const& key,
          std::string const& base = {});
        virtual std::string               chat_url(std::string const& base);
        virtual std::vector<model_record> chat_models(std::string const& base, long timeout);
        virtual std::vector<model_record>
        chat_models_with_key(std::string const& base, long timeout, std::string const& key);
        virtual std::vector<std::string> available_models();
        virtual std::string              resolve_model(std::string const& requested, std::string const& base);
        virtual nlohmann::json           session_descriptor(open_options const& options);
        virtual std::string              error_hint(std::string const& message, std::string const& base);
    };

} // namespace dana::ai
