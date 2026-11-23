---
name: Agentic Chains
description: The dana extract/act layer (.expect, .extract, .exec, .each), its body parser, contracts and item substitution, execution/permissions, and the shared in-process Markdown extractor engine.
---

# Agentic chain layer (`.expect`, `.extract`, `.exec`, `.each`)

Companion skills: `tool-calling`, `agent-loop`, `prompt-development`.

Four prompts turn a chain into an extract/act loop; the in-process extractor
engine owns the selector grammar and the response contracts, so the native
prompts call it directly rather than shelling out.

- **`.expect [NAME=]SELECTOR…`** asks the engine's contract renderer for every
  entry, appends the combined instruction to the context (the next AI sink sees
  it) and publishes `keys::expect` as a **plan**: one newline-separated
  `[NAME=]SELECTOR` entry per argument, exactly one of them unnamed (the
  primary). A single unnamed selector is the degenerate plan.
- **`.extract [SELECTOR]`** is a sink that replaces the context with the
  selection. No selector means the `keys::expect` plan, then `auto`. Each
  **named** plan entry is run alone and its value published as that fact (so
  `save.stem=field:filename` names the next `.save`), then the primary replaces
  the context. `--list` leaves the context alone and prints candidates on
  stderr; `--json` and `--pick` pass through.
- **`.exec [SELECTOR] [--] COMMAND…`** is a sink that runs one command and
  replaces the context with its output. With no command it extracts one from
  the context (selector → the plan's primary → `auto`); a context that is
  exactly one `prompt …` line is run directly, which is what makes
  `… .extract .exec` work. A child `prompt` chain runs with the normal chain
  semantics.
- **`.each [SELECTOR] [loop flags] BODY .end`** is a sink that loops `BODY` over
  the engine's items (table rows, code blocks, sections, commands), one child
  `prompt` per item, in parallel. A selector omitted here means the plan's
  primary (`agent::primary_selector`).

## Bodies and the parser

`.each` registers with `takes_body=true`
(`register_sink(..., takes_body=true)`), so `parse_chain` keeps the raw body
tokens as `prompt_invocation::body` (a `string_view` span in `view::body`) and
the dispatcher's flag/help parser never sees them. The body starts at the first
dot token; everything before it is `.each`'s own flags and selector. Bodies
nest by counting `.each` openers, and a missing or unexpected `.end` is a parse
error (exit 2). The body is a normal chain, never a quoted string; item data is
substituted into its tokens before the child runs.

`.exec -- COMMAND…` is the other parser-level rule: `--` gives the sink the
rest of argv so a child `prompt` chain (`.exec -- prompt .tse …`) is not split
into segments of the outer chain. A `prompt` command without `--` is
diagnosed. Direct program arguments are used as argv verbatim; one token that
contains whitespace is treated as a command line and split with quotes and
backslashes only. Compound shell syntax (`| & ; < > \`` and `$(`) is refused.

## Contracts and item substitution

The shared grammar is `kind[:qualifier][@N]` — `table[:col1,col2]`,
`code[:lang]`, `section[:heading]`, `command[:program]`, `field[:label]`
(a `label: value` line), `frontmatter`, `any`, `auto`, `none`, or a bare
heading. `@N` selects the Nth match. `.delegate` publishes `command:prompt`
itself (plus a `save.stem=field:filename` binding when `.save` has no explicit
target), so its output feeds `.extract`/`.exec` without an explicit `.expect`.

`.each` substitutes `{field}` (any table column or item key), `{item}`,
`{body}`, `{index}`, `{n}` and `{file}` (a temporary file holding the item, for
`.files`). Item data only ever reaches argv: a whole-token placeholder whose
value resolves to a prompt name or `.end` is rejected. A `{field}` program is
allowed (it is dynamic, so the child's policy approves the resolved command).

## Execution and permissions

Each `.each` item is a child `prompt` process with `PROMPT_INNER=1`,
`PROMPT_INNER_DEPTH` (parent + 1; loops are capped at depth 2) and
`PROMPT_INNER_ALLOW` (the static-program grants). The parent owns the clipboard
and prints one redaction legend, so an inner process does neither.

`policy::engine` (`sdk/policy.*`) decides commands: built-ins (`tse*` and
`md.extract` allow; `sudo`, `su`, `dd`, `mkfs*`, `shutdown`, `reboot`,
`poweroff`, `halt` deny; everything else ask), then
`$XDG_CONFIG_HOME/prompt/policy` (or `~/.config/prompt/policy`), then
`<git-root>/.prompt/policy`, then `PROMPT_POLICY_ALLOW`/`PROMPT_POLICY_DENY`
and `PROMPT_INNER_ALLOW`/`PROMPT_INNER_YES`. Rules are
`allow|ask|deny PROGRAM [arg-globs…]`; argument patterns are a prefix (`**`
for the rest). More specific rules win, then the higher source; deny wins a full
tie, then allow, then ask. `--yes` turns an ask into an allow but never
overrides a deny. An ask with no `/dev/tty` is a soft failure with the rule
hint. Prompts are trusted code, but a generated `prompt …` chain is displayed
whole and approved through `require_chain`. `.each` pre-approves the static
`.exec` programs in its body once and passes the program names as
`PROMPT_INNER_ALLOW`; a dynamic command asks in the child.

`.each` flags: `-j/--jobs N` (default min(items, 8), capped at 64), `--limit N`,
`--stdin`, `--print`, `--list`, `--json`, `--no-header`, `--keep-files`,
`--on-item-error stop|skip|keep`, `--timeout SEC` (600), `--yes`. `--print`
prints the substituted body commands without running them.

Tests: the `agent_*` ctests (`expect` and its `plan`, `extract`, its `field`
selector and the `test_extract_bind.sh` binding/save round-trip, `exec`
print/allowed/denied/`--` chain/verbatim args, `each` print/run/list) plus the
body and `--` cases in `tests/test_chaining.cpp` and the offline
`md_extract_unit` (`tests/test_md_extract.py`: the `field` selector and the
combined contract).

# Markdown extractor engine (`sdk/extract.*`)

The selector grammar, the response contracts and the extraction itself are one
in-process engine; the four agentic prompts and the standalone `bin/md.extract`
CLI share it, so they can never disagree.

- **One file = one extractor.** An extractor is `src/extract/<name>.cpp`
  implementing a `run(text, qualifier, options)` function and ending with one
  `dana::extract::register_extractor("<name>", "<summary>", &run_<name>);`.
  CMake globs the sources, so no other file changes. The registry is a fixed
  array of `extract_registry_capacity` slots and aborts at startup when it
  overflows (like the prompt registry) rather than silently dropping a kind.
  The registered kinds are `code`, `table`, `section`, `frontmatter`,
  `command`, `field`, `auto`, `json`, `function`, `filename`, plus the `any`
  alias.
- **Grammar** (`src/sdk/extract.cpp`): `entry := [BIND "="] pipeline`,
  `pipeline := step ("/" step)*`, `step := NAME [":" QUALIFIER] ["@" N]`.
  `/` splits only before a registered extractor name (`\/` escapes), so a
  qualifier may contain slashes; a bare word that is not a kind is `section`
  heading sugar (`all` normalises to `any`). `parse_selection` parses one
  selector, `parse_plan`/`format_plan`/`primary` handle the `keys::expect`
  plan, and `is_selector_token` classifies a token (the `.exec`/`.each` flag
  parser inherits it, which is why `json`/`function`/`filename` count as
  selectors).
- **Sharing the document.** `src/extract/document.{hpp,cpp}` parses Markdown
  once into context-carrying `block`s (enclosing heading path plus the prose
  before/after) that every extractor reads; `src/extract/render.*` holds the
  output modes (`render_selection`, `drop_first_line`/`join_blocks`) and the
  `--pick` fzf picker, and `src/extract/common.hpp` the comma-list, language
  family and `label: value` helpers. The four prompts include
  `"../extract/render.hpp"` and call the engine directly — no subprocess.
- **Standalone CLI.** `src/md_extract_main.cpp` is `bin/md.extract`: the
  flag-based interface (`--kind`/`--lang`/`--select`/`--items`/`--contract`/
  `--json`/`-o`, …) that predates the selector language, byte-compatible for
  pipelines (`… | git apply`). It uses `std::regex` for its qualifier/heading/
  grep patterns and is the only caller of `infer_family`/`--guess`; the chain
  itself never guesses. `.expect`/`.extract`/`.each` pass their strings through
  the same parser and renderer, so `md.extract --select X` and `prompt … .extract X`
  select identically.
