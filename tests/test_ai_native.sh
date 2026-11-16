#!/usr/bin/env bash
# End-to-end native API providers: `ai.cli` and the `.ai.*` sinks speak the
# OpenAI-compatible protocol to a local stub (no network, no key).
set -euo pipefail

prompt=$1
cli=$2
tmp=$(mktemp -d "${TMPDIR:-/tmp}/prompt-ai-native.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

fail() {
    echo "test_ai_native: $*" >&2
    exit 1
}

cat >"$tmp/server.py" <<'PY'
import json
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

port_file, log_file = sys.argv[1], sys.argv[2]

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def _body(self):
        length = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(length) if length else b""

    def do_GET(self):
        if self.path.endswith("/models"):
            payload = {"data": [
                {"id": "stub-model", "name": "Stub Model", "context_length": 8192,
                 "pricing": {"prompt": "0.000001", "completion": "0.000002"}},
                {"id": "embed-model", "type": "embedding"},
            ]}
            body = json.dumps(payload).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_response(404)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_POST(self):
        body = self._body()
        with open(log_file, "a", encoding="utf-8") as handle:
            handle.write(body.decode("utf-8", "replace") + "\n")
        if not self.path.endswith("/chat/completions"):
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "close")
        self.end_headers()
        events = [
            {"choices": [{"delta": {"reasoning_content": "thinking "}}]},
            {"choices": [{"delta": {"content": "hello "}}]},
            {"choices": [{"delta": {"content": "from stub"}}]},
            {"usage": {"prompt_tokens": 3, "completion_tokens": 4}},
        ]
        for event in events:
            self.wfile.write(("data: " + json.dumps(event) + "\n\n").encode())
            self.wfile.flush()
        self.wfile.write(b"data: [DONE]\n\n")
        self.wfile.flush()

server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
with open(port_file, "w", encoding="utf-8") as handle:
    handle.write(str(server.server_address[1]))
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    threading.Event().wait()
except KeyboardInterrupt:
    pass
PY

python3 "$tmp/server.py" "$tmp/port" "$tmp/bodies.log" &
server_pid=$!
trap 'kill "$server_pid" 2>/dev/null || true; rm -rf "$tmp"' EXIT

for _ in $(seq 1 100); do
    [[ -s "$tmp/port" ]] && break
    sleep 0.05
done
[[ -s "$tmp/port" ]] || fail "the stub server did not start"
port=$(cat "$tmp/port")
base="http://127.0.0.1:$port"

# --- ai.cli: a plain turn, reasoning on stderr, JSON events ---------------
out=$("$cli" openai --base-url "$base" --api-key test -m stub-model --no-thinking "say hi" 2>"$tmp/err")
[[ $out == *"hello from stub"* ]] || fail "ai.cli answer: $out"
grep -q '\[usage\]' "$tmp/err" || fail "ai.cli did not report usage"

json=$("$cli" openai --base-url "$base" --api-key test -m stub-model --json "say hi" 2>/dev/null)
[[ $json == *'"kind": "text"'* ]] || fail "ai.cli --json text event"
[[ $json == *'"kind": "done"'* ]] || fail "ai.cli --json done event"

# --- --list, --print-session, --print-prompt -----------------------------
models=$("$cli" openai --base-url "$base" --api-key test --list)
[[ $models == *"stub-model"* ]] || fail "--list: $models"
[[ $models == *"ctx 8192"* ]] || fail "--list context: $models"
[[ $models == *"embed-model"* ]] && fail "--list kept an embedding model"

session=$("$cli" openai --base-url "$base" --api-key test -m stub-model --print-session)
[[ $session == *'"reachable": true'* ]] || fail "--print-session: $session"
[[ $session == *'"stub-model"'* ]] || fail "--print-session model: $session"

printed=$("$cli" openai --base-url "$base" --api-key test -m stub-model --print-prompt "raw prompt")
[[ $printed == "raw prompt" ]] || fail "--print-prompt: $printed"

# --- --prompt-file is refused (no upload endpoint) -----------------------
set +e
"$cli" openai --base-url "$base" --api-key test -m stub-model --prompt-file "x" >/dev/null 2>"$tmp/refused"
status=$?
set -e
[[ $status -eq 2 ]] || fail "--prompt-file exit status $status"
grep -q 'no upload endpoint' "$tmp/refused" || fail "--prompt-file message"

# --- -c replays the transcript -------------------------------------------
: >"$tmp/bodies.log"
"$cli" openai --base-url "$base" --api-key test -m stub-model --no-thinking "first turn" >/dev/null 2>&1
"$cli" openai --base-url "$base" --api-key test -m stub-model --no-thinking -c "second turn" >/dev/null 2>&1
second=$(tail -n 1 "$tmp/bodies.log")
[[ $second == *'"first turn"'* ]] || fail "-c did not replay the first turn: $second"
[[ $second == *'"hello from stub"'* ]] || fail "-c did not replay the answer: $second"
[[ $second == *'"second turn"'* ]] || fail "-c missed the new turn"

# --- the sink runs in-process and keeps the plain answer -----------------
sink=$(printf 'note' | "$prompt" .note "say hi" .ai.openai --base-url "$base" --api-key test -m stub-model --no-thinking 2>/dev/null)
[[ $sink == *"hello from stub"* ]] || fail "sink answer: $sink"
[[ $sink != *$'\x1b'* ]] || fail "sink emitted ANSI escapes when piped"

# The sink's chain context is the plain answer, so a following .note sees it.
chained=$(printf 'x' | "$prompt" .note "say hi" .ai.openai --base-url "$base" --api-key test -m stub-model --no-thinking .note "after" 2>/dev/null)
[[ $chained == *"hello from stub"* ]] || fail "chained sink lost the answer"
[[ $chained == *"after"* ]] || fail "chained sink lost the trailing note"

# --- the sink's --print-prompt is a dry run ------------------------------
dry=$(printf 'the context' | "$prompt" .ai.openai --print-prompt 2>/dev/null)
[[ $dry == "the context" ]] || fail "sink --print-prompt: $dry"

echo "test_ai_native: ok"
