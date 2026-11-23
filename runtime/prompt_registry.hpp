#pragma once

#include "completion.hpp"
#include "errors.hpp"
#include "schema.hpp"
#include "sink_types.hpp"
#include "stage.hpp"

#include <cstdint>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dana {

    // The catalog grouping `.tools` filters on. It is metadata on the prompt, so it
    // lives on the descriptor; `category_for` derives it from the name at
    // registration time, and a prompt that wants an explicit grouping is one line
    // in that lookup.
    enum struct [[nodiscard]] prompt_category : std::uint8_t {
        core,    // chaining primitives and context builders
        ai,      // AI sinks
        history, // browsing the store
        catalog, // list, list-prompts, tools, auto
        code,    // fix, review, refactor, run, ...
        git,     // commit, repo, verify, git.*
        tse,     // market data
        other,   // everything else
    };

    [[nodiscard]] std::string_view to_string(prompt_category value);
    prompt_category                category_for(std::string_view name);

    // One execute signature for every segment, sinks included: a sink is a role in
    // the descriptor, not a different function type. The dispatcher owns the
    // barrier, the shared flags and the scheduling.
    using exec_fn      = void (*)(view const&, result&);
    using preflight_fn = void (*)(view const&);
    using declare_fn   = void (*)(declaration&);

    struct [[nodiscard]] prompt_descriptor {
        std::string_view name;
        std::string_view help_summary;
        exec_fn          execute_fn;
        void (*render_help_fn)(std::ostream&);
        // Optional positional/value completion (`--complete`), in addition to the
        // flags the dispatcher derives from render_help_fn. nullptr = flags only.
        completion_fn complete_fn;
        // Sinks run at a barrier and receive the chain text accumulated before
        // them. Their common flags live in sdk/sink.hpp; the dispatcher owns them.
        chain_role role              = chain_role::normal;
        // A silent/passthrough sink (`.save`) never writes stdout and does not
        // spend the stdin pipe: the dispatcher must not open a streamed-output
        // separator for it and must keep piped stdin in the accumulated context.
        bool stream_output           = true;
        // A sink that ignores the incoming context (`.stashed`) must not spend a
        // pending stdin pipe: the dispatcher leaves it for a later sink instead of
        // folding it into the context this sink is about to replace.
        bool keeps_stdin             = false;
        // True for a prompt that embeds file content. The dispatcher runs a batch
        // containing one sequentially, so a later file prompt sees the earlier
        // `keys::files` artifact and skips what was already emitted (see
        // sdk/file_scope.hpp).
        bool emits_files             = false;
        // True for a prompt that changes the chain-wide file selection filter
        // (`.files.exclude`, `.files.scope`). A batch containing one runs in argv
        // order too, so the later file prompts in the same batch see the fact.
        bool filters_files           = false;
        // Optional validation/early-setup hook. The dispatcher runs it after chain
        // and sink parsing but before any prompt executes, so a sink such as
        // `.save` can create and check its destination directory without doing the
        // expensive work first. It throws prompt_error on failure.
        preflight_fn preflight       = nullptr;
        // Optional cheap pre-pass. It runs in the dispatcher before a batch forks,
        // so parallel siblings can agree on argv-derived identity (one tse.resolve
        // per chain, for example). It must not print or touch the network.
        declare_fn declare           = nullptr;
        // True for a block opener (`.each`): its invocation owns the raw tokens up
        // to the matching `.end`, which `parse_chain` stores in body instead of
        // parsing them at this level.
        bool takes_body              = false;
        // False for the `.history*` family: browsing the store must not append to
        // it, or `prompt .history` would make itself the newest run.
        bool record                  = true;
        // The prompt's optional typed input declaration. A prompt that declares one
        // is a *callable tool*: `.tools --for chat` advertises it and `.tools.run`
        // can execute a model's JSON call against it. nullptr means catalog-only
        // (`from_help` describes it, but it cannot be called).
        schema::tool const* input    = nullptr;
        prompt_category     category = prompt_category::other;
    };

    std::span<prompt_descriptor const> get_all_prompts();

    prompt_descriptor const* find_prompt(std::string_view name);

    // Locale-aware name ordering (LC_COLLATE from the environment), shared by
    // list/list-prompts/delegate so the catalog has one order.
    bool prompt_descriptor_less(prompt_descriptor const* a, prompt_descriptor const* b) noexcept;

    std::vector<std::string_view> list_prompt_names();

    // The fixed registry capacity. A build that registers more prompts than this
    // fails loudly (see register_prompt) instead of silently dropping one.
    inline constexpr std::size_t prompt_registry_capacity = 250;

    // Every prompt registers itself: each prompts/<name>.cpp ends with one
    // call like
    //
    //     [[maybe_unused]] bool const registered = dana::register_prompt(
    //         "api", "Review this API design.", &execute_api, &render_help_api);
    //
    // which runs at static-init time, before main. Nothing centralises the list,
    // so adding a prompt means editing exactly one file. A registration only fails
    // when the catalog outgrows prompt_registry_capacity, and it aborts rather than
    // returning false, so a silently missing prompt is impossible.
    bool register_prompt(
      std::string_view name,
      std::string_view help_summary,
      exec_fn          execute_fn,
      void (*render_help_fn)(std::ostream&),
      completion_fn       complete_fn   = nullptr,
      chain_role          role          = chain_role::normal,
      bool                stream_output = true,
      preflight_fn        preflight     = nullptr,
      declare_fn          declare       = nullptr,
      bool                takes_body    = false,
      bool                record        = true,
      schema::tool const* input         = nullptr,
      bool                keeps_stdin   = false,
      bool                emits_files   = false,
      bool                filters_files = false) noexcept;

    // Register an ordinary prompt that embeds file content, so the dispatcher runs
    // a batch containing it sequentially and later file prompts can skip what it
    // already emitted (`keys::files`).
    inline bool register_file_prompt(
      std::string_view name,
      std::string_view help_summary,
      exec_fn          execute_fn,
      void (*render_help_fn)(std::ostream&),
      completion_fn       complete_fn = nullptr,
      preflight_fn        preflight   = nullptr,
      declare_fn          declare     = nullptr,
      bool                takes_body  = false,
      bool                record      = true,
      schema::tool const* input       = nullptr) noexcept {
        return register_prompt(
          name,
          help_summary,
          execute_fn,
          render_help_fn,
          complete_fn,
          chain_role::normal,
          true,
          preflight,
          declare,
          takes_body,
          record,
          input,
          false,
          true);
    }

    // Register a prompt that publishes the chain-wide file selection filter
    // (`.files.exclude`, `.files.scope`). Like a file prompt it serializes a batch,
    // so a `.files`/`.read` after it in the same batch sees the new fact.
    inline bool register_file_filter(
      std::string_view name,
      std::string_view help_summary,
      exec_fn const    execute_fn,
      void (*render_help_fn)(std::ostream&),
      completion_fn       complete_fn = nullptr,
      preflight_fn        preflight   = nullptr,
      declare_fn          declare     = nullptr,
      bool                takes_body  = false,
      bool                record      = true,
      schema::tool const* input       = nullptr) noexcept {
        return register_prompt(
          name,
          help_summary,
          execute_fn,
          render_help_fn,
          complete_fn,
          chain_role::normal,
          true,
          preflight,
          declare,
          takes_body,
          record,
          input,
          false,
          false,
          true);
    }

    // Register a sink: the same registry entry with role=sink. Sink flags
    // (--on-error/--with-context/--show-context/--head) are handled by the
    // dispatcher through runtime/sink.hpp, so adding a sink only needs its own
    // executor, help renderer, and stream flag.
    bool register_sink(
      std::string_view name,
      std::string_view help_summary,
      exec_fn          execute_fn,
      void (*render_help_fn)(std::ostream&),
      completion_fn       complete_fn   = nullptr,
      bool                stream_output = true,
      preflight_fn        preflight     = nullptr,
      declare_fn          declare       = nullptr,
      bool                takes_body    = false,
      bool                record        = true,
      schema::tool const* input         = nullptr,
      bool                keeps_stdin   = false) noexcept;

    // Captures a prompt's own `render_help_*` output into a string, for prompts
    // that append it to a failure message (cli, man, tse.analyze, tse.intraday)
    // instead of printing it directly (`help_text(render_help_cli)` instead of the
    // ostringstream dance).
    template <typename RenderFn>
    std::string help_text(RenderFn render) {
        std::ostringstream os;
        render(os);
        return os.str();
    }

} // namespace dana
