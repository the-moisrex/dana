#include "tse_screen_review.hpp"

#include "tse_common.hpp"

#include <cstddef>
#include <format>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace dana::tools::tse {

    namespace {

        [[nodiscard]] constexpr bool blank(char c) noexcept {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
        }

        [[nodiscard]] constexpr std::string_view strip(std::string_view text) noexcept {
            while (!text.empty() && blank(text.front())) {
                text.remove_prefix(1);
            }
            while (!text.empty() && blank(text.back())) {
                text.remove_suffix(1);
            }
            return text;
        }

        [[nodiscard]] constexpr std::string_view lstrip(std::string_view text) noexcept {
            while (!text.empty() && blank(text.front())) {
                text.remove_prefix(1);
            }
            return text;
        }

        [[nodiscard]] std::vector<std::string_view> split_lines(std::string_view text) {
            std::vector<std::string_view> lines;
            std::size_t                   start = 0;
            while (start <= text.size()) {
                auto const end = text.find('\n', start);
                if (end == std::string_view::npos) {
                    lines.push_back(text.substr(start));
                    break;
                }
                lines.push_back(text.substr(start, end - start));
                start = end + 1;
            }
            return lines;
        }

        [[nodiscard]] std::string csv_field(std::string value) {
            if (value.find_first_of(",\"\n\r") == std::string::npos) {
                return value;
            }
            std::string out;
            out.reserve(value.size() + 2);
            out.push_back('"');
            for (char const c : value) {
                if (c == '"') {
                    out += "\"\"";
                } else {
                    out.push_back(c);
                }
            }
            out.push_back('"');
            return out;
        }

        [[nodiscard]] std::string cell_text(json const& value) {
            if (value.is_null()) {
                return {};
            }
            if (value.is_boolean()) {
                return value.get<bool>() ? "True" : "False";
            }
            if (value.is_string()) {
                return value.get<std::string>();
            }
            return value.dump();
        }

        [[nodiscard]] std::vector<json> rows_from(json const& doc) {
            if (doc.is_object()) {
                auto const results = doc.find("results");
                if (results != doc.end() && results->is_array()) {
                    return results->get<std::vector<json>>();
                }
                if (doc.contains("symbol")) {
                    return {doc};
                }
                return {};
            }
            if (doc.is_array()) {
                return doc.get<std::vector<json>>();
            }
            return {};
        }

    } // namespace

    std::string classify_screen_text(std::string_view text) {
        auto const trimmed = strip(text);
        if (trimmed.empty()) {
            return "empty";
        }
        auto const doc = json::parse(trimmed, nullptr, false);
        if (!doc.is_discarded()) {
            if (doc.is_object()) {
                auto const results = doc.find("results");
                if (results != doc.end() && results->is_array()) {
                    return "doc";
                }
                if (doc.contains("symbol")) {
                    return "jsonl";
                }
                return "nl";
            }
            if (doc.is_array()) {
                if (doc.empty() || doc.front().is_object()) {
                    return "array";
                }
                return "nl";
            }
            return "nl";
        }
        auto const       lines = split_lines(trimmed);
        std::string_view first;
        for (auto const line : lines) {
            if (!strip(line).empty()) {
                first = lstrip(line);
                break;
            }
        }
        if (first.starts_with('{')) {
            return "jsonl";
        }
        auto const token_end = first.find_first_of(" \t\r\f\v");
        auto const token     = first.substr(0, token_end);
        if (token == "Symbol") {
            return "table";
        }
        return "nl";
    }

    std::string screen_rows_to_csv(std::string_view text) {
        auto const        doc = json::parse(text, nullptr, false);
        std::vector<json> rows;
        if (!doc.is_discarded()) {
            rows = rows_from(doc);
        } else {
            for (auto const line : split_lines(text)) {
                auto const trimmed = strip(line);
                if (trimmed.empty()) {
                    continue;
                }
                auto const row = json::parse(trimmed, nullptr, false);
                if (row.is_discarded()) {
                    throw error("not JSON rows");
                }
                rows.push_back(row);
            }
        }
        for (auto const& row : rows) {
            if (!row.is_object()) {
                throw error("rows are not objects");
            }
        }
        std::vector<std::string> columns;
        std::set<std::string>    seen;
        for (auto const& row : rows) {
            for (auto it = row.begin(); it != row.end(); ++it) {
                if (seen.insert(it.key()).second) {
                    columns.push_back(it.key());
                }
            }
        }
        if (columns.empty()) {
            return {};
        }
        std::string out;
        for (std::size_t i = 0; i < columns.size(); ++i) {
            if (i != 0) {
                out.push_back(',');
            }
            out += csv_field(columns[i]);
        }
        out.push_back('\n');
        for (auto const& row : rows) {
            for (std::size_t i = 0; i < columns.size(); ++i) {
                if (i != 0) {
                    out.push_back(',');
                }
                if (auto const it = row.find(columns[i]); it != row.end()) {
                    out += csv_field(cell_text(*it));
                }
            }
            out.push_back('\n');
        }
        if (!out.empty()) {
            out.pop_back(); // drop the final row terminator
        }
        return out;
    }

    screen_snapshot_info screen_snapshot_meta(std::string_view text) {
        auto const doc = json::parse(text, nullptr, false);
        if (doc.is_discarded() || !doc.is_object() || !doc.contains("results") || !doc.at("results").is_array()) {
            throw error("not a .tse screen snapshot (expected a results array)");
        }
        auto const meta = doc.contains("meta") && doc.at("meta").is_object() ? doc.at("meta") : json::object();
        screen_snapshot_info info;
        if (meta.contains("screened_at") && meta.at("screened_at").is_string()) {
            info.screened = meta.at("screened_at").get<std::string>();
        }
        auto const matched = doc.at("results").size();
        json       total   = nullptr;
        if (meta.contains("total_universe")) {
            total = meta.at("total_universe");
        }
        bool const has_total = !total.is_null() && !(total.is_number() && total.get<double>() == 0.0) &&
                               !(total.is_string() && total.get<std::string>().empty());
        if (has_total) {
            info.counts = std::format("{} matched / {} screened", matched, cell_text(total));
        } else {
            info.counts = std::format("{} results", matched);
        }
        if (meta.contains("command") && meta.at("command").is_string()) {
            info.command = meta.at("command").get<std::string>();
        } else if (meta.contains("args") && meta.at("args").is_array()) {
            bool first = true;
            for (auto const& arg : meta.at("args")) {
                if (!first) {
                    info.command.push_back(' ');
                }
                info.command += cell_text(arg);
                first         = false;
            }
        }
        return info;
    }

    std::size_t screen_row_count(std::string_view text) {
        auto const doc = json::parse(text, nullptr, false);
        if (doc.is_discarded() || !doc.is_array()) {
            throw error("not a JSON array");
        }
        return doc.size();
    }

} // namespace dana::tools::tse
