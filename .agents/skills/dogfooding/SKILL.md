---
name: Dogfooding the prompt CLI
description: Use the dana `prompt` command itself while developing the project — gather context with deterministic prompts (.read.grep, .files, .glob, .git.diff, .compress), collaborate with other AIs through the .ai.* sinks, and batch a whole chain into one invocation to save tokens and round trips.
---

# Dogfooding the `prompt` CLI

This project builds `prompt`, so the fastest way to work on it is to **use it**.
Instead of re-reading whole files or spending your own tokens on grunt work, let
the binary gather context and let other models review the result. Treat the
tool as your pair: deterministic prompts collect facts, one AI sink reasons over
them, and the chain is the batch.

Companion skills: `prompt-development`, `chain-contract`, `agentic-chains`,
`sinks`, `agent-loop`, `compression`, `history`.

## Why

- **Fewer tokens for you.** `.read.grep`, `.files`, `.glob` and `.compress`
  select and shrink context locally; you only read the distilled result.
- **A second opinion is cheap.** `prompt .verify .ai.dseek` sends the diff to
  another model, so a bug is caught before you spend a turn on it.
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

## Verify before you declare done

Run a review chain at the end of a change, and again after a non-trivial edit:

```bash
prompt .verify .ai.dseek --no-thinking          # correctness pass over the diff
prompt .git.diff .ai.grok --no-thinking         # a second opinion
prompt .read.grep 'TODO' .ai.gap --no-thinking  # check the area you touched
```

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
