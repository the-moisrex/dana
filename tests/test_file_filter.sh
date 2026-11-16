#!/usr/bin/env bash
# End-to-end chain-level file filtering: `.files.exclude` drops files from every
# later file prompt, `.files.scope` restricts the search root, and the fzf picker
# (stubbed) feeds the exclusion.
set -u

prompt=$1
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cd "$work" || exit 1
git init -q
mkdir -p src tests
printf 'int a;\n' > src/a.cpp
printf 'int b;\n' > src/b.test.cpp
printf 'int t;\n' > tests/t.cpp
printf 'readme\n' > README.md
git add -A
git -c user.email=t@t -c user.name=t commit -qm init

# A stub fzf that picks the first candidate, so the picker path is testable
# without a terminal.
stub=$(mktemp -d)
printf '#!/bin/sh\nhead -1\n' > "$stub/fzf"
chmod +x "$stub/fzf"

fail=0
check() {
    local what=$1 want=$2 got=$3
    if [ "$got" != "$want" ]; then
        echo "FAIL: $what (want $want, got $got)" >&2
        fail=1
    fi
}

# A basename glob excludes the matching file everywhere it appears.
out=$("$prompt" .files.exclude '*.test.cpp' .files src 2>/dev/null)
check "exclude glob: File count" 1 "$(printf '%s\n' "$out" | grep -c '^File')"
check "exclude glob: kept a.cpp" 1 "$(printf '%s\n' "$out" | grep -c '^File src/a[.]cpp$')"

# A directory pattern excludes its whole subtree, and exclusions accumulate.
out=$("$prompt" .files.exclude tests .files.exclude '*.test.cpp' .files src tests 2>/dev/null)
check "exclude dir+glob: File count" 1 "$(printf '%s\n' "$out" | grep -c '^File')"

# `.files.scope` resolves a relative FILE argument under it.
out=$("$prompt" .files.scope src .files a.cpp 2>/dev/null)
check "scope resolves relative arg" 1 "$(printf '%s\n' "$out" | grep -c '^File src/a[.]cpp$')"

# `.files.scope` becomes the default search path for `.grep` and `.glob`.
check "scope default grep path" 1 "$("$prompt" .files.scope tests .grep int 2>/dev/null | grep -c 'tests/t[.]cpp:')"
check "scope default glob path" 1 "$("$prompt" .files.scope tests .glob '*.cpp' 2>/dev/null | grep -c 'tests/t[.]cpp$')"

# `.read.grep` honours the exclusions.
out=$("$prompt" .files.exclude '*.test.cpp' .read.grep int 2>/dev/null)
check "read.grep excludes b.test.cpp" 0 "$(printf '%s\n' "$out" | grep -c 'b[.]test[.]cpp')"

# `.git-dirty` and `.git.diff` honour the exclusions.
printf 'int c;\n' >> src/a.cpp
check "git-dirty excludes the only change" 1 "$("$prompt" .files.exclude src/a.cpp .git-dirty 2>&1 | grep -c 'No changes found')"
check "git.diff excludes the only change" 1 "$("$prompt" .files.exclude src/a.cpp .git.diff 2>&1 | grep -c 'No changes found')"

# The fzf picker: the first candidate (README.md) is excluded.
out=$(PATH="$stub:$PATH" "$prompt" .files.exclude .files . 2>/dev/null)
check "fzf pick excludes README" 0 "$(printf '%s\n' "$out" | grep -c '^File README[.]md$')"
check "fzf pick keeps the rest" 3 "$(printf '%s\n' "$out" | grep -c '^File')"

# `--clear` drops every exclusion.
out=$("$prompt" .files.exclude '*.test.cpp' .files.exclude --clear .files src 2>/dev/null)
check "clear restores the file" 2 "$(printf '%s\n' "$out" | grep -c '^File')"

# Filters are batch-global, not positional: a filter written after a file
# prompt still applies to it (the dispatcher runs filter steps first).
out=$("$prompt" .files src/a.cpp src/b.test.cpp .files.exclude '*.test.cpp' 2>/dev/null)
check "exclude after file prompt: kept a.cpp" 1 "$(printf '%s\n' "$out" | grep -c '^File src/a[.]cpp$')"
check "exclude after file prompt: dropped b.test.cpp" 0 "$(printf '%s\n' "$out" | grep -c 'b[.]test[.]cpp')"

out=$("$prompt" .files src/a.cpp .files.scope tests .glob '*.cpp' 2>/dev/null)
check "scope after file prompt: glob under tests" 1 "$(printf '%s\n' "$out" | grep -c '^/.*tests/t[.]cpp$')"
check "scope after file prompt: glob not src" 0 "$(printf '%s\n' "$out" | grep -c '^/.*src/a[.]cpp$')"

rm -rf "$stub"
if [ "$fail" -ne 0 ]; then
    exit 1
fi
echo "file_filter: ok"
