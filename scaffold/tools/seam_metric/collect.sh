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
# 掠视姿态用**一次性位姿设定**(keyevent 13 = KEYCODE_6 → nativeGrazingView:
# 重庆上空 6km、下俯 4°),不再靠 DPAD_UP 累积。
#
# ⚠️ 为什么换掉累积按键(2026-08-08):相机约束收口/碰撞升级/动态 near
# (08-03 21:13~21:53,**比本台子的首份基线晚 8 小时**)之后,倾角与下潜都会
# 被钳住,同一串按键到不了同一个姿态。实测 HEAD 上跑完全程近正俯视、画面
# 无地平线,检测器整段误报(15 万像素/帧,比真值大三个数量级),而脚本照常
# 输出一个漂亮的数字。位姿设定是幂等的,不受钳制的路径依赖影响。
# 可用环境变量换姿态(近场 dense↔coarse 档边界用低空那档):
#   POSE_KEY=12 POSE_ALT_M=700 POSE_LOG="Terrain grazing view set" collect.sh ...
POSE_KEY="${POSE_KEY:-13}"
POSE_ALT_M="${POSE_ALT_M:-6000}"   # 该位姿的椭球高,位姿校验的期望值
POSE_LOG="${POSE_LOG:-Grazing horizon view set}"
# ⚠️ 方向(2026-08-08 实测):keyevent 24(VOLUME_UP→1.18)= **内缩**,
# 25(VOLUME_DOWN→0.84)= **外扩**。这与 08-03 首份基线时相反 —— 手势管线
# 重做修掉 dolly 方向双错后翻了过来。别照抄旧脚本的按键顺序。
#
# 步数必须留在净空钳制线以内:6km 起手内缩,理论 0.84^25→75m,实测约第 15 步
# 就被地形净空钳在 ~450m。被钳的步数随地形加载进度浮动,而外扩段步步生效 →
# 往返终点随机(实测 8198 / 50019 / 66552 三种)。12 步 → 6000×0.84^12≈780m,
# 距钳制线仍有余量,往返闭合到 ≈0.95×,轨迹可复现。
ZOOM_STEPS=12
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

    adb shell input keyevent "$POSE_KEY"
    sleep 3
    shot "$RUNDIR/pre.png"

    ( for s in $(seq 1 "$SHOTS"); do
          shot "$RUNDIR/$(printf 's%02d.png' "$s")"
      done ) &
    CAPPID=$!
    for _ in $(seq 1 "$ZOOM_STEPS"); do adb shell input keyevent 24; done   # 内缩
    sleep 6
    for _ in $(seq 1 "$ZOOM_STEPS"); do adb shell input keyevent 25; done   # 外扩
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

    # 位姿校验:本台子曾**静默**失效整整一轮改造期(见上方 POSE_KEY 注释),
    # 跑完照常给数字。三条都从 logcat 真值取,不看按键发没发出去:
    #   ① 位姿设定日志在不在 —— keyevent 到没到 app
    #   ② 起手 camH 落在期望带 —— 位姿真的生效了(而非被拒/被覆盖)
    #   ③ 收尾 camH 回到同一带 —— 缩放往返闭合(不闭合 = 某段被钳住)
    POSE_WHY=""
    if ! grep -q "$POSE_LOG" "$RUNDIR/logcat.log"; then
        POSE_WHY="位姿设定日志缺失(keyevent $POSE_KEY 未到达 app?)"
    else
        # CamPose 每帧一行。必须从**位姿设定那一行之后**切,否则取到的是
        # demo 默认视角(1500m),与期望带不符会误报——切片是这条校验的前提。
        POSE_H=$(sed -n "/$POSE_LOG/,\$p" "$RUNDIR/logcat.log" |
                 grep -o "camH=[0-9.]*" | sed 's/camH=//')
        FIRST_H=$(echo "$POSE_H" | awk '$1>1 {print int($1); exit}')
        LAST_H=$(echo "$POSE_H" | awk '$1>1 {v=int($1)} END{print v}')
        # 带宽取 [0.3×, 3×] 期望值:缩放往返理论闭合到 0.99^25≈0.80×,给足
        # DVFS/加载节奏的余量,但 900km 那种量级(150×)必被拦下。
        LO=$((POSE_ALT_M * 3 / 10)); HI=$((POSE_ALT_M * 3))
        if [ -z "$FIRST_H" ] || [ "$FIRST_H" -lt "$LO" ] || [ "$FIRST_H" -gt "$HI" ]; then
            POSE_WHY="起手 camH=${FIRST_H:-?}m 不在 [$LO,$HI]"
        elif [ "$LAST_H" -lt "$LO" ] || [ "$LAST_H" -gt "$HI" ]; then
            POSE_WHY="收尾 camH=${LAST_H}m 不在 [$LO,$HI](缩放往返未闭合)"
        fi
    fi
    if [ -n "$POSE_WHY" ]; then
        echo "   ⚠️ 位姿校验未过:$POSE_WHY → 剔除"
        echo "$POSE_WHY" > "$RUNDIR/pose-invalid"
    fi
    echo "   $(ls "$RUNDIR"/s*.png 2>/dev/null | wc -l | tr -d ' ') 帧截图"
done

echo "== 完成。下一步:tools/seam_metric/report.py $OUT [另一档] =="
