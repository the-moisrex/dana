---
name: File Filter
description: The chain-wide file filter in dana (.files.exclude, .files.scope) and the shared file_filter/file_scope SDK helpers that every file-selecting prompt applies.
---

# Chain-wide file filter (`.files.exclude`, `.files.scope`)

Companion skills: `chain-contract`, `sdk-helpers`, `prompt-development`.

Two silent chain steps narrow what every file prompt in the same batch sees;
they emit nothing (their report goes to stderr), so they never alter the context
or the clipboard. `src/prompts/files_exclude.cpp` and `files_scope.cpp` register
with `register_file_filter` (sets `prompt_descriptor::filters_files`, serializing
a batch so a later `.files`/`.read` in it sees the fact). Filter steps are
**batch-global, not positional**: `main.cpp`'s `batch_execution_order` runs every
`filters_files` segment before the rest of its sink-delimited run, so a filter
written after a file prompt still applies to it (the prompt emits nothing, so the
reorder leaves the output untouched). Two consequences: `.files.exclude` steps
accumulate (union), and `.files.scope` steps collapse to the last one in the
batch (no mid-batch scope changes). A sink (`.ai.*`, `.clear`, `.save`) ends the
batch, so a filter after it only affects the following batch.

- **`.files.exclude [PATTERN…]`** publishes `keys::excluded_files`: the union of
  the prior patterns and its own, newline-separated. A pattern is matched
  against a file's absolute path, its path relative to the git root, its path
  relative to `.files.scope`, and its basename (`*`/`?` via
  `dana::glob::match`). A wildcard-free pattern also names a directory (its
  whole subtree is out) and, when it has no `/`, matches a directory component
  anywhere in the path. With no PATTERN it opens `fzf -m` over the candidates
  and excludes the picks. `--clear` drops every exclusion, `--list` prints them
  on stderr.
- **`.files.scope [DIRECTORY]`** publishes `keys::file_root` (absolute, last
  writer wins). It becomes the default search path of `.grep`, `.glob`,
  `.files.grep` and `.read.grep`, the candidate root of `.files`/`.git.files`'
  picker, the first place a relative FILE argument is resolved, and an implicit
  git pathspec for `.git-dirty`, `.git.diff`, `.git.worktree.*`, `.verify` and
  `.commit`. With no DIRECTORY it opens `fzf -m` over the directories (plus the
  scope itself). `--clear`/`--list` as above.

`sdk/file_filter.hpp` is the shared read-only filter: `file_filter::from(prior,
env)` builds it from the facts, `excluded(path)` matches the patterns,
`resolve(env, arg)` tries the scope before the normal resolver,
`candidates()`/`directories()` produce the picker's (git-listed or walked)
entries minus the excluded ones, and `rg_globs()`/`grep_excludes()` translate
the patterns into `rg -g '!…'` / GNU grep `--exclude`/`--exclude-dir` flags.
`select_files(env, filter)` and `select_directories(env, filter)` pipe those
candidates to `fzf -m`; callers keep the bare `select_files(env)` when the
filter is empty so `$FZF_DEFAULT_COMMAND` still applies. `core/glob.hpp`'s
`dana::glob::match`/`has_wildcard` is the one matcher, shared with
`core/git.cpp`'s diff path filter. The reach is every prompt that selects or
embeds files (`.files`, `.read`, `.files.grep`/`.read.grep`, `.grep`, `.glob`,
`.git.files`, `.git-dirty`, `.git.diff`, `.git.worktree.files`/`.diff`,
`.verify`, `.commit`, `.paths`, `.clang-tidy`, `.compress`). Instructions-only
prompts (`.bigo`, `.plan`, `.docstring`) take no FILE arguments and are not
filtered. Known limitation: `.each`/`.exec` child runs start from a fresh fact
store, so they do not inherit the exclusions or the scope.
