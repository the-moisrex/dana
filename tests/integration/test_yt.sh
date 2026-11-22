#!/usr/bin/env bash
# End-to-end `prompt yt`: a failed subtitle fetch fails the prompt by default
# (exit 1, nothing on stdout) and `--fallback` restores the old behaviour of
# handing the AI the bare URL.
set -u

prompt=$1
tmp=$(mktemp -d "${TMPDIR:-/tmp}/prompt-yt.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

# The binary is copied beside the stubs: `bin_tool` checks the executable's own
# directory first, so no real network fetch (yt.links/subtitle/srt2text) runs.
cp "$prompt" "$tmp/prompt"
prompt=$tmp/prompt

cat >"$tmp/yt.links" <<'STUB'
#!/bin/sh
cat >/dev/null
echo "https://youtu.be/TESTVIDEO01"
STUB

cat >"$tmp/subtitle" <<'STUB'
#!/bin/sh
dir=$(dirname "$0")
if [ -e "$dir/subtitle.fail" ]; then
    echo "subtitle: could not download" >&2
    exit 1
fi
printf '1\n00:00:00,000 --> 00:00:01,000\nSTUB SUBTITLE TEXT\n'
STUB

cat >"$tmp/srt2text" <<'STUB'
#!/bin/sh
dir=$(dirname "$0")
if [ -e "$dir/srt2text.fail" ]; then
    echo "srt2text: bad input" >&2
    exit 1
fi
cat >/dev/null
echo "SUBTITLE TEXT"
STUB

chmod +x "$tmp/yt.links" "$tmp/subtitle" "$tmp/srt2text"

fail=0
check() {
    local what=$1 want=$2 got=$3
    if [ "$got" != "$want" ]; then
        echo "FAIL: $what (want $want, got $got)" >&2
        fail=1
    fi
}
contains() {
    local what=$1 needle=$2 haystack=$3
    case "$haystack" in
        *"$needle"*) ;;
        *)
            echo "FAIL: $what (missing '$needle')" >&2
            fail=1
            ;;
    esac
}
lacks() {
    local what=$1 needle=$2 haystack=$3
    case "$haystack" in
        *"$needle"*)
            echo "FAIL: $what (unexpected '$needle')" >&2
            fail=1
            ;;
    esac
}

url=https://www.youtube.com/watch?v=TESTVIDEO01

# 1. A working fetch builds the article prompt from the subtitles and the link.
out=$(printf '%s\n' "$url" | "$prompt" .yt 2>"$tmp/err")
check "success: exit" 0 "$?"
contains "success: readable link" "Turn this [youtube video](https://youtu.be/TESTVIDEO01) into an article" "$out"
contains "success: subtitles" "SUBTITLE TEXT" "$out"
lacks "success: no raw-URL request" "Turn this into an article" "$out"
check "success: stderr empty" "" "$(cat "$tmp/err")"

# 2. A failing subtitle fetch fails the prompt: exit 1, no stdout, a reason and
#    the --fallback hint on stderr.
: >"$tmp/subtitle.fail"
out=$(printf '%s\n' "$url" | "$prompt" .yt 2>"$tmp/err")
check "failure: exit" 1 "$?"
check "failure: no stdout" "" "$out"
contains "failure: names the stage" "subtitle exited 1" "$(cat "$tmp/err")"
contains "failure: hint" "--fallback" "$(cat "$tmp/err")"
contains "failure: names the url" "https://www.youtube.com/watch?v=TESTVIDEO01" "$(cat "$tmp/err")"

# 3. --fallback keeps the pre-change behaviour: the raw URL under the article
#    request, exit 0, with a warning on stderr.
out=$(printf '%s\n' "$url" | "$prompt" .yt --fallback 2>"$tmp/err")
check "fallback: exit" 0 "$?"
contains "fallback: raw-URL request" "Turn this into an article" "$out"
contains "fallback: the url itself" "https://www.youtube.com/watch?v=TESTVIDEO01" "$out"
lacks "fallback: no subtitles" "SUBTITLE TEXT" "$out"
contains "fallback: warning" "falling back to the raw URL" "$(cat "$tmp/err")"

# 4. The other pipeline stages fail the same way.
rm -f "$tmp/subtitle.fail"
: >"$tmp/srt2text.fail"
out=$(printf '%s\n' "$url" | "$prompt" .yt 2>"$tmp/err")
check "srt2text failure: exit" 1 "$?"
check "srt2text failure: no stdout" "" "$out"
contains "srt2text failure: names the stage" "srt2text exited 1" "$(cat "$tmp/err")"
out=$(printf '%s\n' "$url" | "$prompt" .yt --fallback 2>/dev/null)
check "srt2text failure + --fallback: exit" 0 "$?"
lacks "srt2text failure + --fallback: no subtitles" "SUBTITLE TEXT" "$out"
rm -f "$tmp/srt2text.fail"

# 5. Non-URL input is not a failure: it is the article request over the text.
text='Some pasted article body.'
out=$(printf '%s\n' "$text" | "$prompt" .yt 2>"$tmp/err")
check "text input: exit" 0 "$?"
contains "text input: request" "Turn this into an article" "$out"
contains "text input: the text" "$text" "$out"
check "text input: stderr empty" "" "$(cat "$tmp/err")"

# 6. Only --fallback is accepted; anything else is a usage error.
printf '%s\n' "$url" | "$prompt" .yt --nope >/dev/null 2>"$tmp/err"
check "unknown flag: exit" 2 "$?"
contains "unknown flag: message" "unexpected argument '--nope'" "$(cat "$tmp/err")"

# 7. The flag is offered by completion (derived from the Options: block).
out=$("$prompt" yt --complete -- 2>/dev/null)
contains "completion: --fallback" "--fallback" "$out"

if [ "$fail" -ne 0 ]; then
    exit 1
fi
echo "yt: ok"
