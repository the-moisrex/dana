---
name: SDK Helpers
description: Reference for the dana src/sdk helpers (args, file_scope, file_filter, glob, embed, stage, facts, wire, chain_state, terminal, agent, alias, extract, policy, schema, tool_call, sink, process) so prompts reuse them instead of re-open-coding loops.
---

# SDK helpers (`src/sdk/`)

Companion skills: `chain-contract`, `file-filter`, `sinks`, `agentic-chains`,
`tool-calling`, `agent-loop`, `history`.

Use these instead of re-open-coding the same loops:

- `args.hpp`:
    - `parse_head_option(args, head_lines, strict)` — the raw `--head` validator.
    - `head_lines_or_throw(v, strict = true)` — the parser plus the
      `throw usage_error{...}` preamble, for prompts that only cap output.
    - `file_options_or_throw(v, strict = true)` — the file prompts' whole CLI:
      the validated `--head N` cap plus the value-less `--num`/`--numbered` flag
      and `--no-dedup`, returned as a
      `file_options {head_lines, numbered, dedup}` (built on
      `is_number_flag(arg)`, whose spellings live in `kNumberFlags`, and
      `kNoDedupFlag`).
    - `positional_args(v)` / `collect_file_args(v)` — the FILE arguments with
      the `--head <value>` pair, the `--num`/`--numbered` flags and `--no-dedup`
      removed. `collect_file_args` splits them into `files` (regular files) and
      `missing`; embed the first list and warn about the second. A sibling file
      prompt skips what an earlier one emitted through `keys::files`, not by
      re-embedding it.
    - `file_usage_help(name, tail, pipe, prefix)` and
      `file_options_help(what, show_help)` — the shared usage lines and the
      `--head`/`--num`/`--no-dedup` Options block, so every file prompt's help
      stays identical.
- `file_scope.hpp`: the chain-wide file record behind `keys::files` —
  `file_key(path)` (the canonical absolute key), and `file_scope` with
  `seen(path)`, `mark(path)` and `fact()` (the ordered union to publish). A file
  prompt constructs it from `v.prior`, skips `seen()` files, marks what it emits
  and publishes `fact()`; `--no-dedup` (`opts.dedup == false`) bypasses the skip.
- `file_filter.hpp`: the chain-wide file selection filter behind
  `keys::excluded_files`/`keys::file_root` — `file_filter::from(prior, env)`,
  `empty()`/`has_root()`/`root()`/`exclude()`, `excluded(path)`,
  `resolve(env, arg)`, `candidates()`/`directories()`,
  `rg_globs()`/`grep_excludes()` and `exclude_fact(patterns)`; plus
  `select_files(env, filter)`/`select_directories(env, filter)` and
  `display_path(env, path)`. See the `file-filter` skill.
- `core/glob.hpp`: `dana::glob::match(pattern, text)` (the `*`/`?` matcher
  shared with `core/git.cpp`) and `has_wildcard(pattern)`.
- `embed.hpp`: `infer_lang` (`std::string_view`), `trim_context`,
  `trim_context_nl`, `head_output`, `read_file`, `number_lines(text, numbered)`
  (the `N | line` prefixing `--num` enables), `embed_file`,
  `embed_file_block(file, file_options)`, `warn_missing_file`, `embed_stdin`.
- `stage.hpp`: `environment`, `stream_config`, `stdin_source`, `view`, `result`,
  `declaration`, `run_sub_prompt(parent, name, args, stdin_text)` (runs another
  registered prompt in-process with a fresh result — how `auto`, `verify`, `run`
  and `paths` compose other prompts) and the free services taking an
  `environment`: `clipboard_content(env)`, `clipboard_identifier(env)`,
  `select_files(env)`, `resolve_input_file(env, wanted)`. `environment` is a
  plain data bag built once at startup (`src/environment.cpp`'s
  `make_environment()`, not `main.cpp`); `view` holds references to it, to the
  prior facts and to the stdin source, so it is aggregate-initialised and
  non-assignable (return it by value only when the referents outlive it). The
  "what does this pasted text identify" policy (easytrader/TSETMC/Codal URLs,
  ISIN, insCode, a short Persian symbol) lives beside the clipboard it reads:
  `core/clipboard.hpp`'s `clipboard::detect_identifier()`.
- `facts.hpp`: the fact store and `keys::`; `wire.hpp`: `segment_outcome`,
  `encode_outcome`/`decode_outcome`, `drain_all`; `chain_state.hpp`: the
  dispatcher's accumulated text plus known facts and the redaction/incomplete
  flags.
- `terminal.hpp`: `terminal::id {sid, tty, key}`, `current()` (the
  `PROMPT_TERMINAL_SID`/`PROMPT_TERMINAL_TTY`/`PROMPT_TERMINAL_KEY` overrides
  first, then `getsid(0)` and the controlling tty), `same()` (the identity
  comparison: an opaque key wins when both sides carry one, otherwise the
  session ids must agree and a differing tty rejects a recycled pid),
  `describe()` and `scope()` (the filesystem-safe `key-…` / `sid-…[_tty-…]`
  name; `bin/ai`'s `state.terminal_scope()` keeps the same encoding, and
  `.ai.reply` uses it to find the provider state this terminal wrote). The
  history store is its first user (`.last`, `--here`), but it
  is deliberately history-agnostic so any future per-terminal feature can share
  the same "a place, not a global" identity.
- `agent.hpp`: the agentic layer's shared pieces — `is_selector_token` (a thin
  delegating wrapper over `extract::is_selector_token`),
  `split_command` (quotes/backslashes, compound-syntax refusal), `join_argv`,
  `parse_exec_args` (`.exec`'s own flags plus command/selector split),
  `program_name`, `static_exec_commands` (the `.exec` programs a `.each` body
  will run, nested bodies included) and `validate_exec_template` (a
  malformed `.exec`, before any child runs).
- `alias.hpp`: the path-aliasing table behind `.paths.hide`/`.paths.reveal` and
  the `.ai.loop` workspace — `table` (`add`, `add_hide_only`, `add_path`,
  `hide`, `reveal`, `show_path`, `describe`), `placeholder_for`,
  `workspace_header`, `for_environment` and the `keys::path_alias` codec
  `encode`/`decode`. See the `agent-loop` skill.
- `extract.hpp`: the extractor registry and selector layer shared by the four
  agentic prompts and the CLI — `register_extractor`, `is_extractor_name`/
  `sorted_extractors`, `parse_selection`/`parse_plan`/`format_plan`/`primary`,
  `is_selector_token`, the contract renderer and `extract::run` (plus
  `complete_selector`, the completion callback the prompts register). See the
  `agentic-chains` skill.
- `policy.hpp`: the permission engine — `engine::require(argv)`,
  `require_chain(tokens)`, `set_yes`, the rule sources and the `/dev/tty` ask.
- `schema.hpp`: a prompt's optional typed tool declaration — `tool`, `parameter`,
  `operand`, the `*_flag`/`multi_flag`/`argument`/`make_tool` factories,
  `from_help` (the catalog-only schema from a prompt's own `Options:` block),
  `to_json`/`to_markdown`/`render_options` and `complete` (see the
  `tool-calling` skill). The declaration rides on `prompt_descriptor::input`;
  there is no separate registry.
- `tool_call.hpp`: the JSON face of that declaration — `schema::to_json_value`,
  `schema::parse` (a model's `input` object → argv), `tool_call::extract`
  (finding `{"tool": …}` blocks in an answer), `to_argv` and `format_result`.
  JSON lives here so prompt translation units do not pay for nlohmann.
- `sink.hpp` / `sink_types.hpp`: `register_sink` support — `parse_sink_args`,
  `render_sink_help`, `trim_context_lines`, `write_stdout`, `write_stderr` and
  `run_sink_tool` (see the `sinks` skill).
- `src/core/process.hpp`: `process::command` / `run_command(argv, stdin, env,
  timeout)` — fork/exec, no shell, no quoting. `on_stdout`/`on_stderr` tee each
  chunk while still accumulating it, and the poll loop enforces the timeout
  while reading. `run_command` defaults to 30 s, so a long fetch passes its own
  timeout (`tse_data::run_tool` allows 10 min and turns a timeout, non-zero exit
  or empty dataset into a `prompt <name>: bin/<tool> …` message).
  `src/core/fs.hpp::bin_tool` locates sibling `bin/` tools (exe dir → git root →
  cwd → `$PATH`).
