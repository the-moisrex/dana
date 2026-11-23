# AGENTS.md — `dana` (native prompt dispatcher, C++)

This directory builds the `prompt` command: a native C++ dispatcher where every
prompt is a `prompts/<name>.cpp` that registers itself. There is no scripting
runtime and no prompt-file scanning; the repo-wide guide is `../AGENTS.md`.

The working guide is split into focused skills under `.agents/skills/`. Each
`.agents/skills/<id>/SKILL.md` is self-contained; load the one that matches the
task instead of reading the whole guide. `.agents/skills` is a default discovery
location for OpenCode and other agent tools, so no config file is needed.

## Skill index

| Skill | Use it for |
| --- | --- |
| `prompt-development` | Chain parsing, build/test/lint, adding or changing a prompt or sink registration, and porting an external `bin/` script to native C++. **Start here.** |
| `dogfooding` | Use `prompt` itself **before** exploring: gather context with `.read.grep`/`.files`/`.git.diff`, offload judgment to `.ai.*`, and batch a whole chain into one invocation. |
| `chain-contract` | `dana::view`/`dana::result`, errors, stdin, parallelism, facts and merge rules, output bytes, text rendering. |
| `file-filter` | `.files.exclude` / `.files.scope` and the shared `file_filter`/`file_scope` helpers. |
| `prompt-files-and-functions` | `*.prompt` splicing/tokenizer/resolution and the `.func` chain-template pre-pass. |
| `sinks` | Sink barriers, shared flags, streaming, passthrough/clear/save hints, stash, path transforms, and the `ai.*` providers with network resilience. |
| `agentic-chains` | `.expect`, `.extract`, `.exec`, `.each`, body parsing, permissions, and the Markdown extractor engine. |
| `compression` | `compress/`, `.compress`, `.compress.list`, `.project`, `.compress.delegate`. |
| `history` | The history store, `.history` family, `.last`, `.history.find`, `.history.purge`, env knobs. |
| `runtime-helpers` | Reference for every `runtime/` helper so prompts reuse them instead of re-open-coding loops. |
| `completion` | `--complete` / `--complete-line`, flag auto-completion, positional callbacks, function/prompt-file candidates. |
| `tool-calling` | Typed tool schemas, `schema::parse`, `.tools`, `.tools.run`, inline tool markup, the `toolcall` extractor, `.skill.gen`. |
| `agent-loop` | `.ai.loop` sessions and the `runtime/alias` path-aliasing layer (`.paths.hide`/`.paths.reveal`). |

## Quick start

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j 8   # Release
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j 8 && ctest --test-dir build-debug               # Debug + tests
clang-format -i $(git ls-files '*.cpp' '*.hpp')
clang-format --dry-run -Werror $(git ls-files '*.cpp' '*.hpp')
clang-tidy -p build $(git ls-files '*.cpp' ':!tests')                        # zero findings
clang-tidy -p build-debug $(git ls-files 'tests/**/*.cpp')                   # zero findings
cmake --install build --prefix "$HOME/cmd" --component prompt                # bin/prompt, bin/md.extract, bin/ai.cli + ai.* symlinks
```

The build type is **not** defaulted (only the `Debug` branch adds the warning
flags), so pass `-DCMAKE_BUILD_TYPE=Release` for the installed binary. `Debug`
adds `-g3 -Wall -Wpedantic -Wextra -Wconversion -Wshadow -fsanitize=address`
and must stay warning-free under **both GCC and clang**. `enable_testing()` and
`add_subdirectory(tests)` only run in a `Debug` build, so use `build-debug` for
`ctest`. `.clang-tidy` is warnings-as-errors; the whole tree must stay
clang-format clean.

See `prompt-development` for the full workflow and the prompt-registration
contract.
