#!/bin/sh
# Build and run the coral demo (kitty graphics pixel version).
set -e
cd "$(dirname "$0")"

if [ ! -f termpaint/termpaint.c ]; then
    echo "termpaint sources missing — fetching submodule..." >&2
    git submodule update --init termpaint
fi

make
exec ./build/coral-gfx
