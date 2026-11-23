---
name: Compression Engine
description: The deterministic dana context-compression layer (src/compress, .compress, .compress.list, .project, .compress.delegate) — compressors, regions, signals, intent wiring and AI-driven composition.
---

# Compression engine (`src/compress/*`, `.compress`, `.project`)

Companion skills: `chain-contract`, `agentic-chains`, `file-filter`.

The chain's context-compression layer replaces bulky low-signal material (C/C++
sources, Google Test files, CSV dumps, included headers) with a compact
high-signal summary while recording what was elided, so the AI sees the shape of
the code without the bodies. It is deterministic and local: no AI call and no
network. The registry, the fenced-region scanner and the planner live in
`src/compress/compress.{hpp,cpp}`.

- **One file = one compressor.** A compressor is `src/compress/<name>.cpp`
  implementing an `applies_fn` (does this unit claim the region?) and a
  `compress_fn` (a `region const&` and a `signals const&` → one `outcome`), and
  ending with one `compress::register_compressor(compressor{…})`. CMake globs
  `src/compress/*.cpp` (like the extractors), so adding a compressor is a single
  new file. The registry is a fixed array of `compressor_capacity` (128) slots
  and aborts at startup on overflow rather than silently dropping a unit.
- **A region, not a document.** A compressor only ever sees a `region` — the
  body of one fenced block (context mode) or one file (source mode), plus its
  inferred `path`, normalized `lang`, first `line` and `focused` flag — so every
  unit is testable on a plain string (`tests/test_compress.cpp` does exactly
  that, with no fixture file). One compressor wins a region: the highest
  `priority`, registration order breaking a tie.
- **The signals.** `compress::signals` carries the chain's `intent`
  (general/fix/debug/review), the focal sites, the detected `project` and a
  `level` (1–3, raised by the planner when a `--budget` is tight). A compressor
  branches on the intent — `gtest.units` keeps every failed body in fix/debug and
  samples the passing ones by Levenshtein distance to the failures in
  general/review, and returns the file unchanged in fix/debug when no failed
  test matches.
- **The built-in units.** `gtest.units` (priority 20: failed bodies verbatim, a
  similarity sample of the rest), `cpp.symbols` (priority 10: signatures plus doc
  comments and `#include`s, with the project-local include map appended),
  `csv.sample` (priority 10: the header plus a few rows) and `include.map`
  (priority 5: a capped symbol index of the headers a file includes, also usable
  standalone). `cpp_scan.*` is the shared C/C++ structural scanner the C++
  compressors read.
- **Context vs. source mode.** `compress::compress_document` compresses the
  fenced regions of the accumulated Markdown (what `.compress` receives when a
  chain pipes it in, e.g. `prompt .files … | prompt .compress`), rebuilding the
  document from the untouched literal chunks and the compressed fences;
  `compress::compress_files` embeds the named files exactly as `.files` would and
  runs the same engine. A region shorter than `--min-lines` passes through
  unchanged unless `--force`, and a **focal** region (a `keys::focus` match) is
  never compressed. `compress::explain` is the dry run behind `--explain`.
- **Intent wiring.** `.fix`/`.debug` declare `keys::mode` (fix/debug) and
  `keys::focus` (the file arguments); `.run` declares `keys::mode=debug` and
  publishes `keys::focus` from `signals::extract_focus(output)` (the compiler
  error sites and the `[  FAILED  ]` test names). `keys::focus` is
  newline-separated (`signals::parse_focus`/`join_focus`), and `.compress` reads
  both facts from `v.prior`; `--for MODE` overrides the intent.
- **`.compress [FILE…]`** (`src/prompts/compress.cpp`) is an ordinary prompt:
  with FILEs it compresses those files, and with no FILE it compresses the piped
  input (a fence-less source file is wrapped in a fence with a guessed language;
  a document already carrying fences is scanned for regions). A FILE argument is
  resolved the way `.files` resolves one (`v.env.resolve_input_file`: exact path
  → git root → `git ls-files | fzf`), so a loose `history.hpp` finds the file
  anywhere in the tree. Flags: `--for`, `--level`, `--min-lines`, `--budget`,
  `--only`/`--skip`, `--project`, `--force`/`-f` and `--explain`. It
  auto-detects the project (and publishes the `project.*` facts) whenever the
  intent is not `general` or `--project` is given.
- **`.compress.list`** prints the project line and every registered compressor
  with its one-line summary — the catalog face of the subsystem, and the menu an
  AI reads to choose a unit. **`.project`** (`src/prompts/project.cpp`) detects
  the project shape (`project::detect`) and publishes
  `keys::project_language`/`project_build`/`project_build_dir`/`project_package`/
  `project_tests`; `.compress` shares the same (process-cached) detection and
  publishes the same facts. `project::info` is enums rather than strings, so a
  compressor branches on `build == build_system::cmake`.
- **AI-driven compression composes from primitives, not a built-in flag.** There
  is deliberately no `.compress --ai`: a chain that needs an AI to compress what
  no deterministic unit handles uses `.compress --explain` (or `.compress.list`)
  to see the regions and the catalog, then `.ai.*`, `.expect`/`.extract` and
  `.each` to act on them. **`.compress.delegate [FILE…]`** packages that first
  step: it emits the project summary, the `compress::explain` region plan, the
  registered-compressor catalog, the file list and a one-command contract, then
  publishes `keys::expect = command:prompt`, so `… .ai.dseek .extract .exec`
  runs the model's chosen `prompt .compress …` command (the same
  delegate/`.extract`/`.exec` pattern as `.delegate`). FILE arguments resolve
  like `.compress`'s; with no FILE it reads the piped input.

Tests: `tests/test_compress.cpp` (signals, `cpp.symbols`, `csv.sample` including
sniffing and short input, `gtest.units` in general/fix/no-failure modes, the
document engine's round-trip/focal/budget/`--only`/`--skip`, and the catalog)
plus the `compress` ctest.
