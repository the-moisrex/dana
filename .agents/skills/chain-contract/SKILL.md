---
name: Chain Contract
description: The dana prompt read/write contract (view, result, errors, stdin, parallelism), the facts channel and its merge rules, the output byte rules, and text rendering conventions.
---

# Chain Contract

Companion skills: `prompt-development`, `file-filter`, `sinks`, `history`.

## Prompt contract

Two types, one direction each: `dana::view` is everything a segment may
**read**, `dana::result` is everything it may **produce**. Nothing in a prompt
mutates chain state, which is what makes forked parallel segments safe.

- **Reading** (`sdk/stage.hpp`): `v.name`, `v.args` (a span — `.empty()`,
  `.size()`, indexed), `v.in.text()` (the shared stdin, a `string_view`),
  `v.env.git_root`/`v.env.exe_path`, `v.text` (the accumulated chain context,
  which a sink reads), `v.body` (the raw `.each` body tokens, empty for every
  prompt that is not registered with `takes_body`) and `v.prior` (the facts
  earlier segments declared or published). `v.stream` is the live-terminal
  writers, set by the dispatcher for a streaming sink only.
- **Writing** (`r.`): `r.out` (stdout — sanitised, piped onward, clipboarded),
  `r.err` (stderr, exit stays 0), `r.stderr_buffer` (sink streaming only),
  `r.published` (facts later segments read), `r.no_clipboard`, `r.clear`
  (sinks: drop the accumulated context after this sink succeeds).
- **Fail by throwing** (`src/sdk/errors.hpp`): `usage_error` (exit 2),
  `soft_error` (exit 1), or `prompt_error` with `message`, `partial_output`,
  `no_clipboard`, `stdin_consumed`. `main.cpp` catches them once; a failed
  prompt's partial stdout is still accumulated and the chain continues.
- **Stdin:** chained prompts share the **original** piped stdin, and it is
  spend-once: a prompt that reads it calls `v.in.spend()` (`v.in.text()`, then
  `v.in.spend()`, with `embed_stdin(v.in.text(), …)` formatting it).
  `v.in.piped()` says a pipe was attached at all; `v.in.available()` says nobody
  has read it yet.
- **Parallelism:** with no piped stdin the dispatcher runs independent segments
  in parallel (forked children; prompts must not assume they are the only
  segment running) and emits their results in argv order. Piped stdin keeps the
  chain sequential. A forked segment sees the **declarations** collected before
  the batch (that is what they are for) but never a sibling's `r.published`
  artifacts: those are folded back by the parent once the batch completes, and
  only later runs read them. A batch containing a file source (`emits_files`)
  or a file-filter step (`filters_files`) is therefore run **sequentially**, in
  argv order, with each finished segment's artifacts merged into the next one's
  prior facts, which is what lets a later file prompt skip what an earlier one
  emitted and see the filter an earlier `.files.exclude`/`.files.scope` set.

### Facts (`sdk/facts.hpp`)

Facts are the only inter-prompt channel: an ordered string→string store with
well-known keys in `namespace dana::keys`. Values are length-prefixed on the
wire, so any content (newlines, UTF-8) survives. There are exactly two write
points, with two merge rules:

- **Declarations** — `register_prompt`'s last argument, a
  `void declare_<name>(declaration& d)`. They run in the parent _before_ any
  batch forks and keep the **first** writer, so parallel siblings agree on
  identity derived purely from `d.args`/`d.env` (cheap: no I/O, no fzf, no
  network; a failing declaration is swallowed and the segment falls back to its
  own arguments).
- **Artifacts** — `r.published.set(key, value)` after a successful segment, with
  the **last** writer winning, i.e. the nearest successful preceding segment.
  That is exactly what `.save`'s filename wants.

One agreement rides on this today: `tse_data::declare_identifier` publishes
`keys::tse_symbol` once per chain, so `.tse foolad .tse.codal` and the reverse
order resolve the same instrument. `.delegate` and `.expect` publish
`keys::expect` (a newline-separated plan of `[NAME=]SELECTOR` entries) so
`.extract`, `.exec` and `.each` need no selector of their own; `.save` declares
`keys::save_wants_name` (a presence-only marker, with no target of its own) so
`.delegate` can ask the AI to name the file. File prompts share `keys::files`
(`files.emitted`), the newline-separated canonical paths whose content the chain
has already embedded: a prompt builds a `file_scope` from `v.prior`
(`sdk/file_scope.hpp`), skips `seen()` files, `mark()`s what it embeds and
publishes the ordered union as its artifact, so
`prompt .read.grep X .files.grep X .files X .read X` embeds each file once. A
prompt opts in by registering with `register_file_prompt` (which sets
`prompt_descriptor::emits_files`, serializing a batch that contains one) and
`--no-dedup` (`sdk/args.hpp`'s `kNoDedupFlag`, carried in `file_options.dedup`)
re-emits a file for a continuation read like `.read X --offset 100`. Facts are
deliberately opt-in: add a key when two prompts genuinely cannot work it out
from their own arguments, not as a general channel.

The chain-wide file filter rides the same channel: `.files.exclude` publishes
`keys::excluded_files` (`files.excluded`, newline-separated patterns that
accumulate) and `.files.scope` publishes `keys::file_root` (`files.root`, an
absolute directory, last writer wins). Every file prompt builds a
`file_filter` from `v.prior` (`sdk/file_filter.hpp`) and applies it before
`file_scope` dedup and emission; a filter step registers with
`register_file_filter` (which sets `prompt_descriptor::filters_files`, also
serializing a batch). See the `file-filter` skill.

The fork wire (`sdk/wire.hpp`) carries a `segment_outcome` — stdout, stderr, the
separate stderr buffer, the published facts, the output flags and the exit
status — as one length-prefixed blob per child; `drain_all()` polls every child
pipe at once instead of draining them in turn.

## Output bytes

The dispatcher's output is the contract the tests and the AI-facing prose rely
on; there is no other implementation to stay compatible with. Three behaviours
are deliberate and worth keeping in mind:

- **One text-fold rule.** Every emitted segment goes through
  `text::append_block` (`src/core/strings.{hpp,cpp}`): the block is trimmed of
  leading and trailing newlines, an empty segment contributes nothing (no stray
  separator), consecutive blocks are separated by exactly one blank line, and the
  buffer ends with exactly one newline. The chain context (`chain_state::append`),
  the stdout accumulator, the clipboard builder (`output_emitter`) and the history
  replay (`final_output`) all share it, so stdout, the clipboard and the
  context a sink receives have the same shape. Prompts therefore never trim their
  own output for presentation — except `.c.p`, which normalizes before its own
  `--head N` so the cap counts real content lines.
- The dispatcher prepends a one-line **redaction legend**
  (`sanitize::kRedactionLegend`) whenever `clean_privacy` rewrites personal data
  (`HOME`, `USER`, `HOSTNAME`, `IP_ADDRESS`, `MAC_ADDRESS`, `EMAIL_ADDRESS`,
  `PHONE_NUMBER`, `SSN`). It is a one-line header, so it sits directly above the
  first redacted block with no blank line between them.
- Prompts such as `tse`, `tse.rigor` and `tse.times` and the `--num`/`--numbered`
  options are specific to this dispatcher; the shared help helpers standardise
  the usage lines across prompts.

When changing output, prefer the shared helpers (`file_usage_help`,
`file_options_help`) and keep the wording consistent across prompts.

## Rendering text

- Prefer a raw string literal (`R"PROMPT(...)PROMPT"`) for multi-line prose and
  `std::format` for interpolation.
- The usage lines and the shared `--head`/`--num` options come from
  `file_usage_help` / `file_options_help` (`sdk/args.hpp`); don't re-type them.
- `new` (`new.cpp`) generates a new native prompt and embeds the SDK headers
  plus `fix.cpp` as assets. Those snapshots are baked in at build time from
  `PROMPT_ASSETS` in `CMakeLists.txt` (add a header there when the guidance
  should include it); keep `new.cpp`'s guidance text in sync with the helpers.
