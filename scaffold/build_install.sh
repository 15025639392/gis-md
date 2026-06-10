#!/bin/bash
set -e
export ANDROID_HOME="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
export ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-$ANDROID_HOME}"
export ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-$ANDROID_HOME/ndk/28.2.13676358}"
export JAVA_HOME="${JAVA_HOME:-$HOME/development/jdks/zulu17.64.17-ca-jdk17.0.18-macosx_aarch64/zulu-17.jdk/Contents/Home}"
export PATH="$JAVA_HOME/bin:$ANDROID_HOME/platform-tools:$PATH"

APPDIR="$(cd "$(dirname "$0")/examples/android" && pwd)"

# clean cmake cache
rm -rf "$APPDIR/app/.cxx"

# build
"$APPDIR/gradlew" -p "$APPDIR" assembleDebug --no-daemon --console=plain

# install
APK="$APPDIR/app/build/outputs/apk/debug/app-debug.apk"
adb install -r "$APK"
echo "INSTALLED"

# launch
adb shell am start -n com.earthengine.minimalglobe/.MainActivity
echo "LAUNCHED"
