#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

APPDIR="$SCRIPT_DIR/examples/android"

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
