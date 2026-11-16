#include "../core/process.hpp"
#include "../sdk/errors.hpp"
#include "../sdk/file_filter.hpp"
#include "../sdk/prompt_registry.hpp"
#include "../sdk/schema.hpp"
#include "../sdk/stage.hpp"

#include <cstddef>
#include <filesystem>
#include <format>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace dana::prompts {

    namespace {

        struct glob_options {
            std::string pattern;
            std::string path;
            std::size_t head = 0;
        };

        glob_options parse_glob(view const& v) {
            glob_options opts;
            for (std::size_t i = 0; i < v.args.size(); ++i) {
                std::string_view const arg = v.args[i];
                if (arg == "--path") {
                    if (++i >= v.args.size()) {
                        throw usage_error{"glob: --path needs a directory"};
                    }
                    opts.path = std::string(v.args[i]);
                } else if (arg == "--head") {
                    if (++i >= v.args.size()) {
                        throw usage_error{"glob: --head needs a number"};
                    }
                    try {
                        opts.head = static_cast<std::size_t>(std::stoull(std::string(v.args[i])));
                    } catch (...) {
                        throw usage_error{"glob: --head needs a number"};
                    }
                } else if (arg.size() > 1 && arg.front() == '-') {
                    throw usage_error{"glob: unknown option " + std::string(arg)};
                } else if (opts.pattern.empty()) {
                    opts.pattern = std::string(arg);
                } else {
                    throw usage_error{"glob: at most one PATTERN"};
                }
            }
            return opts;
        }

        bool have_rg() {
            auto const res = process::run_script("command -v rg >/dev/null 2>&1");
            return res.exit_code == 0;
        }

        std::string run_glob(glob_options const& opts, file_filter const& filter) {
            std::string out;
            bool        ran = false;
            if (have_rg()) {
                std::vector<std::string> argv{"rg", "--files", "--color", "never", "--sort", "path"};
                if (!opts.pattern.empty() && opts.pattern != "*") {
                    argv.emplace_back("-g");
                    argv.emplace_back(opts.pattern);
                }
                for (auto const& glob : filter.rg_globs()) {
                    argv.emplace_back("-g");
                    argv.emplace_back(glob);
                }
                argv.emplace_back(opts.path);
                auto const res = process::command(argv).discard_stderr().run();
                if (res.exit_code == 0 || res.exit_code == 1) {
                    out = res.stdout_data;
                    ran = true;
                }
            }
            if (!ran) {
                // Fallback: find only understands a basename glob, which is the common case.
                std::string const name = opts.pattern.empty() ? "*" : opts.pattern;
                auto const        res =
                  process::run_script(R"(find "$1" -type f -name "$2" 2>/dev/null | sort | sed 's|^\./||')",
                                      {"glob", opts.path, name});
                out = res.stdout_data;
            }

            if (filter.exclude().empty()) {
                return out;
            }

            // The exclusion patterns are exact, so filter the listing in-process; a
            // path-shaped pattern is not expressible as an `rg -g` glob reliably.
            std::string kept;
            std::size_t start = 0;
            while (start <= out.size()) {
                auto const             end = out.find('\n', start);
                std::string_view const line =
                  std::string_view(out).substr(start, end == std::string::npos ? end : end - start);
                if (!line.empty() && !filter.excluded(std::filesystem::path{std::string(line)})) {
                    kept += line;
                    kept += '\n';
                }
                if (end == std::string::npos) {
                    break;
                }
                start = end + 1;
            }
            return kept;
        }

        void complete_glob(completion_request const& req, std::ostream& os) {
            auto const* desc = find_prompt("glob");
            if (desc != nullptr && desc->input != nullptr) {
                schema::complete(*desc->input, req, os);
            }
        }

        constexpr std::string_view kGlobSummary =
          "List the files matching a glob pattern (like opencode's glob tool), sorted by path.";

        schema::tool glob_schema() {
            return schema::make_tool(
              "glob",
              std::string(kGlobSummary),
              {
                schema::str_flag("path",
                                 "Directory to search in (default: the git root, then the current directory)",
                                 "DIR"),
                schema::int_flag("head", "Keep only the first N paths"),
              },
              {
                schema::argument("PATTERN", "Glob pattern to match, e.g. '**/*.cpp' (default: every file)"),
              });
        }

        // The declaration is a static because the descriptor keeps a pointer to it; the
        // registry itself never drops a tool (it aborts on overflow), so the allocation
        // is fine here.
        // NOLINTNEXTLINE(bugprone-throwing-static-initialization)
        schema::tool const k_glob_tool = glob_schema();

    } // namespace

    static void execute_glob(view const& v, result& r) {
        file_filter const filter = file_filter::from(v.prior, v.env);
        glob_options      opts   = parse_glob(v);
        if (opts.path.empty()) {
            opts.path = filter.has_root() ? filter.root().string() : ".";
        }
        std::string out = run_glob(opts, filter);

        if (opts.head > 0) {
            std::size_t lines = 0;
            std::size_t cut   = out.size();
            for (std::size_t i = 0; i < out.size(); ++i) {
                if (out[i] != '\n') {
                    continue;
                }
                if (++lines >= opts.head) {
                    cut = i + 1;
                    break;
                }
            }
            out.resize(cut);
        }
        r.out = std::move(out);
    }

    static void render_help_glob(std::ostream& os) {
        os << R"EOF(Usage: prompt glob [--path DIR] [--head N] [PATTERN]

List the files matching PATTERN, sorted by path, using ripgrep (`rg --files -g`)
and falling back to `find`. With no PATTERN every file is listed. Paths are
printed as `rg` returns them (relative to DIR). `.files.scope` becomes the
default DIR and `.files.exclude` patterns are skipped.

Examples:
  prompt glob '**/*.cpp'
  prompt glob --path src '*.hpp' | prompt .files
  prompt glob --head 50

)EOF";
        os << schema::render_options(k_glob_tool);
    }

    // This file is the prompt: it implements it and registers it at static-init time
    // (no central list — see sdk/prompt_registry.hpp). The summary is a
    // static-storage string_view; `register_prompt` keeps the view, so never pass a
    // temporary std::string here.
    bool const registered_glob = dana::register_prompt(
      "glob",
      kGlobSummary,
      &execute_glob,
      &render_help_glob,
      &complete_glob,
      chain_role::normal,
      true,
      nullptr,
      nullptr,
      false,
      true,
      &k_glob_tool);

} // namespace dana::prompts
