---
name: Agent Loop and Path Aliasing
description: The dana .ai.loop agentic session (driver, session setup, tool execution, first message, budgets, output) and the path-aliasing layer (.paths.hide/.paths.reveal, runtime/alias) used by AI sinks.
---

# Agentic loop (`.ai.loop`, `.ai.<provider>.loop`)

Companion skills: `tool-calling`, `sinks`, `agentic-chains`.

`.ai.loop` is the sink that turns one AI turn into a whole working session: it
sends the task, runs the tool calls in the answer, feeds the `### Tool:` results
back and repeats until the model answers without a call. `.ai.<provider>.loop`
is the same executor with the provider baked into the prompt name
(`.ai.gap.loop`, `.ai.dseek.loop`, …); `.ai.loop [PROVIDER]` takes it as a
leading argument or `--provider`.

- **Three pieces, each independently testable.** `ai/agent_loop.{hpp,cpp}`
  is the pure driver (`run_agent_loop`): everything with a side effect is
  injected (`turn`, `run_tools`, `status`), so it is unit-tested with no
  provider (`tests/unit/test_agent_loop.cpp`). `ai/sink_session.{hpp,cpp}` is the
  setup half the plain `.ai.*` sink and the loop share (`prepare_sink`,
  `open_sink_session`, `make_turn_options`, `save_sink_state`), so provider
  resolution, `--print-prompt`/`--list`/`--print-session`, session loading,
  model resolution, resume state and the terminal-scope save cannot drift apart.
  `runtime/tool_exec.{hpp,cpp}` is the execution half `.tools.run` and the loop
  share (`tool_exec::run`): extract, `to_argv`, policy approval, in-process
  `run_sub_prompt`, `### Tool:` blocks, the JSON report and the per-turn call
  cap.
- **The first message** (`build_agent_message`) carries the agent preamble, the
  working directory and git root, the `.tools --for chat` catalog (read, grep,
  glob, files, edit, write, cli, run), the project's `AGENTS.md` (opt-in with
  `--agents`) and the chain context under `# Task`. Later turns carry only the
  tool results, so the conversation stays coherent and the task is not re-sent.
- **Path hiding.** By default the real workspace paths are replaced with a
  stable placeholder (`runtime/alias.hpp`): the header shows
  `/prompt-ws-<hash>/…`, the assembled message is hidden, every tool call is
  revealed before it runs and its `### Tool:` result is hidden again, and the
  final answer (or transcript) is revealed. `--real-paths` sends the real paths.
  `--print-prompt` prints the hidden message plus one stderr line mapping the
  placeholder to the real root. The saved chat keeps the hidden form, and the
  stable placeholder keeps a later `.ai.reply` coherent.
- **Approval.** Read-only tools (`read`, `grep`, `glob`, `files`) run freely;
  `edit`, `write`, `cli` and `run` go through `policy::engine` (ask by default,
  `--yes` turns asks into allows but never overrides a deny). A deny or a
  cancelled ask always aborts, whatever `--on-error` says.
- **Budgets.** `--max-turns N` (25), `--max-calls N` per turn (16, excess calls
  are dropped with a note), `--tool-head N` (400) and `--tool-bytes N` (64 KiB)
  truncate each result so one big read cannot poison the context. Reaching
  `--max-turns` while the model is still calling tools is not an error: it is
  reported on stderr and the last answer is kept.
- **Output.** Streamed turns are display-only (the engine's styled frames never
  reach the chain); the final context is the last answer, or the whole loop with
  `--transcript`. The chat is saved through `save_sink_state`, so a later
  `.ai.reply` continues it. `--print-prompt` is the offline dry run (it prints
  the assembled first message before any session is opened). The mapping is
  published as `keys::path_alias`, so a later `.ai.reply` hides its instruction
  and reveals the answer.
- **Tests.** `tests/unit/test_agent_loop.cpp` (turn order, the tool feedback prompt,
  the no-call stop, exhaustion, failure, truncation) and `tests/unit/test_alias.cpp`
  (the mapping itself).

# Path aliasing (`runtime/alias.*`, `.paths.hide`, `.paths.reveal`)

The AI-facing path layer: real absolute paths are replaced with a stable,
non-existent placeholder directory and translated back when the model names a
file. `runtime/alias.{hpp,cpp}` is the reusable mechanism; `.ai.loop` builds a
table for its workspace, and `.paths.hide` publishes one for every later
`.ai.*` sink.

- **The placeholder** is `/prompt-ws-<12 hex>` where the hash is FNV-1a over the
  normalized canonical path (`weakly_canonical`). It is stable per project (a
  resumed `.ai.reply` chat keeps working) and unique per path; a directory that
  happens to exist is re-hashed with a counter salt.
- **The mapping** is root + relative structure: `show_path(/a/b/src/x.cpp)` is
  `/prompt-ws-<hash>/src/x.cpp`. `hide()` replaces the longest matching form
  first and only at path-component boundaries (`/a/b` matches `/a/b` and
  `/a/b/c`, never `/a/bc` or `/a/b.txt`); `reveal()` is the inverse.
- **Sanitized spellings.** The dispatcher's sanitizer rewrites
  `$HOME/Projects/x` to `x`, so `for_environment` also registers that form as a
  hide-only entry. A bare basename only matches when a `/` follows, so prose is
  never rewritten.
- **`workspace_header(cwd, git_root, table)`** collapses to one
  `Working directory (git root): …` line when the two are the same directory.
- **`keys::path_alias`** carries the table as a length-prefixed list of
  (real, shown) pairs (`encode`/`decode`), so any path bytes survive.
- **`.paths.hide [PATH…]`** is a silent sink: it rewrites the context in place
  (nothing printed or copied), maps the working directory and git root
  automatically plus any PATH arguments, and publishes the fact.
  **`.paths.reveal`** is the inverse transform, using the fact an earlier
  `.paths.hide` published. Every `.ai.*` sink reads the fact: it hides the
  context it sends and reveals the answer it gets back (`--print-prompt` keeps
  the hidden form, since that is exactly what would be sent).

Tests: `tests/unit/test_alias.cpp` (boundaries, round-trip, stable/non-existent
placeholders, the header, the codec, the environment mapping).
