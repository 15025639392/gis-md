#!/bin/bash
# Pixel_7_API_35 Android 模拟器管理脚本(带窗口,可在屏幕上看到)。
# 用法: ./run_emulator.sh [start|stop|restart]   (默认 start)
set -e
ANDROID_HOME="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
PATH="$ANDROID_HOME/platform-tools:$ANDROID_HOME/emulator:$PATH"
AVD="Pixel_7_API_35"
cmd="${1:-start}"
case "$cmd" in
  start)
    pkill -9 -f "qemu-system.*$AVD" 2>/dev/null || true
    rm -rf "$HOME/.android/avd/$AVD.avd/"*.lock 2>/dev/null || true
    rm -rf "$HOME/.android/avd/$AVD.avd/snapshots" 2>/dev/null || true
    nohup emulator -avd "$AVD" -no-snapshot -no-audio -gpu swiftshader_indirect >/tmp/emulator_$AVD.log 2>&1 &
    echo "emulator starting (windowed). log: /tmp/emulator_$AVD.log"
    adb wait-for-device
    for i in $(seq 1 120); do
      b=$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')
      [ "$b" = "1" ] && { echo "BOOTED"; break; }
      sleep 3
    done
    adb root >/dev/null 2>&1 || true
    adb wait-for-device
    adb shell 'ip route add default via 10.0.2.2 dev eth0 2>/dev/null || true; setprop net.dns1 8.8.8.8 2>/dev/null || true; setprop net.dns2 114.114.114.114 2>/dev/null || true' || true
    adb devices
    ;;
  stop)
    adb -s emulator-5554 emu kill 2>/dev/null || pkill -9 -f "qemu-system.*$AVD" || true
    echo "stopped"
    ;;
  restart)
    "$0" stop; sleep 3; "$0" start
    ;;
  *)
    echo "usage: $0 [start|stop|restart]"; exit 1;;
esac
