#!/bin/bash

# Q Launcher
# Quick launcher for the Q R IDE

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
EXECUTABLE="${SCRIPT_DIR}/build/bin/q"

if [ ! -f "$EXECUTABLE" ]; then
    echo "R GUI 2 not built yet!"
    echo "Run: ./build.sh"
    exit 1
fi

echo "Starting Q..."
"$EXECUTABLE" "$@"
