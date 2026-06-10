#!/bin/bash
# 构建并安装 Android Debug APK
# 从 earth-md/scaffold 目录运行：bash build_and_install.sh

set -e

export ANDROID_HOME="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
export ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-$ANDROID_HOME}"
export ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-$ANDROID_HOME/ndk/28.2.13676358}"
export JAVA_HOME="${JAVA_HOME:-$HOME/development/jdks/zulu17.64.17-ca-jdk17.0.18-macosx_aarch64/zulu-17.jdk/Contents/Home}"
export PATH="$JAVA_HOME/bin:$ANDROID_HOME/platform-tools:$PATH"

cd "$(dirname "$0")/examples/android"

echo "=== Cleaning CMake cache ==="
rm -rf app/.cxx

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
