#!/usr/bin/env bash
# 接缝 A/B 采集器:同一段确定性运动(掠视 + 缩放往返),冷缓存跑 N 轮,
# 逐轮落盘运动期截图序列。判定交给 report.py。
#
# 暂态接缝的峰值随缓存温度/加载节奏剧烈波动(实测同构建冷装 1260 vs 热启
# 6699)——单轮采样不可判 A/B,这正是本脚本存在的原因。协议强制冷缓存
# (卸载重装),多轮取分布。
#
# 用法:
#   tools/seam_metric/collect.sh before 3
#   (改代码,重建 release)
#   tools/seam_metric/collect.sh after 3
#   tools/seam_metric/report.py out/before out/after
#
# ⚠️ 必须 release APK;⚠️ 排查地形时先关矢量 demo 图层(kEnableVectorDemoLayers)。
set -euo pipefail

LABEL="${1:?用法: collect.sh <label> [runs] [outdir]}"
RUNS="${2:-3}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SCAFFOLD_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
OUTROOT="${3:-$SCRIPT_DIR/out}"
OUT="$OUTROOT/$LABEL"

PKG=com.earthengine.minimalglobe
ACTIVITY="$PKG/.MainActivity"
APK="$SCAFFOLD_DIR/examples/android/app/build/outputs/apk/release/app-release.apk"

SETTLE_S=22      # 初始视图收敛
TILT_STEPS=5     # DPAD_UP(19)×5 → 掠视(接缝最易暴露的姿态)。
                 # ⚠️ 方向:demo GLESView 里 DPAD_UP=tilt -80(压向掠视),
                 # DPAD_DOWN=tilt +80(抬回俯视)。写反 = 全程正俯视,
                 # 无地平线,检测器整段误报(踩过:steady 39 万全是假)。
ZOOM_STEPS=25    # 音量键缩放往返幅度
SHOTS=40         # 运动期截图数(与缩放并行采,覆盖整个暂态窗)

# screencap 偶发悬挂(实测一次悬死 10+ 分钟拖垮整轮)——所有截图走 10s 超时
# + 一次重试;两次都挂记 0 字节文件,report 侧按坏帧跳过。
shot() {
    local out="$1"
    for _try in 1 2; do
        ( adb exec-out screencap -p > "$out" ) &
        local pid=$!
        local waited=0
        while kill -0 "$pid" 2>/dev/null && [ "$waited" -lt 100 ]; do
            sleep 0.1
            waited=$((waited + 1))
        done
        if ! kill -0 "$pid" 2>/dev/null; then
            wait "$pid" 2>/dev/null || true
            [ -s "$out" ] && return 0
        else
            kill "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
    : > "$out"
    return 0
}

if [ ! -f "$APK" ]; then
    echo "ERROR: 找不到 release APK:$APK" >&2
    exit 1
fi

mkdir -p "$OUT"
echo "== 采集档位 '$LABEL':$RUNS 轮,输出 $OUT =="
echo "   APK md5=$(md5 -q "$APK" 2>/dev/null || md5sum "$APK" | cut -d' ' -f1)"

# 续号(判 INCONC 后补样本,同 load_ab)
BASE=0
for existing in "$OUT"/run*; do
    [ -d "$existing" ] || continue
    n="$(basename "$existing")"; n="${n#run}"
    [ "$n" -gt "$BASE" ] 2>/dev/null && BASE="$n"
done
[ "$BASE" -gt 0 ] && echo "   已有 $BASE 轮,续号自 run$((BASE + 1))"

for k in $(seq 1 "$RUNS"); do
    i=$((BASE + k))
    RUNDIR="$OUT/run$i"
    mkdir -p "$RUNDIR"
    echo "-- run $k/$RUNS (run$i)"

    adb uninstall "$PKG" >/dev/null 2>&1 || true
    adb install "$APK" >/dev/null
    adb logcat -c
    adb logcat -v time > "$RUNDIR/logcat.log" 2>&1 &
    LOGPID=$!
    sleep 1
    adb shell am start -n "$ACTIVITY" >/dev/null
    sleep "$SETTLE_S"

    for _ in $(seq 1 "$TILT_STEPS"); do adb shell input keyevent 19; done
    sleep 3
    shot "$RUNDIR/pre.png"

    ( for s in $(seq 1 "$SHOTS"); do
          shot "$RUNDIR/$(printf 's%02d.png' "$s")"
      done ) &
    CAPPID=$!
    for _ in $(seq 1 "$ZOOM_STEPS"); do adb shell input keyevent 25; done
    sleep 6
    for _ in $(seq 1 "$ZOOM_STEPS"); do adb shell input keyevent 24; done
    wait "$CAPPID" 2>/dev/null || true
    sleep 6
    shot "$RUNDIR/post.png"

    kill "$LOGPID" 2>/dev/null || true
    wait "$LOGPID" 2>/dev/null || true

    # 污染检测(同 load_ab):脚本只发 keyevent,GESTDIAG dragStart 出现 =
    # 真人碰了手机 → 该轮相机轨迹不可比,标记剔除。
    if grep -q "GESTDIAG.*dragStart" "$RUNDIR/logcat.log"; then
        echo "   ⚠️ 检测到真人触摸 → 剔除"
        touch "$RUNDIR/contaminated"
    fi
    echo "   $(ls "$RUNDIR"/s*.png 2>/dev/null | wc -l | tr -d ' ') 帧截图"
done

echo "== 完成。下一步:tools/seam_metric/report.py $OUT [另一档] =="
