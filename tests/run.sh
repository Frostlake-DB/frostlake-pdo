#!/usr/bin/env bash
# Copyright 2026 MLorek
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Runs a test script through the built extension against a throwaway Frostlake server.
#
#   tests/run.sh [script.php [args...]]      default: tests/pdo_frostlake_test.php
#
# The server is the engine jar from the local Maven repository, FROSTLAKE_VERSION (default
# 0.1.0) with the dependencies that release declares, or FROSTLAKE_CLASSPATH when set. With
# FROSTLAKE_URL set, no server is started and the script runs against that one.
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
PHP="${PHP:-php}"
EXTENSION="$HERE/build/pdo_frostlake.so"
SCRIPT="${1:-$HERE/tests/pdo_frostlake_test.php}"
shift || true

[ -f "$EXTENSION" ] || { echo "build the extension first: make" >&2; exit 1; }

run_script() {
    "$PHP" -d "extension=$EXTENSION" "$SCRIPT" "$@"
}

if [ -n "${FROSTLAKE_URL:-}" ]; then
    rest="${FROSTLAKE_URL#*://}"
    rest="${rest%%/*}"
    export FROSTLAKE_HOST="${rest%%:*}"
    export FROSTLAKE_PORT="${rest##*:}"
    run_script "$@"
    exit $?
fi

engine_classpath() {
    if [ -n "${FROSTLAKE_CLASSPATH:-}" ]; then
        printf '%s\n' "$FROSTLAKE_CLASSPATH"
        return
    fi
    local version="${FROSTLAKE_VERSION:-0.1.0}"
    local m2="${M2_REPO:-$HOME/.m2/repository}"
    local classpath="" jar
    # The engine and what its pom declares, with the two jars databind brings along.
    for jar in \
        "dev/frostlake/frostlake-db/$version/frostlake-db-$version.jar" \
        tools/jackson/core/jackson-databind/3.2.1/jackson-databind-3.2.1.jar \
        tools/jackson/core/jackson-core/3.2.1/jackson-core-3.2.1.jar \
        com/fasterxml/jackson/core/jackson-annotations/2.22/jackson-annotations-2.22.jar \
        org/antlr/antlr4-runtime/4.13.2/antlr4-runtime-4.13.2.jar \
        org/slf4j/slf4j-api/2.0.17/slf4j-api-2.0.17.jar \
        org/slf4j/slf4j-simple/2.0.17/slf4j-simple-2.0.17.jar \
        io/airlift/aircompressor/2.0.3/aircompressor-2.0.3.jar \
        org/jline/jline/3.30.9/jline-3.30.9.jar \
        org/graalvm/polyglot/polyglot/25.0.2/polyglot-25.0.2.jar; do
        [ -f "$m2/$jar" ] || { echo "missing $m2/$jar — set FROSTLAKE_CLASSPATH" >&2; return 1; }
        classpath="${classpath:+$classpath:}$m2/$jar"
    done
    printf '%s\n' "$classpath"
}

CLASSPATH_FOR_ENGINE="$(engine_classpath)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/frostlake-pdo-test.XXXXXX")"
PORT="$("$PHP" -r '$s = stream_socket_server("tcp://127.0.0.1:0"); echo explode(":", stream_socket_get_name($s, false))[1];')"
LOG="$WORK/engine.log"
JAVA="${JAVA_HOME:+$JAVA_HOME/bin/}java"

# A private user.home keeps this server's stage files apart from any other engine on the machine;
# without nodelay a keep-alive client waits on every statement.
"$JAVA" "-Duser.home=$WORK/home" -Dsun.net.httpserver.nodelay=true -cp "$CLASSPATH_FOR_ENGINE" \
    dev.frostlake.http.DatabaseHttpServer "$PORT" > "$LOG" 2>&1 &
ENGINE_PID=$!

cleanup() {
    kill "$ENGINE_PID" 2>/dev/null || true
    wait "$ENGINE_PID" 2>/dev/null || true
    if [ -n "${KEEP_ENGINE_LOG:-}" ]; then
        echo "engine log: $LOG" >&2
    else
        rm -rf "$WORK"
    fi
}
trap cleanup EXIT

for _ in $(seq 1 150); do
    if "$PHP" -r 'exit(@file_get_contents($argv[1]) === false ? 1 : 0);' "http://127.0.0.1:$PORT/api/health"; then
        break
    fi
    kill -0 "$ENGINE_PID" 2>/dev/null || { echo "the engine exited during startup:" >&2; cat "$LOG" >&2; exit 1; }
    sleep 0.2
done

export FROSTLAKE_HOST=127.0.0.1
export FROSTLAKE_PORT="$PORT"
export FROSTLAKE_ENGINE_LOG="$LOG"
run_script "$@"
