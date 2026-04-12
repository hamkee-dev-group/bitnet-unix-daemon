# tests/common.sh — shared helpers for bitnetd integration tests.
#
# Source this file from a test script:
#   . "$(dirname "$0")/common.sh"
#
# Provides:
#   REPO_DIR        — absolute path to the repository root
#   TEST_TMPDIR     — per-test temp directory (cleaned up on EXIT)
#   TEST_PORT       — ephemeral port for this test run
#   TEST_CONF       — path to the generated config file
#   TEST_LOG        — path to the daemon log file
#   TEST_PID        — path to the daemon pid file
#   bitnetd_mkconf  — generate a config from the .test.in template

# Resolve repo root relative to this file (works when sourced).
# POSIX-compatible: if BASH_SOURCE is available use it, else fall
# back to the caller setting REPO_DIR before sourcing.
if [ -n "${BASH_SOURCE:-}" ]; then
    REPO_DIR="$(cd "$(dirname "$BASH_SOURCE")/.." && pwd)"
else
    # POSIX sh: $0 is the caller, so go up one level from the test dir.
    REPO_DIR="${REPO_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
fi

# ── Temp directory with automatic cleanup ────────────────────────
TEST_TMPDIR="$(mktemp -d)"
_common_cleanup() {
    rm -rf "$TEST_TMPDIR"
}
trap _common_cleanup EXIT

# ── Ephemeral port ───────────────────────────────────────────────
TEST_PORT=$(( 30000 + $$ % 20000 ))

# ── Default paths inside the temp dir ────────────────────────────
TEST_CONF="$TEST_TMPDIR/test.conf"
TEST_LOG="$TEST_TMPDIR/test.log"
TEST_PID="$TEST_TMPDIR/test.pid"

# bitnetd_mkconf [model_path]
#
# Substitutes tokens in conf/bitnetd.conf.test.in and writes the
# result to $TEST_CONF.  Uses $BITNET_MODEL or the argument as the
# model path.  Callers may override TEST_PORT, TEST_PID, TEST_LOG
# before calling.
bitnetd_mkconf() {
    _model="${1:-${BITNET_MODEL:?BITNET_MODEL must be set}}"
    _template="$REPO_DIR/conf/bitnetd.conf.test.in"

    if [ ! -f "$_template" ]; then
        echo "ERROR: template not found: $_template" >&2
        return 1
    fi

    sed \
        -e "s|@@MODEL_PATH@@|${_model}|g" \
        -e "s|@@LISTEN_PORT@@|${TEST_PORT}|g" \
        -e "s|@@PIDFILE@@|${TEST_PID}|g" \
        -e "s|@@LOGFILE@@|${TEST_LOG}|g" \
        "$_template" > "$TEST_CONF"
}
