#!/usr/bin/env bash
# Build and run every test harness. Uses its own build directory so it never
# disturbs the normal app build in build/.
#
#   ./tests/run-tests.sh            # all of them
#   ./tests/run-tests.sh proton env # just these
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD="$ROOT/build-tests"

cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON >/dev/null

# The AppImage target is part of ALL, and packaging is not wanted here.
TARGETS=(proton account addon env ui-context-menu)
make -C "$BUILD" -j"$(nproc)" "${TARGETS[@]}"

echo
if [ $# -gt 0 ]; then
    ctest --test-dir "$BUILD" --output-on-failure -R "$(IFS='|'; echo "$*")"
else
    ctest --test-dir "$BUILD" --output-on-failure
fi
