#!/usr/bin/env bash
# 加载期 A/B 采集器：同一段确定性输入，冷缓存跑 N 轮，逐轮落盘 logcat。
#
# 判定归机器不归肉眼 —— 本脚本只负责"把两档的原始序列采干净"，
# 指标与结论交给 report.py。两档的唯一差异必须是 APK，输入逐字相同。
#
# 用法：
#   tools/load_ab/collect.sh before            # 采基线（当前已构建的 release APK）
#   tools/load_ab/collect.sh after  5          # 改完再采一档
#   tools/load_ab/report.py out/before out/after
#
# ⚠️ 必须用 release APK（examples/android/build_apk.sh release）。
#    debug 是 -O0，帧时数字全是幻觉，A/B 结论无效。
set -euo pipefail

LABEL="${1:?用法: collect.sh <label> [runs] [outdir]}"
RUNS="${2:-5}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SCAFFOLD_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
OUTROOT="${3:-$SCAFFOLD_DIR/tools/load_ab/out}"
OUT="$OUTROOT/$LABEL"

PKG=com.earthengine.minimalglobe
ACTIVITY="$PKG/.MainActivity"
APK="$SCAFFOLD_DIR/examples/android/app/build/outputs/apk/release/app-release.apk"

# 时序参数。SETTLE 要足够让初始视图收敛到 pendUp=0（实测 <20s）；
# DRAIN 要足够长于最坏排空（基线实测 2.0s，留足余量）。
SETTLE_S=20
DRAIN_S=20
ZOOM_STEPS=14   # 音量下 = nativeDebugZoom(0.84)，14 步 ≈ 1500m → 万米量级

if [ ! -f "$APK" ]; then
    echo "ERROR: 找不到 release APK：$APK" >&2
    echo "       先跑 examples/android/build_apk.sh release" >&2
    exit 1
fi

mkdir -p "$OUT"
echo "== 采集档位 '$LABEL'：$RUNS 轮，输出 $OUT =="
echo "   APK $(ls -lh "$APK" | awk '{print $5}')  md5=$(md5 -q "$APK" 2>/dev/null || md5sum "$APK" | cut -d' ' -f1)"

for i in $(seq 1 "$RUNS"); do
    LOG="$OUT/run$i.log"
    echo "-- run $i/$RUNS"

    # 冷缓存：pm clear 在本机被 SecurityException 拒，只能卸载重装。
    adb uninstall "$PKG" >/dev/null 2>&1 || true
    adb install "$APK" >/dev/null
    adb logcat -c

    adb logcat -v time > "$LOG" 2>&1 &
    LOGPID=$!
    sleep 1
    adb shell am start -n "$ACTIVITY" >/dev/null

    sleep "$SETTLE_S"
    for _ in $(seq 1 "$ZOOM_STEPS"); do adb shell input keyevent 25; done
    sleep "$DRAIN_S"

    kill "$LOGPID" 2>/dev/null || true
    wait "$LOGPID" 2>/dev/null || true

    # 污染检测：脚本只发 keyevent，不产生 GESTDIAG 拖动。出现 = 真人碰了手机，
    # 该轮相机轨迹与别轮不同，工作量不可比 → report.py 会剔除。
    if grep -q "GESTDIAG.*dragStart" "$LOG"; then
        echo "   ⚠️  检测到真人触摸（GESTDIAG dragStart）→ 本轮将被剔除"
        touch "$OUT/run$i.contaminated"
    fi
    echo "   $(grep -c LoadGate "$LOG" || true) 条 LoadGate，$(du -h "$LOG" | cut -f1)"
done

echo "== 完成。下一步：tools/load_ab/report.py $OUT [另一档目录] =="
