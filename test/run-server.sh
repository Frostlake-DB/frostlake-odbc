#!/bin/bash
#
# Start a Frostlake HTTP server for the ODBC smoke test. Uses the engine jar
# from the local Maven repository (or FROSTLAKE_JAR / FROSTLAKE_CP overrides).
#
#   test/run-server.sh [PORT]     default 18095; prints the PID, waits until healthy
#
set -euo pipefail

PORT="${1:-18095}"

if [ -n "${FROSTLAKE_CP:-}" ]; then
    CP="$FROSTLAKE_CP"
else
    JAR="${FROSTLAKE_JAR:-$(ls -1 "$HOME"/.m2/repository/dev/frostlake/frostlake-db/*/frostlake-db-*.jar 2>/dev/null \
        | grep -v -- '-sources\|-javadoc\|-tests' | sort -V | tail -1)}"
    [ -n "$JAR" ] || { echo "no frostlake-db jar found — set FROSTLAKE_JAR or FROSTLAKE_CP"; exit 1; }
    # The published jar is thin: resolve its runtime deps from the local repo,
    # newest version of each — globbing all versions can put two slf4j
    # generations on the classpath at once.
    M2="$HOME/.m2/repository"
    newest() {
        ls -1 "$M2"/$1 2>/dev/null | grep -v -- '-sources\|-javadoc' | sort -V | tail -1
    }
    DEPS=""
    for pattern in \
        'io/airlift/aircompressor-v3/*/aircompressor-v3-*.jar' \
        'org/antlr/antlr4-runtime/*/antlr4-runtime-*.jar' \
        'tools/jackson/core/jackson-databind/*/jackson-databind-*.jar' \
        'tools/jackson/core/jackson-core/*/jackson-core-*.jar' \
        'com/fasterxml/jackson/core/jackson-annotations/*/jackson-annotations-*.jar' \
        'org/slf4j/slf4j-api/*/slf4j-api-*.jar' \
        'org/slf4j/slf4j-simple/*/slf4j-simple-*.jar' \
        'org/jline/jline/*/jline-*.jar' \
        'org/graalvm/polyglot/polyglot/*/polyglot-*.jar' \
        'org/graalvm/sdk/collections/*/collections-*.jar' \
        'org/graalvm/sdk/nativeimage/*/nativeimage-*.jar' \
        'org/graalvm/sdk/word/*/word-*.jar'; do
        jar_path="$(newest "$pattern")"
        [ -n "$jar_path" ] && DEPS="$DEPS$jar_path:"
    done
    CP="$JAR:$DEPS"
fi

JAVA="${JAVA_HOME:-/usr}/bin/java"
LOG="${TMPDIR:-/tmp}/frostlake-odbc-server-$PORT.log"

"$JAVA" -cp "$CP" dev.frostlake.http.DatabaseHttpServer "$PORT" > "$LOG" 2>&1 &
PID=$!

for _ in $(seq 1 50); do
    if curl -s "http://localhost:$PORT/api/health" | grep -q healthy; then
        echo "$PID"
        exit 0
    fi
    kill -0 "$PID" 2>/dev/null || { echo "server died — log: $LOG" >&2; exit 1; }
    sleep 0.2
done
echo "server did not become healthy — log: $LOG" >&2
kill "$PID" 2>/dev/null || true
exit 1
