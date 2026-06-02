#!/bin/bash
export ANDROID_HOME=/opt/homebrew/share/android-commandlinetools
export ANDROID_NDK_HOME=/opt/homebrew/share/android-commandlinetools/ndk/28.2.13676358
cd /Users/ldy/Desktop/work/earth-md/scaffold/examples/android
./gradlew assembleDebug --no-daemon 2>&1
