#!/bin/bash
# 构建并安装 Android Release APK(-O2 / RelWithDebInfo native)。
# 从 earth-md/scaffold 目录运行：bash build_and_install.sh
#
# ⚠️ 性能/手感验证必须用 release 变体:debug(assembleDebug)是 -O0,glm 双精度
# 数学不内联,会把 compose/update 性能膨胀 2.5-32×(2026-08-20 真机 A/B:
# compose CPU 4270→131ms/60tick、单任务 46→1.2ms),得出错误的瓶颈画面。
# release 用 debug key 签名仅为本地可安装,严禁分发。

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

cd "$SCRIPT_DIR/examples/android"

echo "=== Cleaning CMake cache ==="
rm -rf app/.cxx earthsdk/.cxx app/build earthsdk/build

echo "=== Building APK ==="
./gradlew assembleRelease --no-daemon --console=plain

APK="app/build/outputs/apk/release/app-release.apk"
if [ ! -f "$APK" ]; then
    echo "ERROR: APK not found at $APK"
    exit 1
fi

echo "=== APK built: $(ls -lh "$APK" | awk '{print $5}') ==="

# Release has an applicationIdSuffix so it can coexist with the debug/base
# install.  Keep install and launch on the same package; launching the base
# package here used to make visual verification run an older APK by mistake.
PACKAGE="com.earthengine.minimalglobe.codexverify"
# The manifest keeps the Java class in the base namespace even when the
# release applicationId gets the .codexverify suffix.
ACTIVITY="$PACKAGE/com.earthengine.minimalglobe.MainActivity"

echo "=== Installing to device ==="
adb install -r "$APK"

echo "=== Launching app ==="
adb shell am force-stop "$PACKAGE"
adb shell am start -n "$ACTIVITY"

echo "=== Done ==="
