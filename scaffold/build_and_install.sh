#!/bin/bash
# 构建并安装 Android Debug APK
# 从 earth-md/scaffold 目录运行：bash build_and_install.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

cd "$SCRIPT_DIR/examples/android"

echo "=== Cleaning CMake cache ==="
rm -rf app/.cxx earthsdk/.cxx

echo "=== Building APK ==="
./gradlew assembleDebug --no-daemon --console=plain

APK="app/build/outputs/apk/debug/app-debug.apk"
if [ ! -f "$APK" ]; then
    echo "ERROR: APK not found at $APK"
    exit 1
fi

echo "=== APK built: $(ls -lh "$APK" | awk '{print $5}') ==="

echo "=== Installing to device ==="
adb install -r "$APK"

echo "=== Launching app ==="
adb shell am start -n com.earthengine.minimalglobe/.MainActivity

echo "=== Done ==="
