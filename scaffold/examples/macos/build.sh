#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SCAFFOLD_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

echo "SCAFFOLD_DIR=$SCAFFOLD_DIR"
source "$SCAFFOLD_DIR/env.sh"

# Set VCPKG_ROOT
if [ -f "$SCAFFOLD_DIR/../../globe/third_party/vcpkg/vcpkg" ]; then
    export VCPKG_ROOT="$SCAFFOLD_DIR/../../globe/third_party/vcpkg"
fi

BUILD_DIR="$SCRIPT_DIR/build"
mkdir -p "$BUILD_DIR"

cmake -B "$BUILD_DIR" \
    -S "$SCRIPT_DIR" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DGLM_INCLUDE_DIR="$GLM_INCLUDE_DIR" \
    "$@"

cmake --build "$BUILD_DIR" --target MinimalGlobe -j "$(sysctl -n hw.logicalcpu)" 2>&1

echo ""
echo "Build complete."
echo "App: $BUILD_DIR/MinimalGlobe.app"
echo "Run: open $BUILD_DIR/MinimalGlobe.app"
