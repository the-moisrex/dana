---
name: Dogfooding the prompt CLI
description: Use BEFORE exploring the repo, running grep/rg/find/cat/git diff, reading several files, or reviewing a change. The dana `prompt` command gathers context deterministically (.read.grep, .files, .glob, .git.diff, .compress) and offloads judgment to cheaper .ai.* models, so the expensive main-agent context stays small.
---

# Dogfooding the `prompt` CLI

This project builds `prompt`, so the fastest — and cheapest — way to work on it
is to **use it**. The main agent's context is the expensive resource; local
deterministic prompts and the `.ai.*` providers are cheap. Treat the tool as
your pair: deterministic prompts collect facts, one AI sink reasons over them,
and the chain is the batch.

Companion skills: `prompt-development`, `chain-contract`, `agentic-chains`,
`sinks`, `agent-loop`, `compression`, `history`.

## Use this skill first (hard rule)

This is not optional background reading. Before you explore the repo for any
non-trivial task, run a dogfooding chain. If you are about to do any of these,
stop and use `prompt` instead:

- run `grep`, `rg`, `find`, `cat`, `sed`, `head`, `git diff`, `git status`, or
  `ls` more than once;
- read more than two files, or read a file just to locate a symbol;
- implement, review, or debug a change that touches more than one file;
- answer "where is X", "how does Y work", or "what changed".

One `prompt` invocation is the unit of work. Batch the reads, the diff, and the
question into a single chain; do not launch ten separate processes. You should
use this for even better thinking since the other AIs are usually bigger models,
and your own API tokens are costlier than theirs.

## First move

Start with the cheapest chain that can answer the question:

```bash
# Locate and embed the code that mentions a symbol.
prompt .read.grep 'register_prompt' runtime

# Understand a file and its tests together.
prompt .read prompts/gtest_case.cpp tests/unit/test_chaining.cpp

# See exactly what changed before editing.
prompt .git.diff

# Review the uncommitted change with a second model.
prompt .verify .ai.dseek --no-thinking
```

`.verify` embeds the full uncommitted diff itself; piping a diff into it
(`prompt .git.diff | prompt .verify .ai.dseek`) replaces that context. Use the
explicit `.note "..." .git.diff` form when you want to attach a specific
question to the diff.

Only after a deterministic chain has narrowed the problem should you spend your
own tokens reading the result or making the edit. If a task needs judgment
("which of these is the root cause?", "does this diff regress anything?"),
spend a cheap `.ai.*` call on it instead of reasoning through the whole repo
yourself.

## Why

- **Fewer tokens for you.** `.read.grep`, `.files`, `.glob` and `.compress`
  select and shrink context locally; you only read the distilled result.
- **A second opinion is cheap.** `prompt .note "Review this diff." .git.diff
  .ai.dseek --no-thinking` sends the diff to another model, so a bug is caught
  before you spend a turn on it.
- **It tests the product.** Every chain you run exercises the dispatcher,
  sinks, extractors and providers you are editing. A broken chain is a real
  finding, not a hypothetical.
- **One invocation beats many.** A single chain runs its independent segments in
  parallel and keeps the context in-process; separate shell calls re-read files,
  re-pay startup and lose the accumulated context.

## Gather information with deterministic prompts

Prefer these over `cat`/`rg`/`find` in the shell — they produce chain-ready,
deduplicated, filter-aware output and never call an AI:

| Need | Command |
| --- | --- |
| Search contents and embed each matching file once | `prompt .read.grep 'PATTERN' [PATH]` |
| Same, but only the matching files' content as `.files` would embed | `prompt .files.grep 'PATTERN' [PATH]` |
| Match lines (not whole files) | `prompt .grep 'PATTERN' [PATH]` |
| List files by glob | `prompt .glob '**/*.hpp'` |
| Pick/embed files | `prompt .files [PATH…]` |
| Read one file/dir with line numbers | `prompt .read FILE` |
| The uncommitted diff | `prompt .git.diff` / `prompt .git-dirty` |
| Project shape (language, build, tests) | `prompt .project` |
| Symbol index | `prompt .symbols FILE` |
| Shrink bulky sources/tests/CSV | `prompt .compress FILE…` |
| Find an earlier answer | `prompt .history.find QUERY` / `prompt .last` |
| The tool catalog an agent reads | `prompt .tools --for chat` |

Narrow before you read: `.files.scope DIR` pins the search root,
`.files.exclude PATTERN` drops noise, `--head N` caps lines, and
`.compress` replaces low-signal bodies with their shape. A small, precise
context is both cheaper and easier for the reviewing model.

## Task recipes

| Task | Chain |
| --- | --- |
| Find where a symbol is defined | `prompt .read.grep 'symbol' [PATH]` |
| Read one file with line numbers | `prompt .read FILE` |
| Read a file and its tests together | `prompt .read SRC TEST` |
| See what changed | `prompt .git.diff` |
| Port a script/tool to native C++ | `prompt .read OLD_SCRIPT .read TARGET.cpp .ai.dseek --no-thinking` |
| Review a change | `prompt .note "Review this diff." .git.diff .ai.dseek --no-thinking` |
| Cross-check a risky change | `prompt .note "Cross-check this diff." .git.diff .ai.debate dseek gap --no-thinking` |
| Check the area you touched | `prompt .read.grep 'TODO\|FIXME' PATH .ai.gap --no-thinking` |

The AI recipe is a starting point: ask it for a concrete port plan or a diff,
then verify the answer with a deterministic prompt before editing. Do not paste
the whole repo into your own context to do work a cheap chain can do.

## Collaborate with other AIs

The `.ai.*` sinks send the accumulated chain context to a provider and put the
answer back into the chain. Use a **different** provider for review than the one
you used to write the change when you can.

Always pass `--no-thinking`: it hides the models' reasoning and stops the
provider from spending tokens on it, so a review costs only the answer.

```bash
# Ask a model to review the uncommitted change.
prompt .verify .ai.dseek --no-thinking

# Give a model the diff plus a specific question.
prompt .note "Find regressions in this diff." .git.diff .ai.gap --no-thinking

# Hand a model the exact files that mention a symbol, then ask.
prompt .note "Does this registration order stay consistent?" \
       .read.grep 'register_prompt' runtime .ai.chatgpt --no-thinking

# Cross-check two models; the panel argues and returns a verdict.
prompt .verify .ai.debate dseek gap --no-thinking

# Continue the same conversation instead of resending the context.
prompt .note "Now show the minimal patch." .ai.reply --no-thinking
```

Available providers: `.ai.gap`, `.ai.chatgpt`, `.ai.dseek`, `.ai.grok`,
`.ai.claude`, `.ai.kimi`, `.ai.perplexity`, `.ai.gemini`, `.ai.lms`,
`.ai.openai`, `.ai.openrouter`. `.ai.debate` runs several as a panel, and
`.ai.loop` lets a model call the repo's tools across turns (see the
`agent-loop` skill). Use `--print-prompt` to see exactly what would be sent
without spending a request.

## Batch everything into one chain

**Pack as many steps as the task needs into one `prompt` invocation.** The chain
is the batch: segments before a sink are gathered together (independent ones run
in parallel), the sink sees the combined context, and later segments see its
answer.

```bash
# One invocation: gather the registration code, the tests and the diff,
# then ask one model to review all of it.
prompt .note "Review these changes for correctness and missing tests." \
       .read.grep 'register_prompt' runtime \
       .read.grep 'register_prompt' tests \
       .git.diff \
       .ai.dseek --no-thinking
```

- Put **deterministic gathering first**, the **AI call last** — the sink is a
  barrier, so everything before it is one context.
- Repeat work with `.each … .end` (one child run per item) instead of a shell
  loop; run a command with `.exec -- …` instead of a separate shell call.
- When you do need the shell, still make **one** call that runs the whole chain;
  do not launch ten separate `prompt` processes.
- In the agent harness, issue independent tool calls together in a single batch
  rather than one after another.

## Verify before you declare done (hard rule)

Do not end a non-trivial task without at least one review chain. Run it at the
end of a change, and again after a non-trivial edit:

```bash
prompt .verify .ai.dseek --no-thinking          # review the uncommitted diff
prompt .git.diff .ai.grok --no-thinking         # a second opinion
prompt .read.grep 'TODO' .ai.gap --no-thinking  # check the area you touched
```

`.verify` picks up the uncommitted diff on its own, so it is the shortest review
chain. Add `.note "..."` when you want to ask a specific question about the
diff.

Treat a finding as a hypothesis, not an order: reproduce it with a deterministic
prompt (`.read`, `.read.grep`, `.grep`) before editing. If the model is wrong,
say so and move on — the point is a cheap independent check, not deference.

## Cost discipline

- Pass `--no-thinking` on every `.ai.*` call so reasoning tokens are not spent
  on a routine review.
- Gather deterministically; spend an AI call only on judgment.
- One rich context beats several small calls: batch the reads into the same
  chain as the question.
- Reuse: `.ai.reply` continues a chat, `.save`/`.history` keep an answer, and
  `.last`/`.history.find` retrieve it instead of asking again.
- Shrink before sending: `.files.scope`, `.files.exclude`, `--head`, `.compress`.
- Use `--print-prompt` (free) to validate what a sink would send.
- Reserve `.ai.debate` and `.ai.loop` for cases that need them; a single
  provider is enough for a routine review.
- Your own AI token is costlier than these AI tokens, so feel free to use them.
