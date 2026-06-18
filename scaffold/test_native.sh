#!/usr/bin/env bash
# Configure, build, and run native unit tests with the repo-local environment.
#
# Usage:
#   ./test_native.sh                 # run all native tests
#   ./test_native.sh test_rectangle  # build and run one test executable
#   ./test_native.sh --ctest -R Rectangle

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

PRESET="${GIS_MD_NATIVE_TEST_PRESET:-native-tests}"
BUILD_DIR="$SCRIPT_DIR/build/$PRESET"

if ! command -v cmake >/dev/null 2>&1; then
    echo "ERROR: cmake not found after sourcing $SCRIPT_DIR/env.sh" >&2
    exit 127
fi

if ! command -v ninja >/dev/null 2>&1; then
    echo "ERROR: ninja not found after sourcing $SCRIPT_DIR/env.sh" >&2
    exit 127
fi

cd "$SCRIPT_DIR"

cmake --preset "$PRESET"

if [ "${1:-}" = "--ctest" ]; then
    shift
    cmake --build --preset "$PRESET"
    ctest --preset "$PRESET" "$@"
elif [ "$#" -gt 0 ]; then
    target="$1"
    shift
    cmake --build --preset "$PRESET" --target "$target"
    if [ -x "$BUILD_DIR/$target" ]; then
        "$BUILD_DIR/$target" "$@"
    elif [ -x "$BUILD_DIR/tests/$target" ]; then
        "$BUILD_DIR/tests/$target" "$@"
    else
        echo "ERROR: built test executable not found for target '$target'" >&2
        echo "Checked: $BUILD_DIR/$target and $BUILD_DIR/tests/$target" >&2
        exit 1
    fi
else
    cmake --build --preset "$PRESET"
    ctest --preset "$PRESET"
fi
