#!/usr/bin/env bash
# 取 Khronos glslang 预编译二进制(scaffold/build/tools/glslang),供
# check_glsl_compile.py 的 host GLSL 编译守卫使用(T-P6 方案 B)。
#
# 用法: ./tools/fetch_glslang.sh [version]
# 默认 16.5.0;产物落在 scaffold/build/tools/glslang(不进 git)。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_TOOLS="$SCRIPT_DIR/../build/tools"
VERSION="${1:-16.5.0}"
DEST="$BUILD_TOOLS/glslang"

case "$(uname -s)-$(uname -m)" in
    Darwin-arm64|Darwin-x86_64)
        ASSET="glslang-${VERSION}-macos-universal-release.tar.gz" ;;
    Linux-x86_64)
        ASSET="glslang-${VERSION}-linux-x86_64-release.tar.gz" ;;
    *)
        echo "unsupported platform: $(uname -s)-$(uname -m)" >&2
        exit 1 ;;
esac

URL="https://github.com/KhronosGroup/glslang/releases/download/${VERSION}/${ASSET}"

if [ -x "$DEST/bin/glslangValidator" ] && \
   [ "$(cat "$DEST/VERSION" 2>/dev/null || true)" = "$VERSION" ]; then
    echo "glslang ${VERSION} already present: $DEST"
    exit 0
fi

mkdir -p "$BUILD_TOOLS"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "downloading ${URL}"
curl -fL --retry 3 -o "$TMP/glslang.tar.gz" "$URL"
tar xzf "$TMP/glslang.tar.gz" -C "$TMP"

rm -rf "$DEST"
mkdir -p "$DEST"
cp -R "$TMP"/bin "$DEST"/bin
printf '%s\n' "$VERSION" > "$DEST/VERSION"
echo "installed glslang ${VERSION} -> $DEST"
