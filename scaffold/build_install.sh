#!/bin/bash
set -e
export JAVA_HOME=/opt/homebrew/opt/openjdk@17
export PATH="$JAVA_HOME/bin:$PATH"
export ANDROID_HOME=/opt/homebrew/share/android-commandlinetools
export ANDROID_NDK_HOME=/opt/homebrew/share/android-commandlinetools/ndk/28.2.13676358

APPDIR=/Users/ldy/Desktop/work/earth-md/scaffold/examples/android

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
