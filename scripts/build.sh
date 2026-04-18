#!/usr/bin/env bash
# Convenience wrapper: configure + build.
# Usage: scripts/build.sh [extra cmake flags]
#   NO_SANITIZER=1 scripts/build.sh   — skip ASAN/UBSAN
set -euo pipefail

BUILD=${BUILD:-build}
JOBS=$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)

CMAKE_EXTRA=()
[[ ${NO_SANITIZER:-} ]] && CMAKE_EXTRA+=(-DNO_SANITIZER=ON)

cmake -B "$BUILD" "${CMAKE_EXTRA[@]}" "$@"
cmake --build "$BUILD" -j"$JOBS"
