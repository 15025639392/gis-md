#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SCAFFOLD_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
echo "SCRIPT_DIR=$SCRIPT_DIR"
echo "SCAFFOLD_DIR=$SCAFFOLD_DIR"

source "$SCAFFOLD_DIR/env.sh"

GIS_MD_DIR="$(cd "$SCAFFOLD_DIR/.." && pwd)"
if [ -f "$GIS_MD_DIR/../globe/third_party/vcpkg/vcpkg" ]; then
    export VCPKG_ROOT="$GIS_MD_DIR/../globe/third_party/vcpkg"
    echo "VCPKG_ROOT=$VCPKG_ROOT"
fi

cd "$SCRIPT_DIR"
./gradlew assembleDebug "$@"
