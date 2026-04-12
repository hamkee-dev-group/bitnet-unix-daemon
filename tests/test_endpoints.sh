#!/bin/sh
# test_endpoints.sh — integration test for /health and /v1/models endpoints.
#
# Usage:
#   BITNETD_URL=http://host:port  sh tests/test_endpoints.sh
#
# If BITNETD_URL is set, the script tests against that address.
# Otherwise it tries localhost:8080.  If nothing is listening there,
# it attempts to build and start bitnetd with BACKEND=native (requires
# BITNET_C11_DIR and BITNET_MODEL).  If those are not set, it skips
# with exit 0.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
STARTED_DAEMON=""
DAEMON_PID=""
TMPDIR_EP=""

cleanup() {
    if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
        kill "$DAEMON_PID" 2>/dev/null || true
        wait "$DAEMON_PID" 2>/dev/null || true
    fi
    [ -n "$TMPDIR_EP" ] && rm -rf "$TMPDIR_EP"
}
trap cleanup EXIT

TMPDIR_EP="$(mktemp -d)"
FAILS=0

# ── Determine target URL ─────────────────────────────────────────
if [ -n "$BITNETD_URL" ]; then
    BASE_URL="$BITNETD_URL"
elif curl -sf --max-time 2 "http://127.0.0.1:8080/health" >/dev/null 2>&1; then
    BASE_URL="http://127.0.0.1:8080"
else
    # No running instance — try to start one with native backend.
    if [ -z "$BITNET_C11_DIR" ] || [ -z "$BITNET_MODEL" ]; then
        echo "SKIP: no running bitnetd and BITNET_C11_DIR/BITNET_MODEL not set"
        exit 0
    fi
    if [ ! -d "$BITNET_C11_DIR" ]; then
        echo "SKIP: BITNET_C11_DIR ($BITNET_C11_DIR) is not a directory"
        exit 0
    fi
    if [ ! -f "$BITNET_MODEL" ]; then
        echo "SKIP: BITNET_MODEL ($BITNET_MODEL) not found"
        exit 0
    fi

    # Build
    echo "--- building bitnetd (BACKEND=native) ---"
    make -C "$SCRIPT_DIR" clean bitnetd \
        BACKEND=native \
        BITNET_C11_DIR="$BITNET_C11_DIR" \
        ${SIMD:+SIMD=$SIMD}

    PORT=$(( 30000 + $$ % 20000 ))
    CONF="$TMPDIR_EP/ep.conf"
    LOG="$TMPDIR_EP/ep.log"

    cat > "$CONF" <<EOF
[daemon]
pidfile = $TMPDIR_EP/ep.pid
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

    TIMEOUT=${ENDPOINT_TEST_TIMEOUT:-30}
    echo "--- starting bitnetd (foreground, port $PORT) ---"
    "$SCRIPT_DIR/bitnetd" -f -c "$CONF" > "$LOG" 2>&1 &
    DAEMON_PID=$!
    STARTED_DAEMON=1

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

    echo "--- bitnetd is ready (took ~${elapsed}s) ---"
    BASE_URL="http://127.0.0.1:$PORT"
fi

echo "--- testing against $BASE_URL ---"

# ── /health ──────────────────────────────────────────────────────
echo "--- GET /health ---"
HEALTH_HTTP=$(curl -s -o "$TMPDIR_EP/health.json" -w "%{http_code}" \
    --max-time 5 "$BASE_URL/health") || true
HEALTH_BODY=$(cat "$TMPDIR_EP/health.json" 2>/dev/null || true)

if [ "$HEALTH_HTTP" = "200" ]; then
    case "$HEALTH_BODY" in
        *'"status":"ok"'*)
            echo "  PASS /health: HTTP 200, body contains \"status\":\"ok\""
            ;;
        *)
            echo "  FAIL /health: HTTP 200 but body missing \"status\":\"ok\": $HEALTH_BODY"
            FAILS=$((FAILS + 1))
            ;;
    esac
else
    echo "  FAIL /health: expected HTTP 200, got $HEALTH_HTTP"
    FAILS=$((FAILS + 1))
fi

# ── /v1/models ───────────────────────────────────────────────────
echo "--- GET /v1/models ---"
MODELS_HTTP=$(curl -s -o "$TMPDIR_EP/models.json" -w "%{http_code}" \
    --max-time 5 "$BASE_URL/v1/models") || true
MODELS_BODY=$(cat "$TMPDIR_EP/models.json" 2>/dev/null || true)

if [ "$MODELS_HTTP" = "200" ]; then
    models_ok=1

    case "$MODELS_BODY" in
        *'"object":"list"'*)
            ;;
        *)
            echo "  FAIL /v1/models: body missing \"object\":\"list\": $MODELS_BODY"
            FAILS=$((FAILS + 1))
            models_ok=0
            ;;
    esac

    case "$MODELS_BODY" in
        *'"object":"model"'*)
            ;;
        *)
            echo "  FAIL /v1/models: body missing \"object\":\"model\": $MODELS_BODY"
            FAILS=$((FAILS + 1))
            models_ok=0
            ;;
    esac

    if [ "$models_ok" -eq 1 ]; then
        echo "  PASS /v1/models: HTTP 200, body contains required fields"
    fi
else
    echo "  FAIL /v1/models: expected HTTP 200, got $MODELS_HTTP"
    FAILS=$((FAILS + 1))
fi

# ── Summary ──────────────────────────────────────────────────────
echo "---"
if [ "$FAILS" -gt 0 ]; then
    echo "FAILED: $FAILS check(s) failed"
    exit 1
fi

echo "All endpoint checks PASSED"
exit 0
