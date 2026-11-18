#pragma once

#include "chain_state.hpp"
#include "facts.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dana {

    // Process-wide, built once at startup and never written again: where the repo
    // is, where this executable lives, and the clipboard/file-picker services.
    struct [[nodiscard]] environment {
        std::filesystem::path git_root;
        // The directory the dispatcher was started in, captured once so every
        // segment agrees (and so a prompt never has to call current_path()).
        std::filesystem::path working_dir;
        std::filesystem::path exe_path;
        std::string (*clipboard_content)()                                           = nullptr;
        std::optional<std::string> (*clipboard_identifier)()                         = nullptr;
        std::vector<std::filesystem::path> (*select_files)()                         = nullptr;
        std::optional<std::filesystem::path> (*resolve_input_file)(std::string_view) = nullptr;
    };

    // The live-terminal writers. A parallel or captured segment leaves the stream
    // flags false, so write_stdout() only accumulates and the dispatcher emits the
    // result in chain order; a streaming sink tees each chunk as it arrives.
    struct [[nodiscard]] stream_config {
        void (*stdout_writer)(std::string_view) = nullptr;
        void (*stderr_writer)(std::string_view) = nullptr;
        bool stream_stdout                      = false;
        bool stream_stderr                      = false;
    };

    // The shared, spend-once stdin. `piped()` says a pipe was attached;
    // `available()` says nobody has read it yet; `text()` is empty once spent.
    struct [[nodiscard]] stdin_source {
        void load(std::string inp_content);

        [[nodiscard]] bool piped() const {
            return m_piped;
        }

        [[nodiscard]] bool spent() const {
            return m_spent;
        }

        [[nodiscard]] bool available() const {
            return m_piped && !m_spent;
        }

        [[nodiscard]] std::string_view text() const {
            return m_spent ? std::string_view{} : m_content;
        }

        void set_text(std::string_view const new_text) {
            m_content = new_text;
        }

        void spend() {
            m_spent = true;
        }

      private:
        std::string m_content;
        bool        m_piped = false;
        bool        m_spent = false;
    };

    // Everything a segment may read. All views point into dispatcher-owned storage
    // that outlives the call; a segment never mutates chain state.
    struct [[nodiscard]] view {
        std::string_view                  name;
        std::span<std::string_view const> args;
        // The accumulated chain text before this segment (the sink barrier).
        std::string_view text;
        // The verbatim tokens of a body-taking prompt (`.each`), empty otherwise.
        // NOLINTNEXTLINE(readability-redundant-member-init) — keep `view{...}` aggregates warning-free.
        std::span<std::string_view const> body{};
        // Declarations for this batch plus the artifacts of completed segments.
        facts const&       prior;
        environment const& env;
        stdin_source&      in;
        // Default-constructed unless the dispatcher is streaming this segment, so a
        // segment only reads it and an in-process child inherits the defaults.
        stream_config stream{};
        // The live chain state (sinks only; null for ordinary prompts and in the
        // no-sink path). Read-only: a prompt never mutates it. `.stash` reads the
        // raw text and artifacts from here, without the redaction legend the sink
        // input carries.
        chain_state const* chain        = nullptr;
        // The shared sink `--head` value, when one was given (sinks only). The
        // dispatcher has already applied it to `text`; a sink that reads `chain`
        // applies it itself.
        std::optional<std::size_t> head = std::nullopt;
    };

    // Everything a segment may produce. The dispatcher sanitizes `out`, merges
    // `published`, prints `err`/`stderr_buffer`, and applies the flags.
    struct [[nodiscard]] result {
        std::string out;
        std::string err;
        std::string stderr_buffer;
        facts       published;
        bool        no_clipboard = false;
        // Sink-only: drop the accumulated context after this sink succeeds
        // (`.clear`, and `.flush` once its file is written).
        bool clear               = false;
        // Sink-only: replace the whole chain state (text, facts, redaction flag and
        // failure taint) with this snapshot. `.stashed` uses it to continue a saved
        // context. When `restore_append` is set the history merge is recorded as an
        // append (the snapshot already contains the appended text).
        std::optional<chain_state> restore;
        bool                       restore_append = false;
        // Sink-only: keep `out` for the chain context and the history record but do
        // not print or copy it (`.stashed --quiet`).
        bool silent                               = false;
        bool stdout_streamed                      = false;
        bool stderr_streamed                      = false;
    };

    // The cheap, argv/env-derived pre-pass: run in the dispatcher (the parent)
    // before a batch forks, so parallel siblings agree on one identity. A hook must
    // not print, must not touch the network, and must treat `args`/`environment`
    // as its only inputs.
    struct [[nodiscard]] declaration {
        std::span<std::string_view const> args;
        environment const&                env;
        facts const&                      prior;
        facts&                            declared;
    };

    // The dispatcher-injected services, as free functions taking the environment.
    [[nodiscard]] std::string                          clipboard_content(environment const& env);
    [[nodiscard]] std::optional<std::string>           clipboard_identifier(environment const& env);
    [[nodiscard]] std::vector<std::filesystem::path>   select_files(environment const& env);
    [[nodiscard]] std::optional<std::filesystem::path> resolve_input_file(
      environment const& env,
      std::string_view   wanted);

    // Run another registered prompt in-process with a fresh result. The child sees
    // the parent's environment and facts (plus `extra_facts`); its own stdin is the
    // optional `stdin_text`. `inherit_file_scope` is false for a tool call, which
    // must read a file even when the chain already embedded it (the `keys::files`
    // dedup record is dropped). Errors propagate to the caller as usual.
    result run_sub_prompt(
      view const&                       parent,
      std::string_view                  name,
      std::span<std::string_view const> args,
      std::optional<std::string_view>   stdin_text         = std::nullopt,
      facts const*                      extra_facts        = nullptr,
      bool                              inherit_file_scope = true);

} // namespace dana
