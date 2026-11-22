#!/usr/bin/env bash
# A tool call must read a file even when the chain already embedded it: the
# `keys::files` dedup record is a chain-wide record, not a per-call one, so a
# `read` call for an already-embedded file must still return its content.
set -u

prompt=$1
# Use /tmp explicitly: the sanitizer rewrites $TMPDIR (often under $HOME) to
# /tmp, which would make the path in the tool call point elsewhere.
work=$(mktemp -d /tmp/prompt-tools.XXXXXX)
trap 'rm -rf "$work"' EXIT

printf 'one\ntwo\nthree\n' >"$work/numbered.txt"

call=$(printf '```json\n{"tool":"read","input":{"file":["%s/numbered.txt"]}}\n```' "$work")

out=$("$prompt" .files "$work/numbered.txt" .note "$call" .tools.run 2>&1)
if ! printf '%s\n' "$out" | grep -q '^1: one$'; then
    echo "FAIL: tool read of an already-embedded file returned no content" >&2
    printf '%s\n' "$out" >&2
    exit 1
fi
