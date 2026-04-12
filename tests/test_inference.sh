#!/bin/sh
# test_inference.sh — round-trip integration test for /v1/chat/completions.
#
# Usage modes:
#   1. BITNETD_URL is set — test against an already-running instance.
#   2. BITNETD_URL is unset — start a local daemon using BITNET_C11_DIR
#      and BITNET_MODEL, then test against it.
#   3. Neither available — skip with exit 0.

set -e

TIMEOUT=${INFERENCE_TEST_TIMEOUT:-60}
SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DAEMON_PID=""
TMPDIR_TEST=""

cleanup() {
    if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
        kill "$DAEMON_PID" 2>/dev/null || true
        wait "$DAEMON_PID" 2>/dev/null || true
    fi
    [ -n "$TMPDIR_TEST" ] && rm -rf "$TMPDIR_TEST"
}
trap cleanup EXIT

TMPDIR_TEST="$(mktemp -d)"

# ── Determine the base URL ──────────────────────────────────────
if [ -n "$BITNETD_URL" ]; then
    BASE_URL="$BITNETD_URL"
    echo "--- using existing daemon at $BASE_URL ---"
else
    # Try to start our own daemon
    if [ -z "$BITNET_C11_DIR" ] || [ ! -d "$BITNET_C11_DIR" ]; then
        echo "SKIP: BITNETD_URL unset and BITNET_C11_DIR missing or invalid"
        exit 0
    fi
    if [ -z "$BITNET_MODEL" ] || [ ! -f "$BITNET_MODEL" ]; then
        echo "SKIP: BITNETD_URL unset and BITNET_MODEL missing or not found"
        exit 0
    fi

    # Build native backend
    echo "--- building bitnetd (BACKEND=native) ---"
    make -C "$SCRIPT_DIR" clean bitnetd \
        BACKEND=native \
        BITNET_C11_DIR="$BITNET_C11_DIR" \
        ${SIMD:+SIMD=$SIMD}

    PORT=$(( 30000 + $$ % 20000 ))
    CONF="$TMPDIR_TEST/inference.conf"
    LOG="$TMPDIR_TEST/inference.log"
    PID="$TMPDIR_TEST/inference.pid"

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
request_timeout = 60
max_body_size = 65536
backlog = 4

[backend]
port = 0

[security]
rate_limit = 60

[metrics]
enabled = true
EOF

    echo "--- starting bitnetd (foreground, port $PORT) ---"
    "$SCRIPT_DIR/bitnetd" -f -c "$CONF" > "$LOG" 2>&1 &
    DAEMON_PID=$!

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
        exit 1
    fi

    BASE_URL="http://127.0.0.1:$PORT"
    echo "--- bitnetd is ready (took ~${elapsed}s) ---"
fi

# ── POST /v1/chat/completions ───────────────────────────────────
echo "--- POST /v1/chat/completions ---"
CHAT_HTTP=$(curl -s -o "$TMPDIR_TEST/chat.json" -w "%{http_code}" \
    --max-time 60 \
    -H "Content-Type: application/json" \
    -d '{"model":"bitnet","messages":[{"role":"user","content":"Say hello"}],"max_tokens":16}' \
    "$BASE_URL/v1/chat/completions") || true

# ── Assert HTTP 200 ─────────────────────────────────────────────
if [ "$CHAT_HTTP" != "200" ]; then
    echo "FAIL: /v1/chat/completions returned HTTP $CHAT_HTTP (expected 200)"
    cat "$TMPDIR_TEST/chat.json" 2>/dev/null; echo
    [ -f "$TMPDIR_TEST/inference.log" ] && cat "$TMPDIR_TEST/inference.log"
    exit 1
fi
echo "  HTTP 200 OK"

CHAT_BODY=$(cat "$TMPDIR_TEST/chat.json")

# ── Assert "object":"chat.completion" ───────────────────────────
case "$CHAT_BODY" in
    *'"object":"chat.completion"'*)
        echo "  object: chat.completion OK"
        ;;
    *)
        echo "FAIL: response missing \"object\":\"chat.completion\""
        echo "  response: $CHAT_BODY"
        exit 1
        ;;
esac

# ── Assert "choices" array is non-empty ─────────────────────────
case "$CHAT_BODY" in
    *'"choices":['*'"index":'*)
        echo "  choices array: non-empty OK"
        ;;
    *)
        echo "FAIL: response missing non-empty choices array"
        echo "  response: $CHAT_BODY"
        exit 1
        ;;
esac

# ── Assert "content" field is a non-empty string ────────────────
case "$CHAT_BODY" in
    *'"content":""'*)
        echo "FAIL: content field is empty"
        echo "  response: $CHAT_BODY"
        exit 1
        ;;
    *'"content":"'*)
        echo "  content: non-empty string OK"
        ;;
    *)
        echo "FAIL: response missing content field"
        echo "  response: $CHAT_BODY"
        exit 1
        ;;
esac

# ── Assert completion_tokens > 0 ────────────────────────────────
# Extract completion_tokens value using sed
COMPLETION_TOKENS=$(echo "$CHAT_BODY" | sed -n 's/.*"completion_tokens":\([0-9][0-9]*\).*/\1/p')

if [ -z "$COMPLETION_TOKENS" ]; then
    echo "FAIL: completion_tokens not found in response"
    echo "  response: $CHAT_BODY"
    exit 1
fi

if [ "$COMPLETION_TOKENS" -le 0 ] 2>/dev/null; then
    echo "FAIL: completion_tokens is $COMPLETION_TOKENS (expected > 0)"
    echo "  response: $CHAT_BODY"
    exit 1
fi
echo "  completion_tokens: $COMPLETION_TOKENS OK"

# ── Done ────────────────────────────────────────────────────────
echo "--- inference round-trip test PASSED ---"
exit 0
