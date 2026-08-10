#!/bin/bash
#
# Full smoke run: private unixODBC registration (nothing touches the user's
# real ~/.odbc.ini), a throwaway Frostlake server unless one is already
# listening, then the assertions in smoke.c through the real driver manager.
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${FROSTLAKE_TEST_PORT:-18095}"
ETC="$HERE/build/test-etc"

mkdir -p "$ETC"
cat > "$ETC/odbcinst.ini" <<EOF
[Frostlake ODBC Driver]
Description = Frostlake ODBC driver under test
Driver      = $HERE/build/libfrostlakeodbc.so
Threading   = 2
EOF
cat > "$ETC/odbc.ini" <<EOF
[FrostlakeTest]
Driver   = Frostlake ODBC Driver
Server   = localhost
Port     = $PORT
EOF

STARTED_PID=""
if ! curl -s "http://localhost:$PORT/api/health" | grep -q healthy; then
    echo "starting a Frostlake server on port $PORT..."
    STARTED_PID="$("$HERE/test/run-server.sh" "$PORT")"
    echo "server pid $STARTED_PID"
fi
cleanup() {
    if [ -n "$STARTED_PID" ]; then
        kill "$STARTED_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

export ODBCSYSINI="$ETC"
export ODBCINI="$ETC/odbc.ini"
export FROSTLAKE_CONN="DSN=FrostlakeTest"

"$HERE/build/smoke"
echo
echo "smoke test passed against localhost:$PORT (DSN + driver manager path)"

# Same server, but calling the driver directly through dlopen.
FROSTLAKE_TEST_PORT="$PORT" "$HERE/build/direct" "$HERE/build/libfrostlakeodbc.so"
echo "direct test passed against localhost:$PORT (no driver manager)"
