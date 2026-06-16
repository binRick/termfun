#!/bin/sh
# Build and run the extracer process-exec tracer (cell rendering).
set -e
cd "$(dirname "$0")"

if [ ! -f termpaint/termpaint.c ]; then
    echo "termpaint sources missing — fetching submodule..." >&2
    git submodule update --init termpaint
fi

# Build just this target: extracer is the Linux-focused app, and `make` (all)
# also tries to build the macOS-only demos (e.g. sysmon), which don't compile
# on Linux.
make build/extracer
exec ./build/extracer "$@"
