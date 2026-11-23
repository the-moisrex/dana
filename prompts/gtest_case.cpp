#include "../core/git.hpp"
#include "../core/strings.hpp"
#include "../runtime/args.hpp"
#include "../runtime/embed.hpp"
#include "../runtime/prompt_registry.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dana {

    namespace {

        // The source extensions the script hands to `git grep`.
        constexpr std::array<std::string_view, 10> kFileExtensions = {
          ".c",
          ".cc",
          ".cpp",
          ".cxx",
          ".c++",
          ".h",
          ".hh",
          ".hpp",
          ".hxx",
          ".ixx",
        };

        struct [[nodiscard]] test_query {
            std::string suite;
            std::string name;
        };

        struct [[nodiscard]] test_block {
            std::string file;
            std::string full_name;
            std::string matched_query;
            std::string block;
        };

        // Python's str.strip(): whitespace on both sides, including newlines.
        std::string_view trim_ws(std::string_view text) {
            auto const is_space = [](char character) {
                return character == ' ' || character == '\t' || character == '\n' || character == '\r' ||
                       character == '\f' || character == '\v';
            };
            while (!text.empty() && is_space(text.front())) {
                text.remove_prefix(1);
            }
            while (!text.empty() && is_space(text.back())) {
                text.remove_suffix(1);
            }
            return text;
        }

        std::string display_query(test_query const& query) {
            return query.name.empty() ? query.suite : query.suite + "." + query.name;
        }

        test_query parse_test_name(std::string_view value) {
            static std::regex const macro_re(
              R"(^\s*(?:TEST|TEST_F|TEST_P|TYPED_TEST|TYPED_TEST_P)\s*\(\s*([^,]+?)\s*,\s*([^,)]+?)\s*\)\s*$)",
              std::regex::ECMAScript);

            std::string const raw(trim_ws(value));
            std::smatch       match;
            if (std::regex_match(raw, match, macro_re)) {
                return {.suite = std::string(trim_ws(match.str(1))), .name = std::string(trim_ws(match.str(2)))};
            }

            std::string normalized = raw;
            if (normalized.contains('/')) {
                std::vector<std::string> parts;
                std::size_t              start = 0;
                while (start <= normalized.size()) {
                    std::size_t const      end = normalized.find('/', start);
                    std::string_view const part =
                      std::string_view(normalized).substr(start, end == std::string::npos ? end : end - start);
                    if (!part.empty()) {
                        parts.emplace_back(part);
                    }
                    if (end == std::string::npos) {
                        break;
                    }
                    start = end + 1;
                }

                std::string dotted;
                for (auto part_it = parts.rbegin(); part_it != parts.rend(); ++part_it) {
                    if (part_it->contains('.')) {
                        dotted = *part_it;
                        break;
                    }
                }
                if (!dotted.empty()) {
                    normalized = dotted;
                } else if (parts.size() == 2) {
                    return {.suite = std::string(trim_ws(parts[0])), .name = std::string(trim_ws(parts[1]))};
                }
            }

            if (auto const dot = normalized.rfind('.'); dot != std::string::npos) {
                return {
                  .suite = std::string(trim_ws(std::string_view(normalized).substr(0, dot))),
                  .name  = std::string(trim_ws(std::string_view(normalized).substr(dot + 1))),
                };
            }

            return {.suite = std::string(trim_ws(normalized)), .name = ""};
        }

        bool test_matches(std::string_view suite, std::string_view name, test_query const& query, bool exact) {
            std::string const full_name = std::string(suite) + "." + std::string(name);
            std::string const wanted    = display_query(query);
            if (exact) {
                if (!query.name.empty()) {
                    return suite == query.suite && name == query.name;
                }
                return suite == query.suite || full_name == wanted;
            }
            return full_name.starts_with(wanted);
        }

        // Replace comments and string/char/raw-string literals with spaces while
        // preserving every byte offset and newline, exactly like the script's
        // strip_comments_and_strings().
        std::string strip_comments_and_strings(std::string_view text) {
            enum class state : std::uint8_t {
                code,
                line_comment,
                block_comment,
                string,
                character,
                raw_string,
            };

            constexpr std::size_t kMaxRawDelimiterLength = 16;

            std::string result;
            result.reserve(text.size());

            std::size_t const length  = text.size();
            std::size_t       index   = 0;
            state             current = state::code;
            std::string       raw_delim;

            while (index < length) {
                char const current_char = text[index];
                char const next_char    = index + 1 < length ? text[index + 1] : '\0';

                switch (current) {
                    case state::code:
                        if (current_char == '/' && next_char == '/') {
                            result.append(2, ' ');
                            index   += 2;
                            current  = state::line_comment;
                        } else if (current_char == '/' && next_char == '*') {
                            result.append(2, ' ');
                            index   += 2;
                            current  = state::block_comment;
                        } else if (current_char == 'R' && next_char == '"') {
                            std::size_t const delim_start = index + 2;
                            std::size_t       delim_end   = delim_start;
                            bool              found       = false;
                            while (delim_end < length && delim_end - delim_start <= kMaxRawDelimiterLength) {
                                char const delim_char = text[delim_end];
                                if (delim_char == '(') {
                                    found = true;
                                    break;
                                }
                                if (delim_char == ' ' || delim_char == ')' || delim_char == '\\' ||
                                    delim_char == '\t' || delim_char == '\v' || delim_char == '\f' ||
                                    delim_char == '\n' || delim_char == '\r')
                                {
                                    break;
                                }
                                ++delim_end;
                            }
                            if (found) {
                                raw_delim = ")";
                                raw_delim.append(text.substr(delim_start, delim_end - delim_start));
                                raw_delim.push_back('"');
                                result.append(delim_end + 1 - index, ' ');
                                index   = delim_end + 1;
                                current = state::raw_string;
                            } else {
                                result.push_back(current_char);
                                ++index;
                            }
                        } else if (current_char == '"') {
                            result.push_back(' ');
                            ++index;
                            current = state::string;
                        } else if (current_char == '\'') {
                            result.push_back(' ');
                            ++index;
                            current = state::character;
                        } else {
                            result.push_back(current_char);
                            ++index;
                        }
                        break;

                    case state::line_comment:
                        result.push_back(current_char == '\n' ? '\n' : ' ');
                        ++index;
                        if (current_char == '\n') {
                            current = state::code;
                        }
                        break;

                    case state::block_comment:
                        if (current_char == '*' && next_char == '/') {
                            result.append(2, ' ');
                            index   += 2;
                            current  = state::code;
                        } else {
                            result.push_back(current_char == '\n' ? '\n' : ' ');
                            ++index;
                        }
                        break;

                    case state::string:
                        if (current_char == '\\') {
                            result.push_back(' ');
                            if (index + 1 < length) {
                                result.push_back(next_char == '\n' ? '\n' : ' ');
                            }
                            index += 2;
                        } else {
                            result.push_back(current_char == '\n' ? '\n' : ' ');
                            ++index;
                            if (current_char == '"') {
                                current = state::code;
                            }
                        }
                        break;

                    case state::character:
                        if (current_char == '\\') {
                            result.push_back(' ');
                            if (index + 1 < length) {
                                result.push_back(next_char == '\n' ? '\n' : ' ');
                            }
                            index += 2;
                        } else {
                            result.push_back(current_char == '\n' ? '\n' : ' ');
                            ++index;
                            if (current_char == '\'') {
                                current = state::code;
                            }
                        }
                        break;

                    case state::raw_string:
                        if (text.compare(index, raw_delim.size(), raw_delim) == 0) {
                            result.append(raw_delim.size(), ' ');
                            index   += raw_delim.size();
                            current  = state::code;
                        } else {
                            result.push_back(current_char == '\n' ? '\n' : ' ');
                            ++index;
                        }
                        break;
                }
            }

            return result;
        }

        std::size_t skip_ws(std::string_view text, std::size_t index) {
            while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])) != 0) {
                ++index;
            }
            return index;
        }

        std::optional<std::size_t>
        find_matching(std::string_view text, std::size_t open_index, char open_char, char close_char) {
            int depth = 0;
            for (std::size_t index = open_index; index < text.size(); ++index) {
                if (text[index] == open_char) {
                    ++depth;
                } else if (text[index] == close_char) {
                    --depth;
                    if (depth == 0) {
                        return index;
                    }
                }
            }
            return std::nullopt;
        }

        std::vector<std::string> split_top_level_args(std::string_view arg_text) {
            std::vector<std::string> args;
            std::size_t              start = 0;
            int                      depth = 0;
            for (std::size_t index = 0; index < arg_text.size(); ++index) {
                char const current_char = arg_text[index];
                if (current_char == '(' || current_char == '<' || current_char == '[' || current_char == '{') {
                    ++depth;
                } else if (current_char == ')' || current_char == '>' || current_char == ']' || current_char == '}') {
                    depth = std::max(0, depth - 1);
                } else if (current_char == ',' && depth == 0) {
                    args.emplace_back(trim_ws(arg_text.substr(start, index - start)));
                    start = index + 1;
                }
            }
            args.emplace_back(trim_ws(arg_text.substr(start)));
            return args;
        }

        std::vector<test_block>
        find_test_blocks(std::string const& path, std::vector<test_query> const& queries, bool exact) {
            std::ifstream input(path, std::ios::binary);
            if (!input) {
                return {};
            }
            std::string const original{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            std::string const searchable = strip_comments_and_strings(original);

            static std::regex const macro_re(R"(\b(TEST|TEST_F|TEST_P|TYPED_TEST|TYPED_TEST_P)\b)");

            std::vector<test_block> blocks;
            for (auto match_it = std::sregex_iterator(searchable.cbegin(), searchable.cend(), macro_re);
                 match_it != std::sregex_iterator();
                 ++match_it)
            {
                auto const& match       = *match_it;
                auto const  match_start = static_cast<std::size_t>(match.position());
                auto const  index       = skip_ws(searchable, match_start + static_cast<std::size_t>(match.length()));
                if (index >= searchable.size() || searchable[index] != '(') {
                    continue;
                }

                auto const close_paren = find_matching(searchable, index, '(', ')');
                if (!close_paren) {
                    continue;
                }
                auto const args =
                  split_top_level_args(std::string_view(searchable).substr(index + 1, *close_paren - index - 1));
                if (args.size() < 2) {
                    continue;
                }

                auto const& suite = args[0];
                auto const& name  = args[1];

                std::string matched_query;
                for (auto const& query : queries) {
                    if (test_matches(suite, name, query, exact)) {
                        matched_query = display_query(query);
                        break;
                    }
                }
                if (matched_query.empty()) {
                    continue;
                }

                std::size_t const body_start = skip_ws(searchable, *close_paren + 1);
                if (body_start >= searchable.size() || searchable[body_start] != '{') {
                    continue;
                }
                auto const body_end = find_matching(searchable, body_start, '{', '}');
                if (!body_end) {
                    continue;
                }

                std::size_t start = match_start;
                while (start > 0 && (original[start - 1] == ' ' || original[start - 1] == '\t')) {
                    --start;
                }

                std::size_t end = *body_end + 1;
                while (end < original.size() && (original[end] == ' ' || original[end] == '\t' ||
                                                 original[end] == '\r' || original[end] == '\n'))
                {
                    ++end;
                }
                if (end < original.size() && original[end] == ';') {
                    ++end;
                }

                blocks.push_back({
                  .file          = path,
                  .full_name     = std::format("{}.{}", suite, name),
                  .matched_query = matched_query,
                  .block         = std::string(trim_ws(std::string_view(original).substr(start, end - start))),
                });
            }
            return blocks;
        }

        bool is_cpp_source(std::string_view path) {
            auto const slash = path.rfind('/');
            auto const dot   = path.rfind('.');
            if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) {
                return false;
            }
            auto const extension = path.substr(dot);
            return std::ranges::find(kFileExtensions, extension) != kFileExtensions.end();
        }

        std::vector<std::string> cpp_tracked_files() {
            auto files = git::tracked_files(".");
            std::erase_if(files, [](std::string const& file) {
                return !is_cpp_source(file);
            });
            return files;
        }

        // One `git grep -l` command from the script: either a regex or a literal
        // substring over a file's content.
        struct [[nodiscard]] file_matcher {
            std::string               literal;
            std::optional<std::regex> regex;

            [[nodiscard]] bool matches(std::string const& content) const {
                if (regex) {
                    return std::regex_search(content.begin(), content.end(), *regex);
                }
                return content.contains(literal);
            }
        };

        std::optional<std::regex> compile_regex(std::string const& pattern) {
            try {
                return std::regex(pattern);
            } catch (std::regex_error const&) {
                return std::nullopt;
            }
        }

        std::vector<file_matcher> build_matchers(std::vector<test_query> const& queries) {
            std::vector<file_matcher> matchers;
            for (auto const& query : queries) {
                if (!query.name.empty()) {
                    std::string const pattern = R"(\b[A-Z_]*TEST[A-Z_]*\s*\(\s*)" + text::regex_escape(query.suite) +
                                                R"(\s*,\s*)" + text::regex_escape(query.name);
                    // The escaped pattern should always compile; if it does not, the
                    // literal suite/name matchers below still find the files.
                    matchers.push_back({.literal = {}, .regex = compile_regex(pattern)});
                }
                matchers.push_back({.literal = query.suite, .regex = std::nullopt});
                if (!query.name.empty()) {
                    matchers.push_back({.literal = query.name, .regex = std::nullopt});
                }
            }
            return matchers;
        }

        std::vector<std::string> run_git_grep(std::vector<test_query> const&  queries,
                                              std::vector<std::string> const& files) {
            auto const                                   matchers = build_matchers(queries);
            std::unordered_map<std::string, std::string> contents;
            std::unordered_set<std::string>              seen;
            std::vector<std::string>                     found;

            for (auto const& matcher : matchers) {
                for (auto const& file : files) {
                    if (seen.contains(file)) {
                        continue;
                    }

                    auto [entry, inserted] = contents.try_emplace(file);
                    if (inserted) {
                        std::ifstream input(file, std::ios::binary);
                        if (input) {
                            entry->second.assign(std::istreambuf_iterator<char>(input),
                                                 std::istreambuf_iterator<char>());
                        }
                    }

                    auto const& content = entry->second;
                    if (content.empty() || content.contains('\0')) {
                        continue;
                    }
                    if (matcher.matches(content)) {
                        seen.insert(file);
                        found.push_back(file);
                    }
                }
            }
            return found;
        }

        std::string no_match_message(std::vector<test_query> const& queries, bool exact) {
            std::string rendered;
            for (auto const& query : queries) {
                if (!rendered.empty()) {
                    rendered += ", ";
                }
                rendered += display_query(query);
            }
            return "Error: No Google Test cases found for " + std::string(exact ? "exact" : "prefix") +
                   " query: " + rendered + "\n";
        }

    } // namespace

    static void execute_gtest_case(view const& v, result& r) {
        auto const head_lines = head_lines_or_throw(v, false);

        bool                     exact = false;
        std::vector<std::string> raw_args;
        for (auto arg_it = v.args.begin(); arg_it != v.args.end(); ++arg_it) {
            auto const argument = *arg_it;
            if (argument == "--head") {
                ++arg_it;
            } else if (argument == "--exact") {
                exact = true;
            } else {
                raw_args.emplace_back(argument);
            }
        }

        // gtest-case.sh embeds stdin before it validates the test names, so a
        // usage error still ships the piped context on stdout.
        std::string output;
        if (v.in.piped()) {
            output += embed_stdin(v.in.text(), head_lines);
        }

        if (raw_args.empty()) {
            throw usage_error{"Usage: prompt gtest-case [--head N] [--exact] [FILE...] <test-name> [test-name...]\n",
                              std::move(output)};
        }

        // The script's own argument loop: an existing file (when more than one
        // argument remains) is a search target, and the two-token `Suite Test`
        // form is folded into `Suite.Test`.
        std::vector<std::string> files;
        std::vector<std::string> test_names;
        // The `Tests:` header echoes the names the user asked for, in their
        // original spelling, but never the FILE search targets.
        std::vector<std::string> display_names;
        for (std::size_t index = 0; index < raw_args.size(); ++index) {
            std::size_t const remaining = raw_args.size() - index;
            if (remaining > 1 && std::filesystem::is_regular_file(raw_args.at(index))) {
                files.push_back(raw_args.at(index));
                continue;
            }
            if (remaining == 2 && !raw_args.at(index).contains('.') && !raw_args.at(index).contains('/') &&
                !(raw_args.at(index).starts_with("TEST") && raw_args.at(index).contains('(')))
            {
                test_names.push_back(raw_args.at(index) + "." + raw_args.at(index + 1));
                display_names.push_back(raw_args.at(index));
                display_names.push_back(raw_args.at(index + 1));
                ++index;
                continue;
            }
            test_names.push_back(raw_args.at(index));
            display_names.push_back(raw_args.at(index));
        }

        if (test_names.empty()) {
            throw usage_error{"Usage: prompt gtest-case [--head N] [--exact] [FILE...] <test-name> [test-name...]\n",
                              std::move(output)};
        }

        std::vector<test_query> queries;
        for (auto const& name : test_names) {
            if (!trim_ws(name).empty()) {
                queries.push_back(parse_test_name(name));
            }
        }

        std::string tests_output;
        std::string errors;

        if (files.empty() && v.env.git_root.empty()) {
            errors += "Error: Not in a git repository; provide files explicitly\n";
        } else {
            try {
                std::vector<std::string> found_files;
                if (!files.empty()) {
                    found_files = files;
                } else {
                    found_files = run_git_grep(queries, cpp_tracked_files());
                }

                std::unordered_set<std::string> seen_blocks;
                std::vector<test_block>         found;
                for (auto const& file : found_files) {
                    for (auto const& block : find_test_blocks(file, queries, exact)) {
                        if (seen_blocks.insert(block.block).second) {
                            found.push_back(block);
                        }
                    }
                }

                if (found.empty()) {
                    errors += no_match_message(queries, exact);
                } else {
                    bool first = true;
                    for (auto const& block : found) {
                        if (!first) {
                            tests_output += "\n\n";
                        }
                        first         = false;
                        tests_output += "// File: " + block.file + "\n";
                        tests_output += "// Test: " + block.full_name + "\n";
                        if (block.matched_query != block.full_name) {
                            tests_output += "// Matched: " + block.matched_query + "\n";
                        }
                        tests_output += block.block + "\n";
                    }
                }
            } catch (std::exception const& error) {
                errors += std::string("Error: ") + error.what() + "\n";
            }
        }

        std::string tests;
        for (auto const& argument : display_names) {
            if (!tests.empty()) {
                tests += ' ';
            }
            tests += argument;
        }

        output += "Additional Google Test case context:\n\n";
        output += std::format("Tests: {}\n\n", tests);
        output += "```cpp\n";
        output += trim_context_nl(tests_output, head_lines);
        output += "\n```\n\nUse the test case context above when analyzing the issue.";

        r.out = std::move(output);
        r.err = std::move(errors);
    }

    static void render_help_gtest_case(std::ostream& os) {
        os << R"EOF(Usage: prompt gtest-case [--head N] [--exact] [FILE...] <test-name> [test-name...]

Builds a debugging prompt and embeds the original source for Google Test cases.
By default, test names are prefix matches.

A test name can be written as Suite.Test, Suite/Test, Suite Test,
TEST(Suite, Test), TEST_F(Suite, Test), and so on. When FILE arguments are
omitted, tracked C/C++ sources are searched.

Options:
  --head N   Keep only the first N lines of each test case
  --exact    Match each given test name exactly
)EOF";
    }

    // This file is the prompt: it implements it and registers it at static-init time
    // (no central list — see sdk/prompt_registry.hpp).
    [[maybe_unused]] bool const registered = dana::register_prompt(
      "gtest-case",
      "Builds a debugging prompt and embeds the original source for Google Test cases. By default, test "
      "names are prefix matches.",
      &execute_gtest_case,
      &render_help_gtest_case);

} // namespace dana
