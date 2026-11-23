---
name: History Store
description: The dana history store (runtime/history, .history family, .last, .history.find, .history.purge) — file layout, deltas/replay, API, prompts, child runs and env knobs.
---

# History store (`runtime/history.{hpp,cpp}`, `prompts/history*.cpp`)

Companion skills: `chain-contract`, `sinks`, `runtime-helpers`.

The dispatcher records **every invocation** under
`${PROMPT_HISTORY_DIR:-${XDG_STATE_HOME:-~/.local/state}/prompt/runs/}`, so an
answer (e.g. from `prompt .verify .ai.dseek`) can be retrieved later. `main.cpp`
owns the lifecycle: it builds a `recorder` (with the effective argv,
cwd, git root, tty flag and the terminal identity from `terminal::current()`),
sets `PROMPT_HISTORY_CURRENT` to the run id, calls
`record_segment(...)` for each segment (in the no-sink accumulator loop and in
`run_normal_batch`, and for skipped sinks and `update_from_sink` merges) and
finishes with `history.finish(exit_code, incomplete)`. Recording is best-effort:
a write failure is swallowed and can never fail a prompt run.

- **One file per run**, append-only Markdown named
  `<YYYYMMDD-HHMMSS-t<mm>-<xxxx>>.md` (the id is sortable to the millisecond and
  deliberately never matches the sanitizer's phone-number pattern). The body is
  framed by `<!-- history:run {json} -->`, one `<!-- history:segment {json} -->`
  per segment and a closing `<!-- history:end {json} -->` (the run marker also
  stores the terminal identity); each marker carries an
  exact `bytes` (and `input_bytes`) count, so a segment body that contains a
  look-alike marker can never corrupt the parse (a truncated file loads
  partially and is marked `incomplete`).
- **Deltas, not the whole context.** A segment stores its own output
  (`text`), not the growing chain; `merge` records how it folded
  (`append`/`replace`/`skip`/`clear`/`abort`/`failed`). A sink additionally stores
  `input` only when the context it received differs from the one replayed from
  the recorded merges — `barrier_input(run, index)` returns the stored
  input when present, otherwise replays the merges exactly as
  `chain_state::append` does.
- **Replay.** `final_output(run)` reproduces what the dispatcher printed:
  the emitted segments joined by the one text-fold rule (`text::append_block`, the
  same rule `chain_state::append`, `output_accumulator` and the clipboard use) —
  trim both sides, one blank line between blocks, one trailing newline — with the
  redaction legend first when any emitted segment was redacted. `.history` prints
  exactly that, so `prompt .history` byte-matches the original stdout.
- **API.** `run_files`/`run_ids` (newest first), `load`/`load_all`,
  `find_by_id` (an id or prefix) and `find_by_rank` (1 = newest), the
  terminal-scoped `in_terminal`/`find_last_in`/`find_rank_in` (what `.last` and
  `--here` use; only the run marker is read while scanning), `final_output`,
  `barrier_input`, `parse_duration`/
  `parse_size`/`human_bytes`, `keep_runs`/`keep_bytes`/`auto_prune`, `prune`
  (oldest first, never the run passed as protected), and `display_path` (a path
  relative to the cwd when possible, so `$HOME` redaction does not corrupt a
  printed path).
- **Prompts.** `prompts/history.cpp` registers `.history` (default emitted
  text; `--segment`, `--input`, `--all`, `--meta`, `--head`, `--here`; publishes
  `keys::save_stem`, the run's command-line slug such as `tse-foolad` or
  `note-one`, falling back to the run id), `.last`/`.history.last` (the
  terminal-scoped newest run, `--any` for the global view; the shared
  `resolve_run` reads a digits-only key as a rank when `1..count` and otherwise
  as an id prefix), `.last.ai`/`.history.ai` (the newest run **in this terminal
  that has an emitted AI-sink segment**, skipping empty and non-AI runs; an
  optional `--provider NAME` or a bare provider restricts the site, `--any`
  forces the global view, `--head` trims; it publishes `<run-id>-ai` as the save
  stem), `.history.list` (`--limit`, `--since`, `--paths`, `--json`, `--head`,
  `--here`, and `--all`/`--empty`: by default it skips runs that emitted nothing
  and previews a run with the first line of its last emitted segment) and
  `.history.dir`;
  `prompts/history_find.cpp` registers `.history.find` (QUERY, `--regex`,
  `--case-sensitive`, `--name`, `--since`, `--limit`, `--context`, `--paths`,
  `--json`, `--here`; exit 1 via `soft_error` on no match);
  `prompts/history_purge.cpp` registers `.history.purge` (`--older-than`,
  `--keep`, `--max-size`, `--all`, `--dry-run`; protects `--keep` newest runs and
  `PROMPT_HISTORY_CURRENT`; `no_clipboard`). All register with `record = false`.
- **Child runs.** An `.each` item (and a `.exec` child `prompt` chain) records its
  own run; `each.cpp`/`exec.cpp` pass `PROMPT_HISTORY_PARENT=<current run id>` so
  the child is tagged `parent`. `PROMPT_HISTORY_INNER=0` silences children.
- **Knobs.** `PROMPT_HISTORY` (0/off disables recording), `PROMPT_HISTORY_DIR`,
  `PROMPT_HISTORY_MAX_BYTES` (4 MiB per file, over-long writes are truncated),
  `PROMPT_HISTORY_KEEP` (1000), `PROMPT_HISTORY_SIZE` (256M),
  `PROMPT_HISTORY_INNER`, `PROMPT_HISTORY_PRUNE` (0 = no automatic pass),
  `PROMPT_HISTORY_CURRENT`, plus the terminal identity overrides
  `PROMPT_TERMINAL_SID`/`PROMPT_TERMINAL_TTY`/`PROMPT_TERMINAL_KEY` (see
  `runtime-helpers`).
- **Tests.** `tests/unit/test_history.cpp` (marker framing, a fake marker inside a
  body, truncation, replay, ordering/ranks, the terminal identity and scoping,
  prune) and
  `tests/integration/test_history.sh` (end-to-end: `.history` == stdout for plain and sink
  chains, list/find/purge, `.last`/`--here`/`--any` and the fallback note, the
  env knobs, inner-run tagging, completion). The
  tests `ENVIRONMENT` every ctest in `tests/CMakeLists.txt` onto a scratch
  `PROMPT_HISTORY_DIR` so the suite never touches the real store.
