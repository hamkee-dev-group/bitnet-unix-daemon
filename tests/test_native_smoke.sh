#!/bin/sh
# test_native_smoke.sh — gate-able smoke test for the native backend.
#
# Requires:
#   BITNET_C11_DIR  — path to the bitnet-c11 source tree
#   BITNET_MODEL    — path to a GGUF model file
#
# If either is missing the test prints a skip message and exits 0.
# Otherwise it builds bitnetd with BACKEND=native, starts it in
# foreground mode with a temporary config, waits for the "ready"
# log line, and tears the process down.  Exits nonzero on failure.

set -e

TIMEOUT=${NATIVE_SMOKE_TIMEOUT:-30}
SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

# ── Gate checks ───────────────────────────────────────────────────
if [ -z "$BITNET_C11_DIR" ]; then
    echo "SKIP: BITNET_C11_DIR is not set"
    exit 0
fi

if [ ! -d "$BITNET_C11_DIR" ]; then
    echo "SKIP: BITNET_C11_DIR ($BITNET_C11_DIR) is not a directory"
    exit 0
fi

if [ -z "$BITNET_MODEL" ]; then
    echo "SKIP: BITNET_MODEL is not set"
    exit 0
fi

if [ ! -f "$BITNET_MODEL" ]; then
    echo "SKIP: BITNET_MODEL ($BITNET_MODEL) not found"
    exit 0
fi

# ── Build ─────────────────────────────────────────────────────────
echo "--- building bitnetd (BACKEND=native) ---"
make -C "$SCRIPT_DIR" clean bitnetd \
    BACKEND=native \
    BITNET_C11_DIR="$BITNET_C11_DIR" \
    ${SIMD:+SIMD=$SIMD}

# ── Temporary artifacts ───────────────────────────────────────────
TMPDIR_SMOKE="$(mktemp -d)"
CONF="$TMPDIR_SMOKE/smoke.conf"
LOG="$TMPDIR_SMOKE/smoke.log"
PID="$TMPDIR_SMOKE/smoke.pid"

cleanup() {
    if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
        kill "$DAEMON_PID" 2>/dev/null || true
        wait "$DAEMON_PID" 2>/dev/null || true
    fi
    rm -rf "$TMPDIR_SMOKE"
}
trap cleanup EXIT

# Pick an ephemeral port to avoid collisions.
PORT=$(( 30000 + $$ % 20000 ))

cat > "$CONF" <<EOF
[daemon]
pidfile = $PID
loglevel = debug

[model]
path = $BITNET_MODEL
threads = 2
ctx_size = 512

[server]
listen = 127.0.0.1:$PORT
max_connections = 4
request_timeout = 10
max_body_size = 65536
backlog = 4

[backend]
port = 0

[security]
rate_limit = 60

[metrics]
enabled = true
EOF

# ── Launch ────────────────────────────────────────────────────────
echo "--- starting bitnetd (foreground, port $PORT) ---"
"$SCRIPT_DIR/bitnetd" -f -c "$CONF" > "$LOG" 2>&1 &
DAEMON_PID=$!

# ── Wait for "ready" ─────────────────────────────────────────────
elapsed=0
ready=0
while [ "$elapsed" -lt "$TIMEOUT" ]; do
    if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
        echo "FAIL: bitnetd exited prematurely"
        cat "$LOG"
        exit 1
    fi
    if grep -q "ready" "$LOG" 2>/dev/null; then
        ready=1
        break
    fi
    sleep 1
    elapsed=$((elapsed + 1))
done

if [ "$ready" -ne 1 ]; then
    echo "FAIL: bitnetd did not become ready within ${TIMEOUT}s"
    cat "$LOG"
    kill "$DAEMON_PID" 2>/dev/null || true
    exit 1
fi

echo "--- bitnetd is ready (took ~${elapsed}s) ---"

# ── /health endpoint ─────────────────────────────────────────────
echo "--- checking /health ---"
HEALTH_HTTP=$(curl -s -o "$TMPDIR_SMOKE/health.json" -w "%{http_code}" \
    --max-time 5 "http://127.0.0.1:$PORT/health") || true

if [ "$HEALTH_HTTP" != "200" ]; then
    echo "FAIL: /health returned HTTP $HEALTH_HTTP (expected 200)"
    cat "$TMPDIR_SMOKE/health.json" 2>/dev/null; echo
    cat "$LOG"
    kill "$DAEMON_PID" 2>/dev/null || true
    exit 1
fi

HEALTH_BODY=$(cat "$TMPDIR_SMOKE/health.json")
case "$HEALTH_BODY" in
    *'"status":"ok"'*)
        echo "  /health OK: $HEALTH_BODY"
        ;;
    *)
        echo "FAIL: /health body missing {\"status\":\"ok\"}: $HEALTH_BODY"
        cat "$LOG"
        kill "$DAEMON_PID" 2>/dev/null || true
        exit 1
        ;;
esac

# ── /v1/models endpoint ──────────────────────────────────────────
echo "--- checking /v1/models ---"
MODELS_HTTP=$(curl -s -o "$TMPDIR_SMOKE/models.json" -w "%{http_code}" \
    --max-time 5 "http://127.0.0.1:$PORT/v1/models") || true

if [ "$MODELS_HTTP" != "200" ]; then
    echo "FAIL: /v1/models returned HTTP $MODELS_HTTP (expected 200)"
    cat "$TMPDIR_SMOKE/models.json" 2>/dev/null; echo
    cat "$LOG"
    kill "$DAEMON_PID" 2>/dev/null || true
    exit 1
fi

MODELS_BODY=$(cat "$TMPDIR_SMOKE/models.json")

# Verify it looks like JSON with a data array
case "$MODELS_BODY" in
    *'"data":'*'"id":'*)
        ;;
    *)
        echo "FAIL: /v1/models response is malformed JSON: $MODELS_BODY"
        cat "$LOG"
        kill "$DAEMON_PID" 2>/dev/null || true
        exit 1
        ;;
esac

# The model id should match the GGUF file basename (without path or extension)
MODEL_BASENAME=$(basename "$BITNET_MODEL" .gguf)
case "$MODELS_BODY" in
    *"\"id\":\"$MODEL_BASENAME\""*)
        echo "  /v1/models OK: found model id \"$MODEL_BASENAME\""
        ;;
    *)
        echo "FAIL: /v1/models id does not match GGUF basename \"$MODEL_BASENAME\""
        echo "  response: $MODELS_BODY"
        cat "$LOG"
        kill "$DAEMON_PID" 2>/dev/null || true
        exit 1
        ;;
esac

# ── /v1/chat/completions inference ───────────────────────────────
echo "--- checking /v1/chat/completions (non-streaming) ---"
CHAT_HTTP=$(curl -s -o "$TMPDIR_SMOKE/chat.json" -w "%{http_code}" \
    --max-time 30 \
    -H "Content-Type: application/json" \
    -d '{"messages":[{"role":"user","content":"Say hello."}],"max_tokens":8}' \
    "http://127.0.0.1:$PORT/v1/chat/completions") || true

if [ "$CHAT_HTTP" != "200" ]; then
    echo "FAIL: /v1/chat/completions returned HTTP $CHAT_HTTP (expected 200)"
    cat "$TMPDIR_SMOKE/chat.json" 2>/dev/null; echo
    cat "$LOG"
    kill "$DAEMON_PID" 2>/dev/null || true
    exit 1
fi

CHAT_BODY=$(cat "$TMPDIR_SMOKE/chat.json")

# Verify response is JSON with a choices array containing message content
case "$CHAT_BODY" in
    *'"choices":'*'"message":'*'"content":'*)
        ;;
    *)
        echo "FAIL: /v1/chat/completions response is malformed: $CHAT_BODY"
        cat "$LOG"
        kill "$DAEMON_PID" 2>/dev/null || true
        exit 1
        ;;
esac

# Verify the content field is non-empty (not just "content":"")
case "$CHAT_BODY" in
    *'"content":""'*)
        echo "FAIL: /v1/chat/completions returned empty content"
        echo "  response: $CHAT_BODY"
        cat "$LOG"
        kill "$DAEMON_PID" 2>/dev/null || true
        exit 1
        ;;
esac

echo "  /v1/chat/completions OK: got non-empty response"

# ── Tear down ─────────────────────────────────────────────────────
kill "$DAEMON_PID" 2>/dev/null || true
wait "$DAEMON_PID" 2>/dev/null || true
DAEMON_PID=""

echo "--- native smoke test PASSED ---"
exit 0
