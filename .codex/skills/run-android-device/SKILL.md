---
name: run-android-device
description: Build, install, launch, and verify this gis-md project's Android MinimalGlobe app on a connected physical Android device. Use when the user asks to run the current project on Android, deploy to the connected Android phone, fix or validate the Android run environment, or explain the exact build/install steps for scaffold/examples/android.
---

# Run Android Device

## Workflow

Use the project scripts first. They encode the local JDK, Android SDK, NDK, Gradle, CMake, install, and launch sequence for this repository.

From the repository root:

```bash
cd /Users/ldy/Desktop/work/gis-md
bash scaffold/build_and_install.sh
```

This script should:

1. Export the local Android and Java environment.
2. Enter `scaffold/examples/android`.
3. Remove `app/.cxx` to force a clean native configure.
4. Run `./gradlew assembleDebug --no-daemon --console=plain`.
5. Install `app/build/outputs/apk/debug/app-debug.apk`.
6. Launch `com.earthengine.minimalglobe/.MainActivity`.

## Expected Environment

The project scripts default to these local paths, while still allowing callers to override env vars:

```bash
ANDROID_HOME=$HOME/Library/Android/sdk
ANDROID_SDK_ROOT=$ANDROID_HOME
ANDROID_NDK_HOME=$ANDROID_HOME/ndk/28.2.13676358
JAVA_HOME=$HOME/development/jdks/zulu17.64.17-ca-jdk17.0.18-macosx_aarch64/zulu-17.jdk/Contents/Home
PATH=$JAVA_HOME/bin:$ANDROID_HOME/platform-tools:$PATH
```

`scaffold/examples/android/local.properties` should point to:

```properties
sdk.dir=/Users/ldy/Library/Android/sdk
```

## Verify Device And Launch

Before running, confirm that exactly the intended physical device is connected:

```bash
adb devices -l
```

After running, verify the app is foregrounded:

```bash
adb shell dumpsys activity activities | rg 'topResumedActivity|mResumedActivity|com\.earthengine\.minimalglobe'
```

Success includes:

```text
topResumedActivity=... com.earthengine.minimalglobe/.MainActivity
```

## Common Failures

If Gradle reports an invalid `JAVA_HOME`, check that `JAVA_HOME/bin/java` exists and prints JDK 17:

```bash
"$JAVA_HOME/bin/java" -version
```

If Android SDK paths fail, inspect:

```bash
ls -la "$ANDROID_HOME"
ls -la "$ANDROID_NDK_HOME"
```

If install fails with `INSTALL_FAILED_UPDATE_INCOMPATIBLE`, the device has the same package signed by another key. Ask only if preserving app data matters; otherwise uninstall and retry:

```bash
adb uninstall com.earthengine.minimalglobe
bash scaffold/build_and_install.sh
```

If the user asks only for the steps, explain the script sequence above instead of running it.
