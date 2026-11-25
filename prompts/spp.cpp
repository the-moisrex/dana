#include "../tools/spp.hpp"

#include "../compress/compress.hpp"
#include "../compress/cpp_scan.hpp"
#include "../compress/cpp_symbols.hpp"
#include "../runtime/args.hpp"
#include "../runtime/completion.hpp"
#include "../runtime/embed.hpp"
#include "../runtime/errors.hpp"
#include "../runtime/project.hpp"
#include "../runtime/prompt_registry.hpp"
#include "../tools/symbol_index.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <iterator>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dana {

    namespace {

        struct spp_request {
            std::vector<std::string> symbols;
            bool                     file                = false;
            bool                     deps                = false;
            bool                     deps_all            = false;
            bool                     deps_outline        = false;
            bool                     deps_all_outline    = false;
            bool                     callers             = false;
            bool                     callers_all         = false;
            bool                     callers_outline     = false;
            bool                     callers_all_outline = false;
        };

        [[nodiscard]] spp_request parse_spp_args(text::string_views args) {
            spp_request request;

            for (std::size_t i = 0; i < args.size(); ++i) {
                std::string_view const arg = args[i];

                if (arg == "--head") {
                    ++i;
                    continue;
                }
                if (is_number_flag(arg) || arg == kNoDedupFlag) {
                    continue;
                }
                if (arg == "+file") {
                    request.file = true;
                } else if (arg == "+deps") {
                    request.deps = true;
                } else if (arg == "+deps.all") {
                    request.deps_all = true;
                } else if (arg == "+deps.outline") {
                    request.deps_outline = true;
                } else if (arg == "+deps.all.outline") {
                    request.deps_all_outline = true;
                } else if (arg == "+callers") {
                    request.callers = true;
                } else if (arg == "+callers.all") {
                    request.callers_all = true;
                } else if (arg == "+callers.outline") {
                    request.callers_outline = true;
                } else if (arg == "+callers.all.outline") {
                    request.callers_all_outline = true;
                } else if (arg.size() > 1 && arg.front() == '+') {
                    throw usage_error{std::format(
                      "spp: unknown modifier: {}\n"
                      "Valid modifiers: +file, +deps, +deps.all, +deps.outline, +deps.all.outline, "
                      "+callers, +callers.all, +callers.outline, +callers.all.outline\n",
                      arg)};
                } else if (arg.size() > 1 && arg.front() == '-' && arg != "-") {
                    throw usage_error{std::format("spp: unknown option: {}\n", arg)};
                } else {
                    request.symbols.emplace_back(arg);
                }
            }

            return request;
        }

        [[nodiscard]] std::string path_label(std::string_view path) {
            return path.empty() ? std::string{"<unknown>"} : std::string(path);
        }

        void append_numbered_text(std::string& output, std::string_view text, std::size_t start_line) {
            std::size_t line_no = start_line == 0 ? 1 : start_line;
            std::size_t pos     = 0;
            while (pos < text.size()) {
                auto const       nl = text.find('\n', pos);
                std::string_view line =
                  text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
                append_numbered_line(output, line_no, line);
                ++line_no;
                if (nl == std::string_view::npos) {
                    break;
                }
                pos = nl + 1;
            }
        }

        void append_expansion(
          std::string&               output,
          std::string_view           symbol,
          tools::expansion const&    expansion,
          std::optional<std::size_t> head_lines) {
            output += std::format("Symbol: {}\n", symbol);
            if (expansion.parts.empty()) {
                output += "(no expansion found)\n\n";
                return;
            }
            for (auto const& part : expansion.parts) {
                output += std::format("File: {}:{}\n", path_label(part.location.path), part.location.line);
                output += "\n```cpp\n";
                std::string const text = trim_context_nl(part.text, head_lines);
                append_numbered_text(output, text, part.location.line);
                output += "```\n\n";
            }
        }

        [[nodiscard]] std::string
        render_outline(std::filesystem::path const& root, std::string const& path, environment const& env) {
            std::string const content = read_file(root / path);
            if (content.empty()) {
                return {};
            }

            compress::region region;
            region.path = path;
            region.lang = "cpp";
            region.text = content;

            compress::signals signals;
            signals.project = project::detect(root, env);
            signals.level   = 1;

            auto outcome = compress::cpp_symbols::compress(region, signals);
            if (!outcome.markdown.empty()) {
                return outcome.markdown;
            }
            return compress::cpp::render(compress::cpp::scan(content), 1);
        }

        void append_outline(std::string&                 output,
                            std::string const&           path,
                            std::filesystem::path const& root,
                            environment const&           env) {
            std::string const outline = render_outline(root, path, env);
            if (outline.empty()) {
                return;
            }
            output += std::format("### {}\n\n", path);
            output += "```cpp\n";
            output += outline;
            if (outline.back() != '\n') {
                output.push_back('\n');
            }
            output += "```\n\n";
        }

        [[nodiscard]] std::vector<tools::symbol_entry> unique_entries(std::vector<tools::symbol_entry> const& entries) {
            std::vector<tools::symbol_entry> result;
            std::unordered_set<std::string>  seen;
            for (auto const& entry : entries) {
                auto const key = entry.name + '\t' + entry.path + '\t' + std::to_string(entry.line);
                if (seen.insert(key).second) {
                    result.push_back(entry);
                }
            }
            return result;
        }

        // The source lines `start`..`end` of `content` (1-based, inclusive), with
        // `head_lines` capping the kept lines. Used for indexed entries, where the
        // exact range is already known and clang would resolve a same-named
        // standard-library symbol instead.
        [[nodiscard]] std::string slice_lines(
          std::string const&         content,
          std::size_t                start,
          std::size_t                end,
          std::optional<std::size_t> head_lines) {
            start = start == 0 ? 1 : start;
            end   = std::max(start, end == 0 ? start : end);
            if (head_lines && *head_lines < end - start + 1) {
                end = start + *head_lines;
            }

            std::string out;
            std::size_t line_no = 0;
            std::size_t pos     = 0;
            while (pos <= content.size()) {
                auto const       nl   = content.find('\n', pos);
                std::string_view line = std::string_view(content).substr(pos, nl == std::string::npos ? nl : nl - pos);
                ++line_no;
                if (line_no >= start && line_no <= end) {
                    if (!out.empty()) {
                        out.push_back('\n');
                    }
                    out.append(line);
                }
                if (line_no >= end || nl == std::string::npos) {
                    break;
                }
                pos = nl + 1;
            }
            return out;
        }

        void append_indexed_entry(
          std::string&                 output,
          tools::symbol_entry const&   entry,
          std::filesystem::path const& root,
          std::optional<std::size_t>   head_lines) {
            std::string const content = read_file(root / entry.path);
            if (content.empty()) {
                return;
            }
            std::string const text = slice_lines(content, entry.line, entry.end_line, head_lines);
            if (text.empty()) {
                return;
            }

            output += std::format("Symbol: {}\nFile: {}:{}\n\n```cpp\n", entry.name, entry.path, entry.line);
            append_numbered_text(output, text, entry.line);
            output += "```\n\n";
        }

        void append_uncompressed_entries(
          std::string&                            output,
          std::string_view                        title,
          std::vector<tools::symbol_entry> const& entries,
          std::filesystem::path const&            root,
          std::optional<std::size_t>              head_lines) {
            if (entries.empty()) {
                return;
            }
            output += std::format("## {}\n\n", title);
            for (auto const& entry : entries) {
                append_indexed_entry(output, entry, root, head_lines);
            }
        }

        void append_outline_entries(
          std::string&                            output,
          std::string_view                        title,
          std::vector<tools::symbol_entry> const& entries,
          std::filesystem::path const&            root,
          environment const&                      env,
          std::unordered_set<std::string>&        emitted) {
            if (entries.empty()) {
                return;
            }

            std::vector<std::string> paths;
            for (auto const& entry : entries) {
                if (emitted.insert(entry.path).second) {
                    paths.push_back(entry.path);
                }
            }
            if (paths.empty()) {
                return;
            }

            output += std::format("## {}\n\n", title);
            for (auto const& path : paths) {
                append_outline(output, path, root, env);
            }
        }

        [[nodiscard]] std::optional<tools::symbol_entry>
        entry_for(std::string_view symbol, tools::expansion const& expansion, std::filesystem::path const& root) {
            if (auto entry = tools::find_symbol(symbol, root)) {
                return entry;
            }
            if (!expansion.parts.empty()) {
                auto const& location = expansion.parts.front().location;
                return tools::symbol_entry{
                  .name      = std::string(symbol),
                  .path      = location.path,
                  .line      = location.line,
                  .end_line  = location.line,
                  .kind      = {},
                  .signature = {},
                };
            }
            return std::nullopt;
        }

        // The `+` modifiers, shared by completion and the help text.
        constexpr std::array<completion::item, 9> kSppModifiers = {
          {
           {.value = "+file", .description = "Compressed outline of the defining file(s)"},
           {.value = "+deps", .description = "Direct dependencies, uncompressed"},
           {.value = "+deps.all", .description = "Transitive dependencies, uncompressed"},
           {.value = "+deps.outline", .description = "Direct dependencies, compressed outline"},
           {.value = "+deps.all.outline", .description = "Transitive dependencies, compressed outline"},
           {.value = "+callers", .description = "Direct callers, uncompressed"},
           {.value = "+callers.all", .description = "Transitive callers, uncompressed"},
           {.value = "+callers.outline", .description = "Direct callers, compressed outline"},
           {.value = "+callers.all.outline", .description = "Transitive callers, compressed outline"},
           },
        };

        void complete_spp(completion_request const& request, std::ostream& out) {
            // The token after `--head` is a number; leave it to the shell.
            if (!request.prior_args.empty() && request.prior_args.back() == "--head") {
                return;
            }
            if (request.prefix.starts_with('+')) {
                completion::write_items(out, request.prefix, kSppModifiers);
                return;
            }
            if (request.prefix.empty()) {
                completion::write_items(out, request.prefix, kSppModifiers);
                return;
            }
            if (request.prefix.front() == '-') {
                return;
            }
            tools::complete_symbols(request.prefix, request.prior_args, request.git_root, out);
        }

    } // namespace

    static void execute_spp(view const& v, result& r) {
        auto const head_lines = head_lines_or_throw(v);
        auto const request    = parse_spp_args(v.args);

        if (request.symbols.empty()) {
            throw usage_error{"Usage: prompt spp [--head N] [+modifiers] <symbol> [symbol...]\n"};
        }

        std::filesystem::path const root = v.env.git_root;

        std::string output;
        output += "Additional C++ symbol context:\n\n";

        std::vector<tools::symbol_entry> dep_direct;
        std::vector<tools::symbol_entry> dep_all;
        std::vector<tools::symbol_entry> caller_direct;
        std::vector<tools::symbol_entry> caller_all;
        std::vector<std::string>         file_paths;

        for (auto const& symbol : request.symbols) {
            auto expansion = tools::expand_symbol(symbol);
            append_expansion(output, symbol, expansion, head_lines);

            for (auto const& part : expansion.parts) {
                if (std::ranges::find(file_paths, part.location.path) == file_paths.end()) {
                    file_paths.push_back(part.location.path);
                }
            }

            auto const entry = entry_for(symbol, expansion, root);
            if (!entry) {
                continue;
            }

            if (request.deps || request.deps_all || request.deps_outline || request.deps_all_outline) {
                tools::dependency_options options;
                options.transitive = request.deps_all || request.deps_all_outline;
                auto  found        = tools::dependencies(*entry, root, options);
                auto& target       = (request.deps_all || request.deps_all_outline) ? dep_all : dep_direct;
                target.insert(target.end(),
                              std::make_move_iterator(found.begin()),
                              std::make_move_iterator(found.end()));
            }
            if (request.callers || request.callers_all || request.callers_outline || request.callers_all_outline) {
                tools::dependency_options options;
                options.transitive = request.callers_all || request.callers_all_outline;
                auto  found        = tools::callers(*entry, root, options);
                auto& target       = (request.callers_all || request.callers_all_outline) ? caller_all : caller_direct;
                target.insert(target.end(),
                              std::make_move_iterator(found.begin()),
                              std::make_move_iterator(found.end()));
            }
        }

        dep_direct    = unique_entries(dep_direct);
        dep_all       = unique_entries(dep_all);
        caller_direct = unique_entries(caller_direct);
        caller_all    = unique_entries(caller_all);

        if (request.deps) {
            append_uncompressed_entries(output, "Dependencies (uncompressed)", dep_direct, root, head_lines);
        }
        if (request.deps_all) {
            append_uncompressed_entries(output, "Dependencies (transitive, uncompressed)", dep_all, root, head_lines);
        }
        if (request.callers) {
            append_uncompressed_entries(output, "Callers (uncompressed)", caller_direct, root, head_lines);
        }
        if (request.callers_all) {
            append_uncompressed_entries(output, "Callers (transitive, uncompressed)", caller_all, root, head_lines);
        }

        std::unordered_set<std::string> emitted_outlines;
        if (request.deps_outline) {
            append_outline_entries(output, "Dependencies (outline)", dep_direct, root, v.env, emitted_outlines);
        }
        if (request.deps_all_outline) {
            append_outline_entries(
              output,
              "Dependencies (transitive, outline)",
              dep_all,
              root,
              v.env,
              emitted_outlines);
        }
        if (request.callers_outline) {
            append_outline_entries(output, "Callers (outline)", caller_direct, root, v.env, emitted_outlines);
        }
        if (request.callers_all_outline) {
            append_outline_entries(output, "Callers (transitive, outline)", caller_all, root, v.env, emitted_outlines);
        }
        if (request.file) {
            output += "## File context (outline)\n\n";
            for (auto const& path : file_paths) {
                if (emitted_outlines.insert(path).second) {
                    append_outline(output, path, root, v.env);
                }
            }
        }

        output += "Use the symbol context above when analyzing the issue.";
        r.out   = std::move(output);
        r.err   = {};
    }

    static void render_help_spp(std::ostream& os) {
        os << R"EOF(Usage: prompt spp [--head N] [+modifiers] <symbol> [symbol...]

Builds a C++ debugging prompt and expands the given symbols through `spp`.
The symbol's declaration file and line are printed before each expansion, and
the expanded lines are numbered from that declaration line.

Modifiers (order-independent, apply to every symbol on the line):
  +file                 Compressed outline of the file(s) defining the symbol
  +deps                 Direct dependencies, uncompressed
  +deps.all             Transitive dependencies, uncompressed
  +deps.outline         Direct dependencies, compressed outline
  +deps.all.outline     Transitive dependencies, compressed outline
  +callers              Direct callers/usages, uncompressed
  +callers.all          Transitive callers/usages, uncompressed
  +callers.outline      Direct callers, compressed outline
  +callers.all.outline  Transitive callers, compressed outline

Options:
  --head N   Keep only the first N lines of each uncompressed expansion
)EOF";
    }

    // This file is the prompt: it implements it and registers it at static-init time
    // (no central list — see sdk/prompt_registry.hpp).
    [[maybe_unused]] bool const registered = dana::register_prompt(
      "spp",
      "Builds a C++ debugging prompt and expands the given symbols through `spp`.",
      &execute_spp,
      &render_help_spp,
      &complete_spp);

} // namespace dana
