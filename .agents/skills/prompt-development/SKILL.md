---
name: Prompt Development
description: Work on the dana native C++ prompt dispatcher. Covers chain parsing, the build/test/lint workflow, and how to add or change a prompt or sink registration.
---

# Prompt Development — `dana`

This directory builds the `prompt` command: a native C++ dispatcher where every
prompt is a `src/prompts/<name>.cpp` that registers itself. There is no scripting
runtime and no prompt-file scanning; the repo-wide guide is `../AGENTS.md`, and
this skill is the working guide for this subtree.

Companion skills: `chain-contract` (view/result/facts), `sinks`,
`prompt-files-and-functions`, `agentic-chains`, `compression`, `history`,
`sdk-helpers`, `completion`, `tool-calling`, `agent-loop`, `file-filter`.

## Chain parsing

`parse_chain` (`src/sdk/chaining.cpp`) turns argv into invocations. A `.name`
or `-name` shorthand (or a bare head token) is looked up as a prompt name. In
the head position (nothing has matched yet) an unresolved shorthand is a hard
error (exit 2), so a typo like `prompt .dfsgfdsgfdgfds` is never silently
swallowed by the `auto` fallback. A path-like token containing `/`
(`prompt ./build.log`) is kept as an ordinary argument, and after the first
prompt has matched every `.foo` token is that prompt's argument
(`prompt .files .env`), so post-match scope is unchanged. Bare words still
fall through to `auto` — that is how leading file/identifier args reach it.

## Build, test, lint

```bash
cmake -S prompts -B build -DCMAKE_BUILD_TYPE=Release && cmake --build prompts/build -j 8   # Release
cmake -S prompts -B build-asan -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan -j 8 && (cd prompts/build-asan && ctest)                          # Debug + tests
clang-format -i src/**/*.{cpp,hpp} && clang-format --dry-run -Werror src/**/*.{cpp,hpp}
clang-tidy -p build src/*.cpp src/*/*.cpp && clang-tidy -p build-asan tests/*.cpp  # zero findings
cmake --install build --prefix "$HOME/cmd" --component prompt                              # -> bin/prompt, bin/md.extract
```

The build type is **not** defaulted (only the `Debug` branch adds the warning
flags), so pass `-DCMAKE_BUILD_TYPE=Release` for the installed binary — a bare
`cmake -B prompts/build` yields an unoptimised no-config binary. `Debug` adds
`-g3 -Wall -Wpedantic -Wextra -Wconversion -Wshadow -fsanitize=address` and must
stay warning-free under **both GCC and clang**.

`enable_testing()`/`add_subdirectory(tests)` only run in a `Debug` build, so use
`build-asan` for `ctest`. `.clang-tidy` is treated as warnings-as-errors; the
whole tree must stay clang-format clean.

## Adding or changing a prompt

- **One file = one prompt.** A prompt is `src/prompts/<name>.cpp` implementing
  `static void execute_<name>(dana::view const& v, dana::result& r)` and
  `static void render_help_<name>(std::ostream&)`, ending with
  `[[maybe_unused]] bool const registered = dana::register_prompt("<name>", "<summary>", &execute_<name>, &render_help_<name>);`.
  CMake globs `src/prompts/*.cpp`, so no other file changes; registration runs at
  static-init time, which is why `dana_lib` must stay an OBJECT library.
  The registry is a fixed array of `prompt_registry_capacity` (128) slots; a
  build that registers more aborts at startup (`register_prompt` prints and
  calls `std::abort`) rather than silently dropping a prompt, and
  `test_chaining` asserts the catalog fits.
  A data prompt and its data-free lens may share one file when they belong
  together (`tse_intraday.cpp` registers `tse.intraday` and
  `tse.intraday.analysis`); give each its own `bool const` registration
  variable.
- `<summary>` is the first help paragraph verbatim; it is what `prompt list`
  prints. The help's first line must be `Usage: prompt <name>`.
- The name is passed explicitly because dotted/dashed names (`git.diff`,
  `tse.find`, `clang-tidy`) cannot be derived from the identifier.
- A **sink** uses `dana::register_sink(...)` instead (see the `sinks` skill). It
  needs only its executor, help renderer and stream flag; the dispatcher owns
  the shared flags and the barrier ordering.
- A prompt that **embeds file content** registers with `register_file_prompt`
  (sets `emits_files`), and one that **changes the chain-wide file filter**
  (`.files.exclude`, `.files.scope`) with `register_file_filter` (sets
  `filters_files`); both serialize a batch so a later segment sees their facts.
- `record` (the last `register_prompt`/`register_sink` argument, default `true`)
  opts a prompt out of the history store; the `.history*` family sets it to
  `false`, since browsing the store must not append to it. `takes_body` sits just
  before it, so a non-body prompt that must not be recorded passes `false, false`.
