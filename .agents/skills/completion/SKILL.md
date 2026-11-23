---
name: Shell Completion
description: The dana binary-driven shell completion (--complete, --complete-line) — flag auto-completion, positional callbacks, line-mode chain replay, function/prompt-file candidates and tests.
---

# Shell completion (`--complete`, `--complete-line`)

Companion skills: `prompt-development`, `prompt-files-and-functions`,
`tool-calling`.

Every candidate a shell offers comes from the binary: `../shell/completions.fish`
forwards the command line and prints what it gets back, so the completion logic
lives next to the chain semantics it mirrors instead of in the shell script.
Both entry points print `value<TAB>description` lines and both are answered
early in `main.cpp` (before the clipboard is initialized and before stdin is
read), so completion never runs a prompt, consumes the pipe or copies anything.

- `prompt <name> --complete <prefix> [args typed before it...]` — one prompt's
  own candidates (alias `prompt --complete <name> [prefix] ...`).
- `prompt --complete-line <current> [tokens after `prompt`]` — a whole command
  line (`sdk/line_completion.cpp`), which fish calls:
  `prompt --complete-line "$current" $tokens[2..-1]`, under two conditions only
  (name position/step boundary with `-f` so no file noise, every argument
  position without it so fish's own path completion still applies).

- **Flags are automatic.** `completion::write_help_flags` parses the `Options:`
  block of the prompt's own `render_help_fn` output, so every prompt completes
  `--head`, `--num`, `--days`, `--format`, … with no per-file code; both
  spellings of `--type, -t` are emitted. `write_prompt_candidates` (the shared
  `sdk/completion.cpp` renderer) is what both forms call, so a prompt answers
  identically either way.
- **Positional/value candidates** come from the prompt's optional
  `completion_fn` — the fifth, defaulted `register_prompt` argument
  (`sdk/completion.hpp`, embedded by `new`). Put the callback next to the
  prompt's argument parser and use the helpers: `completion::matches`,
  `write_item`, `write_items`, `write_values`, and `write_values_after_flag`
  (completes a flag's value when it is the previous argument).
  `tse_data::complete_instrument` delegates the `tse`/`tse.intraday`
  identifier lookup to `bin/tse.resolve --complete`; `skill`, `run`, `diagram`,
  `metadata`, `opportunities` and `overengineering` have small callbacks.
- **The line mode** replays just enough of `parse_chain` to know what the
  current token is being completed against: a bare token opens a step only while
  none matched (the head), a `.name`/`-name` token naming a prompt **or a
  function** opens the next one, later tokens are that step's arguments
  (`write_step_names` covers the head and the boundaries with the catalog plus
  the visible functions, in all three spellings, merged into one locale-aware
  order). A `--` tail and an open `.each` body are opaque and produce nothing.
  A half-typed chain is normal input, so the scan never calls `parse_chain`.
- **Functions** are completed from the same place the expander resolves them:
  the line is first run through the `*.prompt` pass
  (`expand_prompt_file_tokens`, so a definition living in an inlined file is
  found), then the global `funcs` files and the inline `.func … .end` blocks
  (`collect_inline_functions`) supply the names. The `.func` keyword is offered
  at the top level, `.end` inside a `.func` body, and the body itself completes
  as a chain. `.func` is not offered inside a body and the signature token is
  left alone.
- **`*.prompt` candidates** live with the resolver they mirror,
  `write_prompt_file_candidates` in `sdk/prompt_file.cpp`: the same three
  sources, nearest first, each candidate naming its source (`prompt file`,
  `prompt file (project)`, `prompt file (config)`) and a shared name emitted
  once; a token containing `/` lists that directory instead, offering its
  `*.prompt` files and its subdirectories so the user can descend. They are
  offered at the head, and at an argument position only for a deliberate
  `.prompt`/path token (a plain word stays a word, which is what keeps
  `.files src/ma<TAB>` on fish's path completion). `--no-prompt-files` anywhere
  in the tokens suppresses them.
- `prompt_descriptor::complete_fn` stays `nullptr` when a prompt registers no
  callback (flags only). Tests: `tests/test_completion.cpp` plus the
  `complete_flags`/`complete_values`/`complete_line_*` ctest smokes.
