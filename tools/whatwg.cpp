#include "whatwg.hpp"

#include "../ai/http.hpp"
#include "../core/process.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <unordered_set>
#include <vector>

namespace dana::tools {

    namespace {

        constexpr std::string_view spec_url = "https://url.spec.whatwg.org/";

        struct attribute {
            std::string_view name;
            std::string_view value;
        };

        struct tag {
            std::string_view       name;
            bool                   closing      = false;
            bool                   self_closing = false;
            std::size_t            start        = 0;
            std::size_t            end          = 0;
            std::vector<attribute> attributes;
        };

        [[nodiscard]] bool is_space(char const c) noexcept {
            return std::isspace(static_cast<unsigned char>(c)) != 0;
        }

        [[nodiscard]] bool is_name_char(char const c) noexcept {
            return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_' || c == ':';
        }

        [[nodiscard]] std::string lower(std::string_view text) {
            std::string out(text);
            for (char& c : out) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return out;
        }

        [[nodiscard]] std::string_view trim(std::string_view text) noexcept {
            while (!text.empty() && is_space(text.front())) {
                text.remove_prefix(1);
            }
            while (!text.empty() && is_space(text.back())) {
                text.remove_suffix(1);
            }
            return text;
        }

        [[nodiscard]] std::string_view attribute_value(tag const& element, std::string_view name) noexcept {
            for (auto const& attribute : element.attributes) {
                if (attribute.name == name) {
                    return attribute.value;
                }
            }
            return {};
        }

        [[nodiscard]] std::vector<tag> tokenize(std::string_view html) {
            std::vector<tag> tags;
            std::size_t      pos = 0;
            while ((pos = html.find('<', pos)) != std::string_view::npos) {
                if (html.substr(pos).starts_with("<!--")) {
                    auto const close = html.find("-->", pos + 4);
                    pos              = close == std::string_view::npos ? html.size() : close + 3;
                    continue;
                }
                if (pos + 1 >= html.size() || html[pos + 1] == '!' || html[pos + 1] == '?') {
                    auto const close = html.find('>', pos + 1);
                    pos              = close == std::string_view::npos ? html.size() : close + 1;
                    continue;
                }

                std::size_t i = pos + 1;
                tag         element;
                element.start = pos;
                if (i < html.size() && html[i] == '/') {
                    element.closing = true;
                    ++i;
                }
                std::size_t const name_start = i;
                while (i < html.size() && is_name_char(html[i])) {
                    ++i;
                }
                element.name = html.substr(name_start, i - name_start);
                if (element.name.empty()) {
                    ++pos;
                    continue;
                }

                while (i < html.size()) {
                    while (i < html.size() && is_space(html[i])) {
                        ++i;
                    }
                    if (i >= html.size()) {
                        break;
                    }
                    if (html[i] == '>') {
                        ++i;
                        break;
                    }
                    if (html[i] == '/' && i + 1 < html.size() && html[i + 1] == '>') {
                        element.self_closing  = true;
                        i                    += 2;
                        break;
                    }
                    std::size_t const attr_start = i;
                    while (i < html.size() && !is_space(html[i]) && html[i] != '=' && html[i] != '>' && html[i] != '/')
                    {
                        ++i;
                    }
                    std::string_view const attr_name = html.substr(attr_start, i - attr_start);
                    while (i < html.size() && is_space(html[i])) {
                        ++i;
                    }
                    std::string_view attr_value;
                    if (i < html.size() && html[i] == '=') {
                        ++i;
                        while (i < html.size() && is_space(html[i])) {
                            ++i;
                        }
                        if (i < html.size() && (html[i] == '"' || html[i] == '\'')) {
                            char const        quote       = html[i++];
                            std::size_t const value_start = i;
                            while (i < html.size() && html[i] != quote) {
                                ++i;
                            }
                            attr_value = html.substr(value_start, i - value_start);
                            if (i < html.size()) {
                                ++i;
                            }
                        } else {
                            std::size_t const value_start = i;
                            while (i < html.size() && !is_space(html[i]) && html[i] != '>') {
                                ++i;
                            }
                            attr_value = html.substr(value_start, i - value_start);
                        }
                    }
                    if (!attr_name.empty()) {
                        element.attributes.push_back({.name = attr_name, .value = attr_value});
                    }
                }
                element.end = i;
                tags.push_back(std::move(element));
                pos = i;
            }
            return tags;
        }

        [[nodiscard]] std::string strip_tags(std::string_view html) {
            std::string out;
            out.reserve(html.size());
            std::size_t pos = 0;
            while (pos < html.size()) {
                auto const open = html.find('<', pos);
                if (open == std::string_view::npos) {
                    out.append(html.substr(pos));
                    break;
                }
                out.append(html.substr(pos, open - pos));
                auto const close = html.find('>', open + 1);
                pos              = close == std::string_view::npos ? html.size() : close + 1;
            }
            std::string collapsed;
            collapsed.reserve(out.size());
            bool pending_space = false;
            for (char const c : out) {
                if (is_space(c)) {
                    pending_space = !collapsed.empty();
                    continue;
                }
                if (pending_space) {
                    collapsed.push_back(' ');
                    pending_space = false;
                }
                collapsed.push_back(c);
            }
            return collapsed;
        }

        [[nodiscard]] std::string config_dir() {
            char const* xdg = std::getenv("XDG_CONFIG_HOME"); // NOLINT(concurrency-mt-unsafe): read once at startup.
            if (xdg != nullptr && *xdg != '\0') {
                return std::string(xdg) + "/whatwg-url-spec";
            }
            char const* home = std::getenv("HOME"); // NOLINT(concurrency-mt-unsafe): read once at startup.
            if (home != nullptr) {
                return std::string(home) + "/.config/whatwg-url-spec";
            }
            return "/tmp/whatwg-url-spec";
        }

        [[nodiscard]] std::filesystem::path spec_file() {
            return std::filesystem::path(config_dir()) / "spec.html";
        }

        void ensure_cache(whatwg_options const& options) {
            std::error_code ec;
            std::filesystem::create_directories(config_dir(), ec);
            if (std::filesystem::exists(spec_file(), ec)) {
                return;
            }
            if (options.verbose) {
                (void) std::fprintf(stderr, "=> Downloading spec from %s...\n", std::string(spec_url).c_str());
            }
            ai::http_request request;
            request.url             = std::string(spec_url);
            request.timeout_seconds = 120;
            auto const    body      = ai::request_text(request);
            std::ofstream out(spec_file(), std::ios::binary);
            out << body;
            if (options.verbose) {
                (void) std::fputs("=> Download complete.\n", stderr);
            }
        }

        enum struct [[nodiscard]] item_type : unsigned char {
            heading,
            definition,
            algorithm
        };

        struct item {
            item_type   type = item_type::heading;
            std::string id;
            std::string title;
            int         level = 0;
            std::size_t start = 0;
            std::size_t end   = 0;
        };

        [[nodiscard]] std::vector<item> build_items(std::string_view html, std::vector<tag> const& tags) {
            std::vector<item> headings;
            std::vector<item> definitions;
            std::vector<item> algorithms;

            for (std::size_t index = 0; index < tags.size(); ++index) {
                tag const& element = tags[index];
                if (element.closing) {
                    continue;
                }
                std::string_view const name = element.name;

                if (name.size() == 2 && name[0] == 'h' && name[1] >= '2' && name[1] <= '6') {
                    auto const id = attribute_value(element, "id");
                    if (id.empty()) {
                        continue;
                    }
                    auto const next = html.find('<', element.end);
                    auto const text =
                      html.substr(element.end, next == std::string_view::npos ? next : next - element.end);
                    headings.push_back(
                      {.type  = item_type::heading,
                       .id    = std::string(id),
                       .title = strip_tags(text),
                       .level = name[1] - '0',
                       .start = element.start,
                       .end   = 0});
                } else if (name == "dfn") {
                    auto const id = attribute_value(element, "id");
                    if (id.empty()) {
                        continue;
                    }
                    definitions.push_back(
                      {.type  = item_type::definition,
                       .id    = std::string(id),
                       .title = std::string(id),
                       .level = 0,
                       .start = element.start,
                       .end   = 0});
                } else if (name == "div") {
                    auto const classes = attribute_value(element, "class");
                    if (classes.find("algorithm") == std::string_view::npos) {
                        continue;
                    }
                    auto const algorithm = attribute_value(element, "data-algorithm");
                    if (algorithm.empty()) {
                        continue;
                    }
                    algorithms.push_back(
                      {.type  = item_type::algorithm,
                       .id    = std::string(algorithm),
                       .title = std::string(algorithm),
                       .level = 0,
                       .start = element.start,
                       .end   = 0});
                }
            }

            // Heading sections end at the next heading of the same or higher level.
            for (std::size_t i = 0; i < headings.size(); ++i) {
                std::size_t end = html.size();
                if (auto const main = html.find("</main>", headings[i].start); main != std::string_view::npos) {
                    end = main;
                }
                for (std::size_t j = i + 1; j < headings.size(); ++j) {
                    if (headings[j].level <= headings[i].level) {
                        end = headings[j].start;
                        break;
                    }
                }
                headings[i].end = end;
            }

            // Algorithm elements end at their matching </div>.
            for (auto& algorithm : algorithms) {
                std::size_t depth = 0;
                for (auto const& element : tags) {
                    if (element.start < algorithm.start || element.name != "div") {
                        continue;
                    }
                    if (element.closing) {
                        if (depth > 0) {
                            --depth;
                        }
                        if (depth == 0) {
                            algorithm.end = element.end;
                            break;
                        }
                    } else if (!element.self_closing) {
                        ++depth;
                    }
                }
                if (algorithm.end == 0) {
                    algorithm.end = html.size();
                }
            }

            // Definitions: the enclosing <dt> plus its <dd>.
            for (auto& definition : definitions) {
                std::size_t start = definition.start;
                for (auto const& element : tags) {
                    if (element.start >= definition.start) {
                        break;
                    }
                    if (!element.closing && element.name == "dt") {
                        start = element.start;
                    }
                }
                std::size_t end = definition.start;
                for (auto const& element : tags) {
                    if (element.start < definition.start) {
                        continue;
                    }
                    if (!element.closing && element.name == "dd") {
                        end = element.start;
                        break;
                    }
                }
                for (auto const& element : tags) {
                    if (element.start < end || !element.closing || element.name != "dd") {
                        continue;
                    }
                    end = element.end;
                    break;
                }
                if (end <= definition.start) {
                    end = definition.start + 1;
                }
                definition.start = start;
                definition.end   = end;
            }

            std::vector<item> items;
            items.reserve(headings.size() + definitions.size() + algorithms.size());
            items.insert(items.end(), headings.begin(), headings.end());
            items.insert(items.end(), definitions.begin(), definitions.end());
            items.insert(items.end(), algorithms.begin(), algorithms.end());
            return items;
        }

        [[nodiscard]] std::string collapse_list_blank_lines(std::string_view text) {
            std::vector<std::string_view> lines;
            std::size_t                   start = 0;
            while (start <= text.size()) {
                auto const nl = text.find('\n', start);
                lines.push_back(text.substr(start, nl == std::string_view::npos ? nl : nl - start));
                if (nl == std::string_view::npos) {
                    break;
                }
                start = nl + 1;
            }

            auto const is_list = [](std::string_view line) {
                std::size_t i = 0;
                while (i < line.size() && is_space(line[i])) {
                    ++i;
                }
                if (i >= line.size()) {
                    return false;
                }
                if (line[i] == '-' || line[i] == '*' || line[i] == '+') {
                    return i + 1 < line.size() && is_space(line[i + 1]);
                }
                std::size_t digits = 0;
                while (i < line.size() && isdigit(static_cast<unsigned char>(line[i])) != 0) {
                    ++i;
                    ++digits;
                }
                return digits > 0 && i + 1 < line.size() && line[i] == '.' && is_space(line[i + 1]);
            };

            std::string out;
            for (std::size_t i = 0; i < lines.size(); ++i) {
                if (lines[i].empty() && i > 0 && i + 1 < lines.size() && is_list(lines[i - 1]) && is_list(lines[i + 1]))
                {
                    continue;
                }
                out.append(lines[i]);
                if (i + 1 < lines.size()) {
                    out.push_back('\n');
                }
            }
            return out;
        }

        [[nodiscard]] std::string html_to_markdown(std::string_view html, bool raw_html) {
            if (raw_html) {
                return std::string(html);
            }
            if (!process::tool_exists("pandoc")) {
                (void) std::fputs("ERROR: pandoc is not installed. Outputting HTML instead.\n", stderr);
                return std::string(html);
            }

            auto const lua_path = std::filesystem::temp_directory_path() /
                                  std::format("prompt-whatwg-{}.lua", static_cast<long>(::getpid()));
            {
                std::ofstream lua(lua_path, std::ios::binary);
                lua << "function Link(el) return el.content end\nfunction Span(el) return el.content end\n";
            }

            std::vector<std::string> const argv{
              "pandoc",
              "-f",
              "html",
              "-t",
              "markdown_strict",
              "--strip-comments",
              "--listings",
              "--lua-filter=" + lua_path.string(),
              "--wrap=none",
              "--markdown-headings=atx",
              "--quiet"};
            auto const      result = process::command{argv}.stdin_data(std::string(html)).capture_stderr().run();
            std::error_code ec;
            std::filesystem::remove(lua_path, ec);
            if (result.exit_code != 0) {
                if (!result.stderr_data.empty()) {
                    (void) std::fprintf(stderr, "Pandoc Error: %s\n", std::string(trim(result.stderr_data)).c_str());
                }
                return std::string(html);
            }
            return collapse_list_blank_lines(trim(result.stdout_data));
        }

        [[nodiscard]] std::string list_items(std::vector<item> const& items) {
            struct heading_state {
                int level;
            };

            std::vector<heading_state> stack;
            std::string                out;
            for (auto const& entry : items) {
                if (entry.type == item_type::heading) {
                    while (!stack.empty() && stack.back().level >= entry.level) {
                        stack.pop_back();
                    }
                    stack.push_back({.level = entry.level});
                    out.append(static_cast<std::size_t>(stack.size() - 1) * 2, ' ');
                    out += "▪ ";
                    out += entry.title;
                    out.push_back('\n');
                } else {
                    out.append(static_cast<std::size_t>(stack.size()) * 2, ' ');
                    out += "• ";
                    out += entry.id;
                    out += entry.type == item_type::definition ? " (definition)\n" : " (algorithm)\n";
                }
            }
            return out;
        }

    } // namespace

    std::string whatwg_help() {
        return R"EOF(usage: whatwg-url-specs [-h] [--algorithm] [--all] [--html] [--clean-cache] [--verbose] [queries ...]

Fetch and query the WHATWG URL specification.

positional arguments:
  queries        Section keywords to search for, or 'list' to show all sections in a tree.

options:
  -h, --help     show this help message and exit
  --algorithm    Restrict search to algorithm sections only.
  --all          Print the entire specification's main content.
  --html         Output raw HTML instead of Markdown.
  --clean-cache  Delete cache and exit.
  --verbose      Enable verbose logging.
)EOF";
    }

    std::string whatwg_query(std::vector<std::string> const& queries, whatwg_options const& options) {
        if (options.clean_cache) {
            std::error_code ec;
            if (std::filesystem::exists(config_dir(), ec)) {
                std::filesystem::remove_all(config_dir(), ec);
                return "Cache cleaned.\n";
            }
            return "Cache already clean.\n";
        }

        ensure_cache(options);

        std::ifstream in(spec_file(), std::ios::binary);
        if (!in) {
            throw std::runtime_error("could not read the WHATWG URL specification cache");
        }
        std::string const html((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

        if (options.all) {
            auto const main = html.find("<main");
            auto const end  = html.find("</main>");
            if (main != std::string::npos && end != std::string::npos) {
                return html_to_markdown(std::string_view(html).substr(main, end - main), options.html);
            }
            return html_to_markdown(html, options.html);
        }

        auto const tags  = tokenize(html);
        auto       items = build_items(html, tags);

        if (options.algorithm_only) {
            std::erase_if(items, [](item const& entry) {
                return entry.type != item_type::algorithm;
            });
        }

        bool                     list_mode = false;
        std::vector<std::string> effective = queries;
        if (!effective.empty() && effective.front() == "list") {
            list_mode = true;
            effective.erase(effective.begin());
        }

        if (list_mode) {
            return list_items(items);
        }

        std::vector<item const*>        selected;
        std::unordered_set<std::string> seen;

        auto const add = [&](item const& entry) {
            if (seen.insert(entry.id).second) {
                selected.push_back(&entry);
            }
        };

        if (effective.empty()) {
            for (auto const& entry : items) {
                if (entry.id == "url-parsing") {
                    add(entry);
                }
            }
        } else {
            for (auto const& query : effective) {
                std::string id = query;
                if (query.starts_with(std::string(spec_url) + "#")) {
                    id = query.substr(std::string(spec_url).size() + 1);
                }
                std::string const needle = lower(id);

                bool exact = false;
                for (auto const& entry : items) {
                    if (lower(entry.id) == needle || lower(entry.title) == needle) {
                        add(entry);
                        exact = true;
                    }
                }
                if (exact) {
                    continue;
                }
                for (auto const& entry : items) {
                    if (lower(entry.title).find(needle) != std::string::npos ||
                        lower(entry.id).find(needle) != std::string::npos)
                    {
                        add(entry);
                    }
                }
            }
        }

        if (selected.empty()) {
            throw std::runtime_error("No sections found for the given query.");
        }

        std::string                     joined;
        std::unordered_set<std::string> parts;
        for (auto const* entry : selected) {
            std::string_view const fragment = std::string_view(html).substr(entry->start, entry->end - entry->start);
            std::string const      part(fragment);
            if (parts.insert(part).second) {
                if (!joined.empty()) {
                    joined.push_back('\n');
                }
                joined += part;
            }
        }
        return html_to_markdown(joined, options.html);
    }

} // namespace dana::tools
