---
name: Tool Calling
description: The dana typed tool-schema system and tool calling — schema declarations, from_help fallback, schema::parse, .tools catalog, .tools.run, inline DSML markup, the toolcall extractor, and .skill.gen.
---

# Tool schemas and tool calling (`runtime/schema.*`, `runtime/tool_call.*`, `.tools`, `.tools.run`)

Companion skills: `agentic-chains`, `agent-loop`, `completion`, `runtime-helpers`.

A prompt can describe itself as a typed *tool* — the shape an agent reads before
composing a chain — and, when it does, it is also **callable**: a model's JSON
tool call is parsed into that prompt's argv and run. There is one registry, not
two: the declaration is `prompt_descriptor::input` (`schema::tool const*`), set
by the trailing defaulted argument of `register_prompt`/`register_sink`.

- **One declaration.** A `tool` is a name, a one-line summary, typed
  `parameter`s (name, description, `type` boolean/string/integer, `takes_value`,
  `repeatable`, a value placeholder, optional `enum_values`, an optional short
  `alias`) and required/variadic `operand`s. Build it with the factories —
  `bool_flag`, `str_flag`, `int_flag`, `enum_flag`, `multi_flag`, `argument`
  `(name, description, required, variadic)` and
  `make_tool(name, summary, parameters, operands)` — which set the fields
  explicitly and so keep aggregate/designated-initialization warnings quiet.
  Declare it as a file-local `schema::tool const k_<name>_tool = …;` (a
  `bugprone-throwing-static-initialization` NOLINT on the definition) and pass
  `&k_<name>_tool` as the last argument of the prompt's own `register_prompt`:
  `register_prompt(..., takes_body, record, &k_<name>_tool)`. The descriptor
  keeps the pointer, so the declaration must outlive registration (a namespace-
  or file-local static does).
- **A schema is optional — but only a declared one is executable.** A prompt
  with no declaration is described by `from_help(name, summary, help)`, a
  best-effort parse of its own `Options:` block (the flag spellings left of the
  two-space column, `-i,` as the short alias, a trailing `N`/`MODE`/`<file>` as
  the value placeholder and the integer/string basis). That fallback is
  **catalog-only**: `.tools` uses it so every prompt is described, but
  `schema::parse` is never run against it, so a guessed flag can never become
  an execution contract.
- **The execution contract.** `schema::parse(tool, json)` (`runtime/tool_call.cpp`,
  declared in `runtime/tool_call.hpp` so `schema.hpp` stays JSON-free) turns an
  `input` object into argv: flags first (a `true` boolean emits its flag, a
  `false` one is omitted, a repeatable flag takes an array, an enum value is
  checked), then operands in declared order (variadic takes an array; operand
  keys are exposed lower-cased and marked `x-positional`). An unknown key, a
  missing required parameter/operand, a wrong type or a bad enum throws
  `usage_error`. A flag `.tools` advertises must parse here, and an
  unadvertised input is rejected. The parser is deliberately lenient about the
  *shape* a model sends: a scalar is accepted for a variadic operand or a
  repeatable flag (treated as a one-element list), and a one-element array is
  accepted for a scalar operand, so the model is not forced to wrap a single
  file/command.
- **Renderers and completion.** `to_json`/`schema::to_json_value` emits one flat
  JSON-Schema object (`input.properties` keyed by name, each with an `x-flag`,
  operands with `x-positional`, a `required` array); `to_markdown` and
  `render_options` render the human/AI form (`### name`, the summary, an
  `Options:` block); `complete(tool, request, out)` is the schema-derived
  completion callback — flags from every parameter plus an `enum_flag`'s values
  (used by `.glob`/`.grep`, whose `complete_fn` is a thin
  `schema::complete(*find_prompt(name)->input, req, os)`).
- **`.tools [--category …] [--only NAME,…] [--exclude NAME,…] [--compact] [--for human|chat] [--format markdown|json|names]`**
  (`prompts/tools.cpp`) is the catalog face: it walks `get_all_prompts()`,
  keeps each prompt's declared schema or derives one with `from_help`, filters
  by `prompt_category` (derived from the name by `category_for`; `core`, `ai`,
  `history`, `catalog`, `code`, `git`, `tse`, `other`), orders the prompts with
  `prompt_descriptor_less` and prints markdown (default), one
  `name [category] summary` line per prompt (`--compact`), a JSON array
  (`--format json`) or the bare names. `--for chat` emits the calling protocol
  plus the JSON catalog of the **callable** tools only (those with a declared
  schema), dumped on one compact line (no indentation — it is machine-facing;
  a human `--format json` preview stays pretty-printed), and the protocol tells
  the model to batch every independent call into
  one answer (one block per call) so a chat turn does not cost a round trip per
  call. `--only`/`--exclude` narrow the catalog by prompt name (the agent loop
  uses them to advertise a short, task-specific list).
- **`.tools.run`** (`prompts/tools_run.cpp`) is the sink that closes the
  loop. It extracts the calls from the accumulated context with
  `tool_call::extract` (a fenced ```json block, a bare JSON document, or a
  top-level array; only objects with a `tool` key count, so ordinary code
  samples are ignored), resolves each name with `to_argv`
  (`find_prompt` + `schema::parse`), asks `policy::engine::require_chain` for
  the whole `prompt <name> …` command — except for the read-only tools
  (`read`, `grep`, `glob`, `files`), which need no approval, so an agent can
  explore freely (a deny or a cancelled ask always aborts,
  whatever `--on-call-error` says), runs it in-process with `run_sub_prompt`
  and appends a `### Tool: <name>` block (`tool_call::format_result`) to the
  context. Every call in the answer runs, in order, so batching several calls
  costs one turn. The execution itself lives in `runtime/tool_exec.*`
  (`tool_exec::run`), which `.tools.run` and the `.ai.loop` agent loop share, so
  the two can never disagree about what a call is or how it runs. `extract` also
  accepts the envelope shapes models actually
  emit: parameters under `input`, `arguments` or `parameters`, a JSON string
  holding the object, or flat beside `tool` (the transcript's
  `{"tool": "cli", "command": …}`). `--print` previews the resolved commands,
  `--list` inventories the
  calls on stderr and keeps the context, `--json` emits
  `{tool, input, ok, exit, output}` records, `--yes` turns policy asks into
  allows, and `--on-call-error abort|continue` (default `continue`) decides
  whether a failed call stops the chain. No calls in the context is not an
  error: the sink notes it on stderr and leaves the context unchanged, so a
  plain answer still flows on. The flow is
  `prompt .note "…" .tools --for chat .ai.dseek .tools.run .ai.reply`.
- **Inline tool markup.** `tool_call::extract` also reads the markup DeepSeek
  (and Anthropic-style prompts) emit as text instead of structured
  `tool_calls`:

  ```text
  <｜126｜ calls>
  <｜126｜ invoke name="cli">
  <｜126｜ parameter name="command" string="false">["git", "status"]</｜126｜ parameter>
  </｜126｜ invoke>
  </｜126｜ calls>
  ```

  `parse_dsml` normalises any `｜…｜` wrapper (and the bare `126`/`DSML` token)
  between `<`/`</` and the tag name, so the V4.1 spaced form, the V4
  `<126tool_calls>` form and a plain `<invoke>` all parse. A parameter with
  `string="true"` is raw text; every other value is JSON (with a repair for the
  fused `name="x": value}` shape a model occasionally emits), so an array
  parameter reaches a variadic operand like `cli`'s `COMMAND` intact. The
  parser requires a wrapper or at least one `<parameter`, so prose that merely
  shows an `<invoke>` example is not a call. It runs after the bare-JSON
  attempt and before fenced-block scanning, so `.tools.run`, `.ai.loop` and
  the `toolcall` extractor all accept the markup unchanged.
- **The `toolcall` extractor** (`extract/toolcall.cpp`) exposes the same
  calls to `.extract toolcall`/`.each toolcall … .end` (one item per call, its
  text the `input` object), so the extractor grammar and the sink never
  disagree about what a call is.
- **The first opencode ports.** `prompt glob` (`prompts/glob.cpp`) lists
  files matching a glob (`rg --files -g PATTERN`, `find` fallback; `--path DIR`,
  `--head N`), `prompt grep` (`prompts/grep.cpp`) searches file contents
  (`rg -n` with `--include`/`-i`/`--max`, `grep -rnI` fallback; `PATTERN [PATH]`,
  `--head N`), `prompt read` reads a file/directory with numbered lines, and
  `files`, `cli`, `run`, `edit` and `write` declare their own schemas too. Each
  derives its `Options:` help and completion from the declaration. `edit` takes
  the `path` operand plus `old`/`new`/`old-file`/`new-file`/`all`/`diff`/
  `dry-run`/`head`; `write` takes the `file` operand plus `--content TEXT`,
  which writes exactly that text instead of the chain context — necessary
  because a tool call runs in a child prompt whose context starts empty.
  Tests: `tests/unit/test_schema.cpp` (factories, renderers, `from_help`, schema
  completion) and `tests/unit/test_tool_call.cpp` (`parse`, `extract`,
  formatting, argv conversion).

**`.skill.gen`** (`prompts/skill_gen.cpp`) builds on the same save-hint
machinery as `.delegate`: it turns a task (an argument or stdin) into a request
for a `SKILL.md`, appends the loader's format and an output contract, publishes
`keys::expect = code:markdown` (plus `save.stem=field:filename` when a later
`.save` wants a name) and sets the `keys::save_kind = "skill"` fact. A nameless
`.save` that sees that fact writes `$XDG_DATA_HOME/agents/skills/<stem>/SKILL.md`
(the `agents` skills layout) instead of the working directory; an explicit
`FILE`/`DIR` still wins. Flow: `prompt .skill.gen "<task>" .ai.dseek .extract .save`.
