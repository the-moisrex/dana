#include "core/clipboard.hpp"
#include "core/fs.hpp"
#include "core/sanitize.hpp"
#include "core/strings.hpp"
#include "environment.hpp"
#include "sdk/chain_state.hpp"
#include "sdk/chaining.hpp"
#include "sdk/completion.hpp"
#include "sdk/function.hpp"
#include "sdk/history.hpp"
#include "sdk/line_completion.hpp"
#include "sdk/prompt_file.hpp"
#include "sdk/prompt_registry.hpp"
#include "sdk/sink.hpp"
#include "sdk/stage.hpp"
#include "sdk/terminal.hpp"
#include "sdk/wire.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <print>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

// The prompts shell out to the repository's tools (bin/tse, jq, …) — there is
// no in-process interpreter here, so no ASan suppressions are needed.

namespace {

    // A `.each` body runs as a child `prompt` (PROMPT_INNER=1): the parent already
    // owns the clipboard and prints one redaction legend for the whole chain, so a
    // loop of N items must not copy N times or repeat the legend N times.
    bool inner_process = false;

    // Join two blocks with one blank line, trimming their trailing newlines first.
    std::string prepend_block(std::string_view prefix, std::string_view body) {
        while (!prefix.empty() && prefix.back() == '\n') {
            prefix.remove_suffix(1);
        }
        while (!body.empty() && body.back() == '\n') {
            body.remove_suffix(1);
        }
        if (prefix.empty()) {
            return std::string(body);
        }
        if (body.empty()) {
            return std::string(prefix);
        }
        std::string out(prefix);
        out += "\n\n";
        out.append(body);
        return out;
    }

    // Every emitted segment goes through `text::append_block`, the same rule the
    // chain context uses: trim the block on both sides, skip an empty segment, and
    // separate consecutive blocks with exactly one blank line. So stdout, the
    // clipboard and the context a sink receives all have the same shape.
    struct output_accumulator {
        std::string text;
        std::string clipboard;

        void add(std::string_view output, bool no_clipboard) {
            dana::text::append_block(text, output);
            if (no_clipboard) {
                return;
            }
            dana::text::append_block(clipboard, output);
        }
    };

    // Incremental stdout/clipboard emitter for sink chains. Streamed sink output
    // has already reached the terminal; it only needs to enter the clipboard and
    // affect the separator state.
    class output_emitter {
      public:
        void emit(std::string_view output, bool no_clipboard, bool redacted) {
            output = dana::text::strip_newlines(output);
            if (output.empty()) {
                return;
            }
            if (!first_stdout_) {
                std::print(std::cout, "\n");
            }
            first_stdout_ = false;
            if (redacted && !stdout_legend_ && !inner_process) {
                std::println(std::cout, "{}", dana::sanitize::kRedactionLegend);
                stdout_legend_ = true;
            }
            std::println(std::cout, "{}", output);
            std::cout.flush();
            add_clipboard(output, no_clipboard, redacted);
        }

        void streamed(std::string_view output, bool no_clipboard, bool redacted) {
            if (!output.empty() && output.back() != '\n') {
                std::print(std::cout, "\n");
            }
            std::cout.flush();
            output = dana::text::strip_newlines(output);
            if (output.empty()) {
                return;
            }
            first_stdout_ = false;
            add_clipboard(output, no_clipboard, redacted);
        }

        // Called before a streamed sink starts writing, so its live output is
        // separated from the previous segment like a captured result would be.
        void begin_stream() const {
            if (!first_stdout_) {
                std::print(std::cout, "\n");
                std::cout.flush();
            }
        }

        [[nodiscard]] std::string clipboard() const {
            return clipboard_;
        }

      private:
        void add_clipboard(std::string_view output, bool no_clipboard, bool redacted) {
            if (no_clipboard) {
                return;
            }
            if (!dana::text::append_block(clipboard_, output)) {
                return;
            }
            if (redacted && !clipboard_legend_ && !inner_process) {
                // A one-line header, so it sits directly above the first redacted
                // block with no blank line between them.
                clipboard_        = std::string(dana::sanitize::kRedactionLegend) + "\n" + clipboard_;
                clipboard_legend_ = true;
            }
        }

        bool        first_stdout_     = true;
        bool        stdout_legend_    = false;
        bool        clipboard_legend_ = false;
        std::string clipboard_;
    };

    dana::segment_outcome run_prompt(dana::view const& in, dana::prompt_descriptor const& desc) {
        dana::result          out;
        dana::segment_outcome outcome;
        try {
            desc.execute_fn(in, out);
            auto clean              = dana::sanitize::sanitize(out.out);
            outcome.out             = std::move(clean.text);
            outcome.redacted        = clean.redacted;
            outcome.no_clipboard    = out.no_clipboard;
            outcome.clear           = out.clear;
            outcome.err             = std::move(out.err);
            outcome.stderr_buffer   = std::move(out.stderr_buffer);
            outcome.published       = std::move(out.published);
            outcome.restore         = std::move(out.restore);
            outcome.restore_append  = out.restore_append;
            outcome.silent          = out.silent;
            outcome.stdout_streamed = out.stdout_streamed;
            outcome.stderr_streamed = out.stderr_streamed;
        } catch (dana::prompt_error const& err) {
            auto clean              = dana::sanitize::sanitize(err.partial_output());
            outcome.out             = std::move(clean.text);
            outcome.redacted        = clean.redacted;
            outcome.no_clipboard    = err.no_clipboard();
            outcome.clear           = out.clear;
            outcome.published       = std::move(out.published);
            outcome.err             = err.what();
            outcome.stderr_buffer   = std::move(out.stderr_buffer);
            outcome.stdout_streamed = out.stdout_streamed;
            outcome.stderr_streamed = out.stderr_streamed;
            outcome.failed          = true;
            outcome.exit_code       = err.exit_code();
        }
        // Piped stdin is spent by the first executed segment, exactly as before:
        // the pipe is shared, and a segment that has already inherited it is the
        // only one that can read it.
        outcome.stdin_consumed = in.in.piped();
        return outcome;
    }

    bool wants_help(std::span<std::string_view const> args) {
        return std::ranges::any_of(args, [](std::string_view arg) {
            return arg == "--help" || arg == "-h";
        });
    }

    bool invocation_wants_help(dana::prompt_invocation const& inv) {
        // `.exec -- COMMAND ...` owns the rest of argv, so a command's own `-h`
        // (e.g. `shutdown -h now`) must not turn into this prompt's help request.
        auto args = std::span<std::string_view const>(inv.args);
        if (inv.name == "exec") {
            if (auto const it = std::ranges::find(args, std::string_view{"--"}); it != args.end()) {
                args = args.first(static_cast<std::size_t>(it - args.begin()));
            }
        }
        return wants_help(args);
    }

    dana::segment_outcome render_help_result(dana::prompt_descriptor const& desc) {
        dana::segment_outcome outcome;
        std::ostringstream    help;
        if (desc.render_help_fn != nullptr) {
            desc.render_help_fn(help);
        }
        auto clean       = dana::sanitize::sanitize(help.str());
        outcome.out      = std::move(clean.text);
        outcome.redacted = clean.redacted;
        return outcome;
    }

    dana::segment_outcome failure_result(std::string message) {
        dana::segment_outcome outcome;
        outcome.err       = std::move(message);
        outcome.failed    = true;
        outcome.exit_code = 1;
        return outcome;
    }

    // The history recorder wants owned strings: argv is a span of C strings, and
    // the store outlives this call.
    std::vector<std::string> argv_strings(std::span<char const* const> argv) {
        std::vector<std::string> out;
        out.reserve(argv.size());
        for (char const* arg : argv) {
            out.emplace_back(arg == nullptr ? "" : arg);
        }
        return out;
    }

    std::filesystem::path current_directory() {
        std::error_code ec;
        auto            path = std::filesystem::current_path(ec);
        return ec ? std::filesystem::path{} : path;
    }

    // What the dispatcher knows about one chain position: its descriptor and the
    // arguments the prompt actually sees (a sink's shared flags are stripped).
    struct segment {
        dana::prompt_descriptor const*    desc = nullptr;
        std::string_view                  name;
        std::vector<std::string_view>     args;
        std::span<std::string_view const> body;
    };

    // A batch containing a file source or a file-filter step must run in argv
    // order: each file prompt publishes the files it emitted (`keys::files`) and a
    // later one skips them, and `.files.exclude`/`.files.scope` publish the filter
    // a later file prompt reads. Other batches keep the parallel path.
    bool any_sequential(std::span<segment const> segs) {
        return std::ranges::any_of(segs, [](segment const& seg) {
            return seg.desc->emits_files || seg.desc->filters_files;
        });
    }

    // `.files.exclude`/`.files.scope` set chain-wide state, so they run before the
    // other segments of their batch regardless of argv position: a filter written
    // after a file prompt still applies to it. Filter steps emit nothing, so the
    // reorder does not change the output; results stay indexed by argv position.
    std::vector<std::size_t> batch_execution_order(std::span<segment const>                 segs,
                                                   std::span<dana::prompt_invocation const> invs) {
        auto const is_filter = [&](std::size_t k) {
            return segs[k].desc->filters_files && !invocation_wants_help(invs[k]);
        };
        std::vector<std::size_t> order;
        order.reserve(segs.size());
        for (std::size_t k = 0; k < segs.size(); ++k) {
            if (is_filter(k)) {
                order.push_back(k);
            }
        }
        for (std::size_t k = 0; k < segs.size(); ++k) {
            if (!is_filter(k)) {
                order.push_back(k);
            }
        }
        return order;
    }

    void write_segment(int fd, dana::segment_outcome const& outcome) {
        std::string const blob   = dana::encode_outcome(outcome);
        std::size_t       offset = 0;
        while (offset < blob.size()) {
            auto const n = ::write(fd, blob.data() + offset, blob.size() - offset);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return;
            }
            if (n == 0) {
                return;
            }
            offset += static_cast<std::size_t>(n);
        }
    }

    // Name how a child ended when it died before writing its blob, so a crash is
    // not reported as a generic "no result".
    std::string child_status_note(int status) {
        if (WIFSIGNALED(status)) {
            return " (terminated by signal " + std::to_string(WTERMSIG(status)) + ")";
        }
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            return " (exit status " + std::to_string(WEXITSTATUS(status)) + ")";
        }
        return {};
    }

    // Run every segment of one batch concurrently: one forked child per segment,
    // each writing its encoded `segment_outcome` back over a pipe. `make_view` runs
    // in the child, so it may only read state that already exists in the parent.
    // `--help` requests are answered in the parent and keep their argv position.
    // Outcomes come back in argv order regardless of which child finishes first.
    std::vector<dana::segment_outcome> run_parallel(std::span<segment const>                         segs,
                                                    std::function<dana::view(segment const&)> const& make_view) {
        std::size_t const                  size = segs.size();
        std::vector<dana::segment_outcome> outcomes(size);
        std::vector<int>                   read_fds(size, -1);
        std::vector<pid_t>                 children(size, -1);

        // Fork every segment before reading anything: a prompt must never fork from
        // a multi-threaded parent, and forking up front keeps each child
        // single-threaded for the `process::command` calls it makes.
        for (std::size_t i = 0; i < size; ++i) {
            if (wants_help(segs[i].args)) {
                outcomes[i] = render_help_result(*segs[i].desc);
                continue;
            }

            std::array<int, 2> fds{};
            if (::pipe2(fds.data(), O_CLOEXEC) != 0) {
                outcomes[i] = failure_result("prompt: pipe failed: " + std::string(std::strerror(errno)) + "\n");
                continue;
            }
            pid_t const pid = ::fork();
            if (pid == 0) {
                ::close(fds[0]);
                auto outcome = run_prompt(make_view(segs[i]), *segs[i].desc);
                write_segment(fds[1], outcome);
                ::close(fds[1]);
                ::_exit(0);
            }
            if (pid < 0) {
                ::close(fds[0]);
                ::close(fds[1]);
                outcomes[i] = failure_result("prompt: fork failed: " + std::string(std::strerror(errno)) + "\n");
                continue;
            }
            ::close(fds[1]);
            read_fds[i] = fds[0];
            children[i] = pid;
        }

        auto const blobs = dana::drain_all(read_fds);
        for (std::size_t i = 0; i < size; ++i) {
            if (children[i] < 0) {
                continue;
            }
            int status = 0;
            ::waitpid(children[i], &status, 0);
            if (auto decoded = dana::decode_outcome(blobs[i])) {
                outcomes[i] = std::move(*decoded);
            } else {
                outcomes[i] = failure_result("prompt " + std::string(segs[i].name) +
                                             ": parallel child produced no result" + child_status_note(status) + "\n");
            }
        }
        return outcomes;
    }

} // namespace

// std::print/println may throw std::format_error or ios_base::failure; the
// format strings are literals and main has no recovery path.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char** argv) {
    // `--no-prompt-files` turns `*.prompt` inlining off for this invocation. It
    // is a dispatcher flag, not a prompt argument: consume it here (the only
    // place it is honoured) so help, completion, chain parsing and history
    // never see it.
    bool expand_prompt_files_flag = true;
    if (argc > 1 && std::string_view(argv[1]) == "--no-prompt-files") {
        expand_prompt_files_flag = false;
        for (int i = 1; i + 1 < argc; ++i) {
            argv[i] = argv[i + 1];
        }
        --argc;
    }

    // Handle --help and --version early (only as first argument)
    if (argc > 1) {
        std::string_view arg(argv[1]);
        if (arg == "--help" || arg == "-h") {
            // The dispatcher's usage text: it ships a fixed prompt set and
            // never scans for prompt files.
            std::print(std::cout, "{}", R"(Usage:
  prompt --help
  prompt list
  prompt list-prompts
  prompt <name> [args...]                # run a single prompt
  prompt <name> [-- arg1 arg2 ...]       # explicit end of prompt flags

Shorthand (chain multiple prompts with args):
  prompt .note "Some details" .cli "pwd" .files src/main.cpp
  prompt .agents .git-dirty .commit

  Tokens starting with '.' or '-' that match a known prompt name are treated
  as prompt invocations. An unmatched '.'/'-' token in the head position is a
  hard error (exit 2), so a typo is never silently swallowed by the auto
  fallback; a path-like token containing '/' is still kept as an argument, and
  after a prompt has matched a '.foo' token is that prompt's argument. Bare
  words between them become that prompt's arguments. The first argument is
  also checked as a prompt name when no prompts have been matched yet.

Prompt files (the same shorthand, kept in a file):
  prompt debug.prompt
  prompt .note "context" debug.prompt .length short

  A token ending in .prompt that names a readable file is replaced, in place,
  by the tokens that file holds (the file is the chain after `prompt`, without
  the word `prompt` itself). Tokens are split on spaces and newlines; a `#` at
  the start of a token comments out the rest of its line; "..." and '...' quote
  (inside "..." only \" and \\ are escapes); nothing else is expanded (no $VAR,
  no globbing, no command substitution). A name is looked up next to the file
  that named it (the current directory at the top level), then
  <git-root>/.prompt/, then $XDG_CONFIG_HOME/prompt/prompts (default
  ~/.config/prompt/prompts). Files may include other files (8 levels deep,
  cycles rejected), a leading `prompt` token is ignored, and every token after
  `--` is copied verbatim. `prompt --no-prompt-files ...` as the first argument
  turns the pass off.

Functions (named, parameterized chains):
  prompt .func triage(task, ai=dseek) .note "{task}" .ai.{ai} .end .triage "tidy it"
  prompt .func hi .note hello .end .hi

  A `.func NAME(params) ... .end` block defines a function and `.name` calls it,
  exactly like a prompt. The signature is one token: `NAME` or
  `NAME(p, p=default)` (no spaces inside the parens). A call fills the declared
  params by position or with `--name value`/`--name=value`; in the body
  `{name}` is a value, `{@}` (as a whole token) the positionals past the
  declared ones, `{0}` the function name and `{N}` the Nth argument. A
  `{placeholder}` that is not a parameter is left alone for `.each`.
  Definitions live in the command, in a `.prompt` file, or in funcs files
  (<git-root>/.prompt/funcs, $XDG_CONFIG_HOME/prompt/funcs; project overrides
  user). A function may not shadow a built-in prompt or sink, and a bad
  definition is an error.

Examples:
  cat file.cpp | prompt cpp-debug
  prompt cpp-debug --flag value
  prompt .note "WIP: fixing parser bug" .files parser.h parser.cpp .cli "make test"
  prompt .agents .files one two three .note "Some details" .cli "pwd"
  prompt .note "review this" .files src/main.cpp .ai.dseek

Notes:
  - Ships a fixed set of built-in prompts; it never scans for prompt scripts.
  - Sinks (.ai.chatgpt/.ai.claude/.ai.dseek/.ai.gap/.ai.gemini/.ai.grok/.ai.kimi/.ai.perplexity) wait
    for the preceding chain, send its text to the site, and continue with the
    answer. They share --with-context, --show-context, --on-error and --head.
  - .stash [NAME] saves the whole chain state (context, facts and flags) and
    .stashed [NAME...] restores it in a later invocation; .stash.list,
    .stash.show and .stash.drop manage the store.
  - 'prompt <name> --complete <prefix> [args...]' prints shell-completion
    candidates for the prompt (used by shell/completions.fish), one
    value<TAB>description per line, without running it.
  - 'prompt --complete-line <current> [args...]' answers for a whole command
    line: the dispatcher decides whether the token is a prompt name, the start
    of the next .name/-name step or an argument of the open prompt, and adds
    the *.prompt files it can inline. shell/completions.fish forwards here.
  - 'prompt list' lists available prompts with the first paragraph of each
    prompt's help as the description (--names for bare names only).
  - 'prompt list-prompts' prints the full help text of every available prompt.
)");
            return 0;
        }
        if (arg == "--version" || arg == "-v") {
            std::println(std::cout, "prompt {}", PROMPT_VERSION);
            return 0;
        }
    }

    // Shell completion. Two forms, both answered here, before the clipboard is
    // initialized and before stdin is read: completion must not consume the
    // pipe, copy anything to the clipboard, or run the prompt.
    //
    //   prompt --complete-line <current> [prior args...]   a whole command line
    //   prompt <name> --complete <prefix> [prior args...]  one prompt's own
    //     (alias: `prompt --complete <name> [prefix] ...`)
    //
    // Both take their candidates from the same place: the prompt's own help for
    // its flags and its optional complete_fn for everything else.
    {
        auto arg = [&](int i) {
            return std::string_view(argv[static_cast<std::size_t>(i)]);
        };

        // Whole-line mode: the shell forwards `commandline -ct` and the tokens
        // typed after `prompt`, and the dispatcher works out whether that token
        // is a prompt name, the start of the next `.`/`-` step, or an argument
        // of the open prompt — adding the `*.prompt` files it can inline.
        if (argc > 1 && arg(1) == "--complete-line") {
            std::string_view const        current = argc > 2 ? arg(2) : std::string_view{};
            std::vector<std::string_view> line;
            for (int i = 3; i < argc; ++i) {
                line.push_back(arg(i));
            }
            std::ostringstream out;
            dana::complete_line(current, line, dana::fs::find_git_root().value_or(std::filesystem::path{}), out);
            std::print(std::cout, "{}", out.str());
            return 0;
        }

        std::string_view              complete_name;
        std::string_view              prefix;
        std::vector<std::string_view> prior;
        bool                          wants_complete = false;
        if (argc > 1 && arg(1) == "--complete") {
            wants_complete = true;
            if (argc > 2) {
                complete_name = arg(2);
            }
            if (argc > 3) {
                prefix = arg(3);
            }
            for (int i = 4; i < argc; ++i) {
                prior.push_back(arg(i));
            }
        } else if (argc > 2) {
            for (int i = 2; i < argc; ++i) {
                if (arg(i) != "--complete") {
                    continue;
                }
                wants_complete = true;
                complete_name  = arg(1);
                if (i + 1 < argc) {
                    prefix = arg(i + 1);
                }
                for (int j = i + 2; j < argc; ++j) {
                    prior.push_back(arg(j));
                }
                break;
            }
        }
        if (wants_complete) {
            // Leading `.`/`-` prefixes are shorthands for the same prompt name.
            while (!complete_name.empty() && (complete_name.front() == '.' || complete_name.front() == '-')) {
                complete_name.remove_prefix(1);
            }
            if (auto const* desc = dana::find_prompt(complete_name); desc != nullptr) {
                std::ostringstream out;
                dana::write_prompt_candidates(*desc, prefix, prior, out);
                std::print(std::cout, "{}", out.str());
            }
            return 0;
        }
    }

    // A `.each` body runs as an inner child: the parent owns the clipboard and
    // the redaction legend, so the child does neither.
    inner_process = std::getenv("PROMPT_INNER") != nullptr;

    // Initialize clipboard
    if (!inner_process) {
        dana::clipboard::init();
    }

    // The process-wide environment: built once, never written again.
    dana::environment env = dana::make_environment();

    // Read stdin if available: anything that is not a terminal counts as a
    // pipe, so an empty pipe still yields (empty) piped stdin.
    dana::stdin_source stdin;
    if (isatty(STDIN_FILENO) == 0) {
        std::string content;
        std::string line;
        while (std::getline(std::cin, line)) {
            content += line;
            content += '\n';
        }
        stdin.load(std::move(content));
    }

    // Prompt files: a `*.prompt` token is replaced, in place, by the tokens the
    // file holds, so chain parsing, sinks, `.each` bodies and the history store
    // below only ever see an ordinary argv (see sdk/prompt_file.hpp).
    std::span<char const* const> argv_span(argv, static_cast<std::size_t>(argc));
    dana::expanded_argv expanded = dana::expand_prompt_files(argv_span, env.git_root, expand_prompt_files_flag);
    if (!expanded.error.empty()) {
        std::println(std::cerr, "{}", expanded.error);
        return 2;
    }

    // Functions: a `.func NAME(params) … .end` block defines a named,
    // parameterized chain template and `.name` calls it (see sdk/function.hpp).
    // They expand to ordinary tokens after prompt files and before chain
    // parsing, so sinks, `.each` bodies and the history store below only ever
    // see a normal argv.
    dana::function_table functions;
    if (std::string function_error; !dana::load_function_files(functions, env.git_root, function_error)) {
        std::println(std::cerr, "{}", function_error);
        return 2;
    }
    expanded = dana::expand_functions(expanded.storage, functions, true);
    if (!expanded.error.empty()) {
        std::println(std::cerr, "{}", expanded.error);
        return 2;
    }
    std::vector<char const*> const argv_pointers = expanded.argv();
    argv_span = std::span<char const* const>(argv_pointers.data(), argv_pointers.size());

    // Parse chained prompts
    auto chain = dana::parse_chain(argv_span, dana::get_all_prompts());
    if (!chain.error.empty()) {
        std::println(std::cerr, "{}", chain.error);
        return 2;
    }

    // If no prompts found, default to "auto"
    if (chain.invocations.empty()) {
        chain.invocations.push_back(dana::prompt_invocation{.name = "auto", .args = {}, .body = {}});
    }

    std::size_t const    count = chain.invocations.size();
    std::vector<segment> segments(count);

    // The dispatcher owns the common sink flags: parse them once, before any
    // batch runs, and hand the sink only the remaining arguments. This also
    // makes --show-context known before the first segment is emitted.
    std::vector<std::optional<dana::sink_args>> sink_args(count);
    bool                                        has_sink     = false;
    bool                                        show_context = false;
    for (std::size_t i = 0; i < count; ++i) {
        auto const& inv  = chain.invocations[i];
        auto const* desc = dana::find_prompt(inv.name);
        if (desc == nullptr) {
            std::println(std::cerr, "prompt: prompt '{}' not found", inv.name);
            return 3;
        }
        segments[i].desc = desc;
        segments[i].name = inv.name;
        segments[i].args = inv.args;
        segments[i].body = inv.body;

        if (desc->role != dana::chain_role::sink) {
            continue;
        }
        has_sink    = true;
        auto parsed = dana::parse_sink_args(inv.args);
        if (!parsed.error.empty()) {
            std::print(std::cerr, "prompt {}: {}", inv.name, parsed.error);
            return 2;
        }
        show_context     = show_context || parsed.options.show_context;
        segments[i].args = parsed.rest;
        sink_args[i]     = std::move(parsed);
    }

    auto const plan = dana::plan_chain(chain, dana::get_all_prompts());
    if (!plan.error.empty()) {
        std::println(std::cerr, "prompt: {}", plan.error);
        return 2;
    }

    // A sink asking for help is a terminal help request, not an AI call with
    // the help text as context.
    for (std::size_t i = 0; i < count; ++i) {
        if (segments[i].desc->role == dana::chain_role::sink && invocation_wants_help(chain.invocations[i])) {
            auto outcome = render_help_result(*segments[i].desc);
            std::print(std::cout, "{}", outcome.out);
            if (!inner_process && !outcome.out.empty()) {
                dana::clipboard::copy(outcome.out);
            }
            return 0;
        }
    }

    // The history store: one append-only record of this invocation. It stays
    // dormant until the first recordable segment, so help requests and the
    // `.history*` family never create a run of their own. Every write is best
    // effort: an unwritable store can never fail a prompt run.
    dana::recorder history{
      dana::recorder::info{
                           .argv     = argv_strings(argv_span),
                           .cwd      = current_directory(),
                           .git_root = env.git_root,
                           .terminal = dana::terminal::current(),
                           .tty      = ::isatty(STDOUT_FILENO) != 0,
                           .inner    = inner_process,
                           .parent =
          [] {
              char const* raw = std::getenv("PROMPT_HISTORY_PARENT");
              return raw == nullptr ? std::string{} : std::string(raw);
          }(),
                           }
    };

    // `.history.purge` protects the run that is being written right now.
    if (history.enabled() && !history.id().empty()) {
        ::setenv("PROMPT_HISTORY_CURRENT", history.id().c_str(), 1);
    }

    auto record_segment =
      [&](std::size_t                  i,
          dana::segment_outcome const& outcome,
          std::string_view             merge,
          std::string_view             status,
          bool                         emitted,
          std::string_view             input = {}) {
          if (!segments[i].desc->record) {
              return;
          }
          auto const& inv = chain.invocations[i];
          history.segment(
            dana::stored_segment{
              .index          = i + 1,
              .name           = std::string(inv.name),
              .role           = segments[i].desc->role == dana::chain_role::sink ? "sink" : "prompt",
              .args           = std::vector<std::string>(inv.args.begin(), inv.args.end()),
              .body           = std::vector<std::string>(inv.body.begin(), inv.body.end()),
              .merge          = std::string(merge),
              .status         = std::string(status),
              .text           = outcome.out,
              .input          = {},
              .emitted        = emitted,
              .redacted       = outcome.redacted,
              .original_bytes = 0,
            },
            input);
      };

    // Cheap declarations run in this (parent) process before any batch forks,
    // so parallel siblings can agree on argv-derived identity. A failing
    // declaration is a hint, not an error: the segment falls back to its own
    // arguments at execute time.
    dana::facts declared;
    for (std::size_t i = 0; i < count; ++i) {
        if (segments[i].desc->declare == nullptr || invocation_wants_help(chain.invocations[i])) {
            continue;
        }
        dana::facts local;
        try {
            dana::declaration declaration{
              .args     = segments[i].args,
              .env      = env,
              .prior    = declared,
              .declared = local,
            };
            segments[i].desc->declare(declaration);
        } catch (dana::prompt_error const&) { // NOLINT(bugprone-empty-catch) — best effort
        }
        declared.merge(local, false);
    }

    auto merge_prior = [&](dana::facts const& artifacts) {
        dana::facts prior = artifacts;
        prior.merge(declared, false);
        return prior;
    };
    auto prior_for_batch = [&] {
        return merge_prior({});
    };

    // One helper builds the read-only view, so preflights, parallel children and
    // sequential segments all see exactly the same shape.
    auto make_view = [&](std::string_view                  name,
                         std::span<std::string_view const> args,
                         std::span<std::string_view const> body,
                         dana::facts const&                prior) {
        return dana::view{
          .name  = name,
          .args  = args,
          .text  = {},
          .body  = body,
          .prior = prior,
          .env   = env,
          .in    = stdin,
        };
    };

    // Descriptor preflights run after chain/sink parsing but before any prompt
    // executes. A sink such as `.save` uses this to create and check its
    // destination directory without doing the expensive work first.
    for (std::size_t i = 0; i < count; ++i) {
        if (segments[i].desc->preflight == nullptr || invocation_wants_help(chain.invocations[i])) {
            continue;
        }

        auto prior = prior_for_batch();
        try {
            segments[i].desc->preflight(
              make_view(chain.invocations[i].name, segments[i].args, segments[i].body, prior));
        } catch (dana::prompt_error const& err) {
            if (!err.partial_output().empty()) {
                std::print(std::cout, "{}", err.partial_output());
            }
            std::print(std::cerr, "{}", err.what());
            return err.exit_code();
        }
    }

    if (!has_sink) {
        std::vector<dana::segment_outcome> outcomes(count);

        // Piped stdin is shared with a spend-once rule, so keep that case
        // sequential. Without piped stdin the segments are independent and run
        // in parallel — unless one of them emits files, which needs argv order.
        bool const parallel = count > 1 && !stdin.available() && !any_sequential(segments);
        if (parallel) {
            auto declarations = prior_for_batch();
            outcomes          = run_parallel(segments, [&](segment const& seg) {
                return make_view(seg.name, seg.args, seg.body, declarations);
            });
        } else {
            // Sequential: each segment sees the facts the finished ones
            // published (last writer wins), exactly like the sink path. Filter
            // steps run first so they apply to the whole batch.
            dana::facts artifacts;
            for (std::size_t const i : batch_execution_order(segments, chain.invocations)) {
                auto const& inv = chain.invocations[i];
                if (invocation_wants_help(inv)) {
                    outcomes[i] = render_help_result(*segments[i].desc);
                    continue;
                }
                auto prior = merge_prior(artifacts);
                outcomes[i] =
                  run_prompt(make_view(inv.name, segments[i].args, segments[i].body, prior), *segments[i].desc);
                if (!outcomes[i].failed) {
                    artifacts.merge(outcomes[i].published, true);
                }
                if (outcomes[i].stdin_consumed) {
                    stdin.spend();
                }
            }
        }

        // Accumulate in command-line order regardless of which parallel
        // segment finished first.
        output_accumulator accumulator;
        int                exit_code = 0;
        bool               redacted  = false;
        for (std::size_t i = 0; i < outcomes.size(); ++i) {
            auto const& outcome = outcomes[i];
            redacted            = redacted || outcome.redacted;
            accumulator.add(outcome.out, outcome.no_clipboard);
            record_segment(i, outcome, "append", outcome.failed ? "failed" : "ok", true);
            if (!outcome.err.empty()) {
                std::print(std::cerr, "{}", outcome.err);
            }
            if (!outcome.stderr_streamed && !outcome.stderr_buffer.empty()) {
                std::print(std::cerr, "{}", outcome.stderr_buffer);
            }
            if (outcome.failed && exit_code == 0) {
                exit_code = outcome.exit_code;
            }
        }

        std::string final_output = std::move(accumulator.text);
        if (redacted && !final_output.empty() && !inner_process) {
            final_output = std::string(dana::sanitize::kRedactionLegend) + "\n" + final_output;
        }
        std::print(std::cout, "{}", final_output);

        std::string clip = std::move(accumulator.clipboard);
        if (redacted && !clip.empty() && !inner_process) {
            clip = std::string(dana::sanitize::kRedactionLegend) + "\n" + clip;
        }
        if (!inner_process && !clip.empty()) {
            dana::clipboard::copy(clip);
        }

        history.finish(exit_code, false);
        return exit_code;
    }

    // Sink chain: ordinary prompts run in batches; a sink is a barrier whose
    // answer becomes the context later prompts and sinks see.
    dana::chain_state state;
    bool              stdin_pending = stdin.piped();
    int               exit_code     = 0;
    output_emitter    emitter;

    auto report_result = [&](dana::segment_outcome const& outcome) {
        if (!outcome.err.empty()) {
            std::print(std::cerr, "{}", outcome.err);
        }
        if (!outcome.stderr_streamed && !outcome.stderr_buffer.empty()) {
            std::print(std::cerr, "{}", outcome.stderr_buffer);
        }
    };

    auto batch_view =
      [&](segment const& seg, std::string_view text, dana::facts const& prior, dana::stream_config stream) {
          return dana::view{
            .name   = seg.name,
            .args   = seg.args,
            .text   = text,
            .body   = seg.body,
            .prior  = prior,
            .env    = env,
            .in     = stdin,
            .stream = stream,
          };
      };

    auto run_normal_batch = [&](std::size_t run_index, dana::chain_run const& run) {
        std::string batch_context = state.text;
        if (state.redacted && !batch_context.empty() && !inner_process) {
            batch_context = prepend_block(dana::sanitize::kRedactionLegend, batch_context);
        }
        auto declarations = merge_prior(state.known);

        std::size_t const                  size = run.end - run.begin;
        std::vector<dana::segment_outcome> batch(size);
        auto const                         run_segments = std::span<segment const>(segments).subspan(run.begin, size);
        bool const                         parallel = size > 1 && !stdin.available() && !any_sequential(run_segments);
        if (parallel) {
            batch = run_parallel(run_segments, [&](segment const& seg) {
                return batch_view(seg, batch_context, declarations, {});
            });
        } else {
            // Sequential segments see the facts the finished ones published (last
            // writer wins), so a file prompt can skip what an earlier one emitted.
            // Seed with the facts earlier batches left in `state.known`, so the
            // first segment of this batch still sees them (the parallel path
            // passes `declarations`, which is the same base). Filter steps run
            // first so they apply to the whole batch.
            dana::facts artifacts = state.known;
            for (std::size_t const k : batch_execution_order(
                   run_segments,
                   std::span<dana::prompt_invocation const>(chain.invocations).subspan(run.begin, size)))
            {
                std::size_t const i   = run.begin + k;
                auto const&       inv = chain.invocations[i];
                if (invocation_wants_help(inv)) {
                    batch[k] = render_help_result(*segments[i].desc);
                    continue;
                }
                dana::facts effective = merge_prior(artifacts);
                auto        in        = batch_view(segments[i], batch_context, effective, {});
                batch[k]              = run_prompt(in, *segments[i].desc);
                if (!batch[k].failed) {
                    artifacts.merge(batch[k].published, true);
                }
                if (batch[k].stdin_consumed) {
                    stdin_pending = false;
                    stdin.spend();
                }
            }
        }

        bool const emit_sources = show_context || run_index + 1 == plan.runs.size();
        for (std::size_t k = 0; k < batch.size(); ++k) {
            std::size_t const i       = run.begin + k;
            auto&             outcome = batch[k];
            report_result(outcome);
            if (!outcome.failed) {
                state.known.merge(outcome.published, true);
            }
            if (outcome.failed) {
                state.incomplete = true;
                if (exit_code == 0) {
                    exit_code = outcome.exit_code;
                }
            }
            if (outcome.stdin_consumed) {
                stdin_pending = false;
                stdin.spend();
            }
            append(state, outcome.out, outcome.redacted);
            if (emit_sources) {
                emitter.emit(outcome.out, outcome.no_clipboard, outcome.redacted);
            }
            record_segment(i, outcome, "append", outcome.failed ? "failed" : "ok", emit_sources);
        }
    };

    auto current_input = [&](std::size_t i) {
        std::string input = state.text;
        // A sink that ignores its input (`.stashed`) leaves a pending pipe for
        // a later sink instead of folding it into the context it replaces.
        if (stdin_pending && !segments[i].desc->keeps_stdin) {
            std::string with_stdin = prepend_block(stdin.text(), input);
            // A silent sink is a passthrough tap: it must not spend the pipe.
            // Keep the stdin in the accumulated context so a later sink still
            // receives it (the sink sees its own --head/legend-trimmed copy).
            if (!segments[i].desc->stream_output) {
                state.text = with_stdin;
            }
            input         = std::move(with_stdin);
            stdin_pending = false;
            stdin.spend();
        }
        auto const& opts = sink_args[i]->options;
        if (opts.head_lines) {
            input = std::string(dana::trim_context_lines(input, *opts.head_lines));
        }
        if (state.redacted && !input.empty() && !inner_process) {
            std::string with_legend  = std::string(dana::sanitize::kRedactionLegend);
            with_legend             += "\n\n";
            with_legend             += input;
            input                    = std::move(with_legend);
        }
        return input;
    };

    auto run_sink = [&](std::size_t i, std::string_view input, bool stream) {
        auto declarations = merge_prior(state.known);
        auto in           = batch_view(
          segments[i],
          input,
          declarations,
          dana::stream_config{
            .stdout_writer =
              [](std::string_view chunk) {
                  std::cout.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                  std::cout.flush();
              },
            .stderr_writer =
              [](std::string_view chunk) {
                  std::cerr.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                  std::cerr.flush();
              },
            .stream_stdout = stream,
            .stream_stderr = stream,
          });
        in.chain = &state;
        in.head  = sink_args[i]->options.head_lines;
        return run_prompt(in, *segments[i].desc);
    };

    auto emit_sink = [&](dana::segment_outcome const& outcome) {
        if (outcome.silent) {
            return;
        }
        if (outcome.stdout_streamed) {
            emitter.streamed(outcome.out, outcome.no_clipboard, outcome.redacted);
        } else {
            emitter.emit(outcome.out, outcome.no_clipboard, outcome.redacted);
        }
    };

    auto update_from_sink = [&](std::size_t i, dana::segment_outcome const& outcome, std::string_view& merge) {
        auto const& opts       = sink_args[i]->options;
        bool const  has_output = !dana::text::strip_newlines(outcome.out).empty();
        if (outcome.failed) {
            merge = "abort";
            if (opts.failure == dana::sink_failure::abort) {
                return true;
            }
            merge = "skip";
            if (opts.failure == dana::sink_failure::skip) {
                return false;
            }
            if (has_output) {
                if (opts.with_context) {
                    merge = "append";
                    append(state, outcome.out, outcome.redacted);
                } else {
                    merge      = "replace";
                    state.text = std::string(dana::text::strip_newlines(outcome.out));
                    if (!state.text.empty()) {
                        state.text += '\n';
                    }
                    state.redacted = outcome.redacted;
                }
            } else {
                merge = "skip";
            }
            state.incomplete = true;
            return false;
        }
        // `.stashed` restores a whole chain snapshot: text, facts, redaction
        // flag and failure taint. It replaces the context (or appends to it,
        // when the prompt already built the merged snapshot) without deriving
        // anything from the sink's own output.
        if (outcome.restore) {
            state = *outcome.restore;
            merge = outcome.restore_append ? "append" : "replace";
            return false;
        }
        // A sink can ask for a fresh context (`.clear`, and `.flush` once its
        // file is written): drop everything a later section would inherit — the
        // accumulated text and facts, redaction flag and failure taint. Already
        // emitted output, the clipboard and the exit status are unaffected.
        if (outcome.clear) {
            merge = "clear";
            clear(state);
            return false;
        }
        state.known.merge(outcome.published, true);
        // A silent sink (`.save`) is a tap: it observes the context without
        // producing an answer. Keep both the context and any existing failure
        // taint so later sinks still apply their own --on-error policy.
        if (!has_output) {
            merge = "passthrough";
            return false;
        }
        merge = opts.with_context ? "append" : "replace";
        if (opts.with_context) {
            append(state, outcome.out, outcome.redacted);
        } else {
            state.text = std::string(dana::text::strip_newlines(outcome.out));
            if (!state.text.empty()) {
                state.text += '\n';
            }
            state.redacted = outcome.redacted;
        }
        state.incomplete = false;
        return false;
    };

    // A sink that is never invoked cannot explain itself: when the source batch
    // failed and the chain policy stops it, say which sink did not run — so
    // `.save` (and every other sink) never "does nothing" silently.
    auto note_skipped_sink = [&](std::size_t i, dana::sink_failure policy) {
        std::string_view name = segments[i].desc->name;
        if (policy == dana::sink_failure::abort) {
            std::println(std::cerr,
                         "prompt: .{} not run: an earlier prompt failed; pass --on-error continue to run it anyway",
                         name);
        } else {
            std::println(std::cerr, "prompt: .{} skipped: an earlier prompt failed (--on-error skip)", name);
        }
    };

    bool chain_aborted = false;
    // A sink the policy never ran still needs a record entry; this is the
    // outcome it would have had (empty, complete, no output).
    dana::segment_outcome const skipped_outcome{};
    for (std::size_t run_index = 0; run_index < plan.runs.size() && !chain_aborted; ++run_index) {
        auto const& run = plan.runs[run_index];
        if (!run.sink) {
            run_normal_batch(run_index, run);
            continue;
        }

        for (std::size_t i = run.begin; i < run.end; ++i) {
            auto const& opts = sink_args[i]->options;
            if (state.incomplete) {
                if (opts.failure == dana::sink_failure::abort) {
                    note_skipped_sink(i, opts.failure);
                    record_segment(i, skipped_outcome, "skip", "skipped", false);
                    chain_aborted = true;
                    break;
                }
                if (opts.failure == dana::sink_failure::skip) {
                    note_skipped_sink(i, opts.failure);
                    record_segment(i, skipped_outcome, "skip", "skipped", false);
                    continue;
                }
            }
            std::string const input  = current_input(i);
            bool const        stream = segments[i].desc->stream_output;
            if (stream) {
                emitter.begin_stream();
            }
            auto outcome = run_sink(i, input, stream);
            report_result(outcome);
            emit_sink(outcome);
            if (outcome.failed && exit_code == 0) {
                exit_code = outcome.exit_code;
            }
            std::string_view merge = "replace";
            bool const       abort = update_from_sink(i, outcome, merge);
            record_segment(
              i,
              outcome,
              merge,
              outcome.failed ? "failed" : "ok",
              !outcome.silent && !dana::text::strip_newlines(outcome.out).empty(),
              input);
            if (abort) {
                chain_aborted = true;
                break;
            }
        }
    }

    auto clip = emitter.clipboard();
    if (!inner_process && !clip.empty()) {
        dana::clipboard::copy(clip);
    }
    history.finish(exit_code, state.incomplete);
    return exit_code;
}
