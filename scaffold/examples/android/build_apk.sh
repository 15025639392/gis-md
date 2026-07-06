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

# 默认 debug(-O0)。传 "release" 构建优化(-O2)变体——**性能测量必须用它**,
# debug 会把 selector/Tileset.update 膨胀 ~2.5-3×(glm 双精度未内联)。
# 用法:./build_apk.sh            → assembleDebug
#       ./build_apk.sh release    → assembleRelease(debug 签名,仅本地测性能)
BUILD_VARIANT="${1:-debug}"
if [ "$BUILD_VARIANT" = "release" ]; then
    shift
    echo "构建 RELEASE(-O2,仅供本地性能测量,勿分发)"
    ./gradlew assembleRelease "$@"
else
    ./gradlew assembleDebug "$@"
fi
