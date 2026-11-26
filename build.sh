#!/usr/bin/env bash
set -euo pipefail

mkdir -p build
CC="${CC:-gcc}"
STATIC_FLAG=""
if [ "${1:-}" = "--static" ]; then
    STATIC_FLAG="-static"
fi

echo "Compiling ElliottDB (Linux binary build/elliottdb)..."
"$CC" -std=c11 -O3 -Wall -Wextra -D_GNU_SOURCE -pthread -Isrc $STATIC_FLAG \
    -o build/elliottdb \
    src/edb_util.c \
    src/pal_linux.c \
    src/config.c \
    src/metrics.c \
    src/pager.c \
    src/wal.c \
    src/index.c \
    src/storage.c \
    src/sql_parser.c \
    src/sql_exec.c \
    src/protocol.c \
    src/reactor_linux.c \
    src/server.c \
    src/cli.c \
    src/bench.c \
    src/main.c \
    -pthread -lm

strip --strip-unneeded build/elliottdb 2>/dev/null || true
echo "BUILD SUCCEEDED: build/elliottdb"
