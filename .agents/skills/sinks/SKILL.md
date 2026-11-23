---
name: Sinks
description: How dana sinks work as chain barriers — registration, shared flags, streaming, terminal rendering, passthrough/clear/save hints, stash snapshots, path transforms, and the ai.* provider family including network resilience.
---

# Sinks

Companion skills: `chain-contract`, `agent-loop`, `tool-calling`, `history`.

A sink is a chain barrier, not an ordinary prompt. Register it with
`dana::register_sink("<name>", "<summary>", &execute_<name>, &render_help_<name>, nullptr, stream_output)`
(`stream_output=false` marks a silent sink) and no other switches; contiguous
sinks chain in argv order, each answer feeding the next. Adding a sink does not
touch the AI code or the dispatcher: the descriptor role makes `main.cpp` batch
everything before it, hand the sink the accumulated `v.text`, and let its answer
become the context later segments see.

- **Shared flags are dispatcher-owned** (`runtime/sink_types.hpp` + `sink.hpp`):
  `--on-error abort|continue|skip` (default abort), `--with-context`,
  `--show-context` and `--head N`. `parse_sink_args` removes them once in
  `main.cpp`; the sink implementation only sees its own arguments.
- **Reading the barrier:** the sink reads `v.text`; `--head` has already been
  applied to it by the dispatcher — do not trim it again (`trim_context_lines`
  is the shared helper behind it, applied before the redaction legend is
  prepended). A barrier's stderr is printed by the dispatcher and is never part
  of the chain context.
- **Streaming:** use `write_stdout(v, r, chunk)` and `write_stderr(v, r, chunk)`.
  They append to `r.out`/`r.stderr_buffer` and, when the dispatcher enabled
  streaming, call `v.stream.stdout_writer`/`v.stream.stderr_writer`
  immediately. `run_sink_tool(v, r, argv, stdin, timeout)` wires both to
  `process::command`'s `on_stdout`/`on_stderr` tee callbacks.
- **Terminal rendering:** a tool-backed sink's stdout is a pipe, so a tool that
  checks `isatty()` would drop its colour/Markdown. Pass its force-terminal flag
  when `isatty(STDOUT_FILENO)`, keep the styled bytes display-only, and take the
  chain context from a plain side channel (a `--context-file` read back into
  `r.out`), so rich frames never reach the clipboard or the next prompt. Forward
  the terminal size too (`--width`/`--height`, from `TIOCGWINSZ`): the child
  cannot measure a terminal it is not attached to. The native `ai.*` sinks now
  render in-process, but the same split applies to any external tool sink.
- **Help:** call `render_sink_help(out)` for the shared `Options:` block; the
  prompt's own help follows it. Completion derives every flag from that block.
- **Failure:** throw `prompt_error` like any prompt. The dispatcher catches it,
  applies the sink's `--on-error` policy, and keeps a failed source batch tainted
  so each later sink can decide again. A sink that is not run because the source
  batch failed is named on stderr (`prompt: .save not run: an earlier prompt
  failed; pass --on-error continue …`), so a skipped sink is never silent.
  Fan-out sinks are captured and emitted in argv order (no interleaved live
  streams); chained sinks stream as they run.
- **Passthrough, clearing and save hints:** a successful sink that emits no
  stdout is a no-op for the chain: it leaves the accumulated context and any
  failure taint unchanged. Register such a silent sink with `stream_output=false`
  so the dispatcher does not open an output separator for it; piped stdin is
  kept in the accumulated context instead of being spent by that sink. A sink
  that must reset the context sets the output flag `r.clear` (like
  `r.no_clipboard`): once that sink succeeds, the dispatcher drops the
  accumulated text, the filename hint, the redaction flag, the barrier stderr
  and the source-batch taint, while emitted output, the clipboard and the exit
  status stay untouched. `.clear` (`prompts/clear.cpp`) does only that;
  `.flush` (`prompts/save.cpp`) does it after a completed write, so a failed
  save keeps the context. A prompt that knows a good filename publishes the
  basename stem as the `keys::save_stem` fact (`r.published.set(keys::save_stem,
  stem)`), which the dispatcher carries through parallel children to later
  sinks; a prompt with no preferred name simply publishes nothing. `.save`
  (`prompts/save.cpp`) is the reference passthrough sink: it writes the same
  context an AI sink receives, refuses to overwrite unless `--force`, sanitizes
  the hint and appends `.md` to derived names. With no explicit `FILE`/`DIR` its
  `declare_` hook additionally publishes `keys::save_wants_name`, which is what
  makes a preceding `.delegate` ask the AI for a `filename:` (the plan's
  `save.stem=field:filename` binding then names the file); an explicit target
  suppresses the request. `tse`, `tse.codal`,
  `tse.intraday` and `files` provide hints. `.flush` shares `.save`'s parser,
  preflight and writer, with its own command name in messages. `.c.c`
  (`prompts/clipboard.cpp`) is the clipboard passthrough: it copies the
  context to the system clipboard and leaves it unchanged, so it can sit before
  another sink. Its paste counterparts `.clipboard` and `.c.p` are ordinary
  source prompts — a source never receives the chain context, which is exactly
  why copying has to be a sink.
- **Stash snapshots:** `.stash` (`prompts/stash.cpp`) is a silent sink that
  reads `view.chain` (the raw `chain_state`, not the legend-prefixed
  `view.text`) and `view.prior`, and writes one JSON file per name through
  `runtime/stash.{hpp,cpp}`. `.stashed` (`prompts/stashed.cpp`) is the restore
  sink: it sets `result::restore` (a whole `chain_state`) and
  `result::restore_append`; the dispatcher assigns it in `update_from_sink`
  without deriving anything from the sink's output. It registers
  `keeps_stdin=true` (`prompt_descriptor::keeps_stdin`), so a pending pipe is
  left for a later sink instead of being folded into the context it replaces;
  `result::silent` keeps `--quiet` out of stdout/clipboard while the context is
  still restored. `.stash.list`/`.stash.show`/`.stash.drop` are ordinary
  `record=false` prompts.
- **Path transforms:** `.paths.hide` and `.paths.reveal`
  (`prompts/paths_alias.cpp`) are silent sinks that rewrite the context in
  place and emit nothing. `.paths.hide` maps the working directory, the git root
  and any PATH arguments to stable placeholders and publishes
  `keys::path_alias`; `.paths.reveal` restores the real paths from that fact.
  Every `.ai.*` sink reads the fact to hide what it sends and reveal the answer.
  See the `agent-loop` skill.
- `ai.*` (`prompts/ai.cpp`) is the reference family: eight web provider
  registrations plus `.ai.debate`, one shared in-process executor,
  `--print-prompt` printing exactly what would be sent, and `--stdin-only` so
  positional prompt text cannot silently replace the chain. Three more registrations
  (`.ai.lms`, `.ai.openai`, `.ai.openrouter`) reuse that executor for the
  OpenAI-compatible HTTP providers; the `ai_provider` struct carries an `api`
  flag (and `local` for the local server), which switches the help's
  provider-options block — endpoint (`--base-url`/`--api-key`/`--param`) and
  inline attachments instead of a browser session and `--prompt-file` — while
  the sink plumbing is untouched. `--list`/`--models` (and `--list --json`/
  `--list --all`) are handled by the in-process engine for the endpoint's model
  catalogue. **Every model list and friendly-name alias lives in one file,
  `ai/models.{hpp,cpp}`**; each provider's `provider_info` reads it, so
  adding/removing a model is a one-line edit there and both `--list` and the
  `-m`/`--model` completion (one `complete_ai_<provider>` wrapper per sink,
  `--model=` included) pick it up. The API providers (`lms`, `openai`,
  `openrouter`) have empty static lists because they discover their models from
  the endpoint.
  `.ai.debate` runs in-process through the same executor (the providers passed
  as leading arguments, API ones included); the
  relay loop, framing and context-file writing live in
  `ai/debate.{hpp,cpp}`, which reuses
  `ai/cli.{hpp,cpp}`'s turn/renderer/context helpers. `-c`/`--new`
  are forwarded too; the panel state is stored by `ai/state.cpp`
  under `$XDG_STATE_HOME/ai/debate-<providers>.json`. Its completion
  callback (`complete_ai_debate`) offers the provider names (aliases included),
  `PROVIDER=` after `-m`/`--model`, and that provider's models once the
  `PROVIDER=` half is typed (`gap=A-…`).
  `.ai.reply` (`execute_ai_reply`) continues the chat this terminal talked to
  last: it resolves a provider (explicit `PROVIDER`/`--provider`, else the
  `keys::ai_provider` fact an earlier AI sink in the same chain published, else
  the per-terminal marker), forwards every provider option, and appends the
  `-c`/`--reply` state flags itself. `--any` continues the globally newest chat
  instead of this terminal's. Every AI sink publishes `keys::ai_provider` and
  `keys::ai_answer`, so a chained `.ai.reply` drops the answer it would
  otherwise re-send and keeps only the newly appended text. `.ai.loop` and its
  eleven `.ai.<provider>.loop` aliases reuse the same session setup and run
  many turns on one conversation (see the `agent-loop` skill). The state layout
  lives in `ai/state.cpp`: the plain `ai/<provider>.json` (for `-c`) is
  mirrored per terminal under `ai/terminals/<scope>/<provider>.json` with a
  `last` marker (and a global `ai/last`); `<scope>` is
  `dana::terminal::scope` (`runtime/terminal.cpp`), matched byte-for-byte by
  `state::terminal_scope()`. `.ai.debate` mirrors each participant and sets
  `last` to `debate`, so `.ai.reply <provider>` is required to continue one.
- **Network resilience.** `ai/http.cpp` owns the retry policy so every
  provider inherits it: a request is retried on a transient status
  (408/425/429/500/502/503/504) or a transport failure (DNS, connect, TLS,
  reset, stalled transfer), with exponential backoff + jitter, an honoured
  `Retry-After`, and a total retry budget. A **streaming** request may retry
  only while `transfer_state.delivered == 0` — once a line or chunk reached the
  caller, replaying it would duplicate output. Connections also set TCP
  keepalive and a 1-byte/s low-speed window (`apply_resilience`), so a VPN cut
  or a dead socket becomes a real error instead of a hang; `ws.cpp` sets the
  same keepalive on the websocket handshake, and Grok's `send` retries the
  whole turn once when the socket drops before any text was emitted. Knobs:
  `PROMPT_AI_RETRIES` (3), `PROMPT_AI_RETRY_DELAY_MS` (500),
  `PROMPT_AI_RETRY_MAX_DELAY_MS` (8000), `PROMPT_AI_RETRY_BUDGET_MS` (180000),
  `PROMPT_AI_CONNECT_TIMEOUT` (15), `PROMPT_AI_STALL_TIMEOUT` (300),
  `PROMPT_AI_KEEPIDLE` (30), `PROMPT_AI_KEEPINTVL` (15),
  `PROMPT_AI_KEEPCNT` (4). `is_network_failure` classifies the transport
  failures; `is_transient` is the broader gate the providers use for
  turn-level recovery.

Tests: `tests/unit/test_sink.cpp` (flag parsing, streaming/tee, timeout), the
`plan_chain`/`ai.*`/`save` registration and `tse` declaration cases in
`tests/unit/test_chaining.cpp`, `tests/unit/test_facts.cpp` (the fact store and its two
merge rules, the wire codec, `segment_outcome` round-trip), `tests/unit/test_clipboard.cpp`
(the pasted-text → instrument policy), `tests/integration/test_clipboard_prompt.sh`
(end-to-end `.clipboard`/`.c.p` paste and `.c.c` copy through a stubbed xclip),
`tests/integration/test_ai_sink.sh` (end-to-end `--tty`/`--width`/`--height` and
`--context-file` plumbing through stub providers), `tests/integration/test_ai_debate_sink.sh`
(the same plumbing for `.ai.debate`, plus provider/flag forwarding),
`tests/integration/test_save.sh` (end-to-end save targets, passthrough, `--head`,
no-clobber and sanitized names), `tests/integration/test_clear.sh` (context reset between
sections, piped stdin, hint and failure-taint reset, `.flush` == `.save` +
`.clear`, preflight), `tests/unit/test_stash.cpp` (name validation, scope paths, the
JSON round-trip, append merging, shadowing, sanitization, malformed files and
the cross-process append lock) and `tests/integration/test_stash.sh` (end-to-end
`.stash`/`.stashed`, scope shadowing, fact/flag restoration, the management
subcommands and completion); `tests/integration/test_ai_reply.sh` (end-to-end `.ai.reply` through
stub providers: terminal-scoped provider selection, exact `--reply` resumption,
`--any`, explicit providers and the only-new-text rule), `tests/unit/test_ai.cpp`
(the `ai.state` terminal scope, mirroring, `last` markers, expiry, clear and
debate participant mirroring, the provider registry, the HTTP retry policy
against a stub server, and rendering), `tests/unit/test_ai_debate.cpp` exercises
the relay loop offline (turn order, framing, `--rounds`/`--until-agree`, context
selection, failure), `tests/unit/test_ai_pow.cpp` covers the proof-of-work
solver, and `tests/integration/test_ai_native.sh` drives the native `ai.cli`
binary end to end.
