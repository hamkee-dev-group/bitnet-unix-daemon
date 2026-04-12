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

# ── Tear down ─────────────────────────────────────────────────────
kill "$DAEMON_PID" 2>/dev/null || true
wait "$DAEMON_PID" 2>/dev/null || true
DAEMON_PID=""

echo "--- native smoke test PASSED ---"
exit 0
