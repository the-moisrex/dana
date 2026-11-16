#!/usr/bin/env bash
# End-to-end file dedup: a chain that names the same file in several file
# prompts embeds it once, `--no-dedup` re-emits it, and a diff-only mode does
# not count as emitting the file.
set -u

prompt=$1
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cd "$work" || exit 1
git init -q
printf 'alpha\n' > a.txt
printf 'beta\n' > b.txt
git add a.txt b.txt
git -c user.email=t@t -c user.name=t commit -qm init

fail=0
check() {
    local what=$1 want=$2 got=$3
    if [ "$got" != "$want" ]; then
        echo "FAIL: $what (want $want, got $got)" >&2
        fail=1
    fi
}

# .files, .files, .read: a.txt once (the later .files and .read skip it).
out=$("$prompt" .files a.txt .files a.txt b.txt .read a.txt 2>/dev/null)
check "files+files+read: File a.txt count" 1 "$(printf '%s\n' "$out" | grep -c '^File a\.txt$')"
check "files+files+read: File b.txt count" 1 "$(printf '%s\n' "$out" | grep -c '^File b\.txt$')"
check "files+files+read: Read a.txt count" 0 "$(printf '%s\n' "$out" | grep -c '^Read file a\.txt')"

# .read twice: the second read skips the file.
out=$("$prompt" .read a.txt .read a.txt 2>/dev/null)
check "read+read: Read a.txt count" 1 "$(printf '%s\n' "$out" | grep -c '^Read file a\.txt')"

# --no-dedup re-emits a file an earlier segment already embedded.
out=$("$prompt" .files a.txt .read a.txt --no-dedup 2>/dev/null)
check "no-dedup: Read a.txt count" 1 "$(printf '%s\n' "$out" | grep -c '^Read file a\.txt')"

# .git.files and .files share the same record.
out=$("$prompt" .git.files a.txt .files a.txt 2>/dev/null)
check "git.files+files: File a.txt count" 1 "$(printf '%s\n' "$out" | grep -c '^File a\.txt$')"

# A full-file git-dirty emission counts, so a later .files skips the file.
printf 'changed\n' >> a.txt
out=$("$prompt" .git-dirty --full-diff .files a.txt 2>/dev/null)
check "git-dirty --full-diff + files: File a.txt count" 0 "$(printf '%s\n' "$out" | grep -c '^File a\.txt$')"
check "git-dirty --full-diff: File: a.txt count" 1 "$(printf '%s\n' "$out" | grep -c '^File: a\.txt')"

# A diff-only git-dirty emission does not count: the later .files still embeds.
out=$("$prompt" .git-dirty --diff .files a.txt 2>/dev/null)
check "git-dirty --diff + files: File a.txt count" 1 "$(printf '%s\n' "$out" | grep -c '^File a\.txt$')"
git checkout -q a.txt

# .compress shares the record with .files.
out=$("$prompt" .files a.txt .compress a.txt 2>/dev/null)
check "files+compress: File a.txt count" 1 "$(printf '%s\n' "$out" | grep -c '^File a\.txt$')"

# The record survives a batch boundary: a passthrough sink separates two runs,
# and the second .files still skips the file the first one emitted (the
# dispatcher seeds a sequential batch with the facts earlier batches left).
out=$("$prompt" .files a.txt .save "$work/sink.md" --show-context .files a.txt 2>/dev/null)
check "sink-separated batches: File a.txt count" 1 "$(printf '%s\n' "$out" | grep -c '^File a\.txt$')"

if [ "$fail" -ne 0 ]; then
    exit 1
fi
echo "file_dedup: ok"
