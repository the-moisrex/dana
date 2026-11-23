---
name: Prompt Files and Functions
description: The dana *.prompt file pre-pass (splicing, tokenizer, resolution) and the .func chain-template pre-pass (definition, invocation, global sources, errors).
---

# Prompt files (`prompt debug.prompt`)

Companion skills: `prompt-development`, `completion`.

`*.prompt` files hold the chain DSL that otherwise goes on the command line:
`prompt debug.prompt` behaves exactly as if the file's tokens had been typed
after `prompt`. `sdk/prompt_file.{hpp,cpp}` is a lexical pre-pass over argv that
runs in `main.cpp` before `parse_chain`, so chain parsing, sinks, `.each` bodies,
`.exec` ownership, the clipboard and the history store only ever see an ordinary
argv (the history records the _effective_ tokens, which is what a replay wants).

- **Splicing:** any argument token ending in `.prompt` that resolves to a
  readable file is replaced, in place, by the file's tokens, so
  `prompt .note "context" debug.prompt .length short` works. A `*.prompt` token
  that does not resolve is an error only in the first argv position (where the
  `auto` fallback would silently eat it); elsewhere it stays an ordinary
  argument, so `.files missing.prompt` keeps the prompt's own warning. `--`
  stops the pass for the rest of that token sequence, which is what keeps
  `.exec -- command x.prompt` untouched, and `--no-prompt-files` as the first
  argument copies argv unchanged.
- **Tokenizer:** spaces and newlines separate; `#` at the start of a token
  comments out the rest of its line (`a#b` stays one token); `"..."` honours
  `\"` and `\\`, `'...'` is literal, and a backslash outside quotes escapes the
  next character. Nothing is expanded (no `$VAR`, globbing or command
  substitution). An unterminated quote is `prompt: <file>:<line>: unterminated "`
  and exits 2.
- **Resolution:** a name is looked up next to the file that named it (the
  current directory at the top level, so a nested `inner.prompt` means the one
  beside its includer), then `<git-root>/.prompt/`, then
  `$XDG_CONFIG_HOME/prompt/prompts` (`~/.config/prompt/prompts`). Files may
  include other files up to `kMaxPromptFileDepth` (8) deep; cycles are rejected
  on canonical paths and a file whose first token is `prompt` may keep it.
- The recursion (`expander::sequence` ↔ `expander::file`) is bounded by the
  depth cap and the cycle check, which is why both carry a
  `misc-no-recursion` suppression.

Tests: `tests/test_prompt_file.cpp` (tokenizer, inlining, nesting, cycle, depth,
lookup order, `--`, `--no-prompt-files`) and the `prompt_file_run`/
`prompt_file_missing` ctests over `tests/fixtures/hello.prompt`.

# Functions (`.func NAME(params) … .end`, called as `.name`)

A **function** is the third kind of step, next to a prompt and a sink: a named,
parameterized chain template the user defines and invokes exactly like a prompt.
`sdk/function.{hpp,cpp}` is a lexical pre-pass in `main.cpp` that runs
immediately after the `*.prompt` pass and before `parse_chain`, so sinks,
`.each` bodies, `.exec` ownership, the clipboard and the history store only ever
see an ordinary argv.

- **Definition.** `.func NAME(params) STEPS… .end`. The name and signature are a
  single token (`NAME` or `NAME(p, p=default)`, no spaces inside the parens), so
  a body may start with a bare token without ambiguity. The body ends at the
  matching `.end`, counting compiled body-taking steps (`.each` … `.end`) so
  theirs does not close the definition. Definitions may sit in the command, in a
  `*.prompt` file, or in a `funcs` file; several may appear in one source.
- **Invocation.** `.name args…` (a bare first token that names a function also
  works). A call binds declared params by position, or by `--name value` /
  `--name=value`; positionals past the declared params are `{@}`. In the body
  `{name}` is a value (embedded substitution inside a token is allowed), `{0}`
  is the function name and `{N}` the Nth argument. A `{placeholder}` that is not
  a declared param is left untouched, so `tse`/`.each` fields (`{symbol}`,
  `{item}`, …) compose. A function call's arguments stop at the next step, and
  the whole body is a normal chain.
- **Global sources.** `<git-root>/.prompt/funcs`, then
  `$XDG_CONFIG_HOME/prompt/funcs` (project over user per name). `$PROMPT_FUNCS`
  (colon-separated) replaces the list; an empty value disables global functions.
  A `funcs` file holds only `.func` blocks (a stray token is an error) and uses
  the same tokenizer as `*.prompt` files.
- **Errors are hard (exit 2).** A function may not shadow a compiled prompt or
  sink, nor use a reserved name (`func`, `end`, `prompt`); a duplicate in one
  source, a bad signature/name, a missing/unexpected `.end`, a missing required
  argument, an unknown `--param`, and recursion (direct or through another
  function, capped by `kMaxFunctionDepth`) are all failures. `.func` is top
  level only.
- **Composition with bodies.** A `.func` body may contain `.each … .end` and
  `.exec -- …`; the expander tracks compiled body depth and copies everything
  after `--` verbatim, and a function called inside an `.each` body is expanded
  by the parent so inline definitions stay visible to the child processes.

Tests: `tests/test_function.cpp` (definition/call, params/defaults/named flags/
`{@}`/`{0}`/`{N}`, nesting, `.each`-body and `--` preservation, every error
case, `funcs`-file loading and override) plus the
`function_inline`/`function_params`/`function_shadow` ctests. The suite's global
`ENVIRONMENT` sets `PROMPT_FUNCS=` so a developer's own funcs never leak in.
