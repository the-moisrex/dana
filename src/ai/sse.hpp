#pragma once

// Server-sent-events framing: accumulate `data:` lines and emit one payload
// per blank line, joining a multi-line payload with newlines and ignoring
// `event:`/`id:`/`retry:`/comment lines.  Providers parse the payloads as JSON.

#include <string>
#include <string_view>
#include <vector>

namespace prompt::ai {

    class sse_decoder {
      public:
        // Feed one line (without its newline); returns the payloads completed by
        // that line (usually none or one).
        std::vector<std::string> feed(std::string_view line) {
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            if (line.empty()) {
                return dispatch();
            }
            if (line.starts_with("data:")) {
                std::string_view value = line.substr(5);
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                    value.remove_prefix(1);
                }
                data_.emplace_back(value);
            }
            return {};
        }

        // Flush a final event that was not terminated by a blank line.
        std::vector<std::string> finish() {
            return dispatch();
        }

      private:
        std::vector<std::string> dispatch() {
            std::vector<std::string> out;
            if (data_.empty()) {
                return out;
            }
            std::string payload;
            for (std::size_t i = 0; i < data_.size(); ++i) {
                if (i != 0) {
                    payload += '\n';
                }
                payload += data_[i];
            }
            data_.clear();
            if (payload != "[DONE]") {
                out.push_back(std::move(payload));
            }
            return out;
        }

        std::vector<std::string> data_;
    };

} // namespace prompt::ai
