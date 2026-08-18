#!/usr/bin/env bash
# 真机测量 preflight:把踩过的坑收成 1 秒拦截,测量前必跑。
#
#   tools/device_preflight.sh [serial]
#
# serial 缺省取 $ANDROID_SERIAL,再缺省取唯一在线设备(多设备时必须显式给,
# 曾因多设备注错目标)。逐项 PASS/FAIL/WARN,任一 FAIL 退出非零。
#
# 每项检查对应一次真实翻车(docs/issues 与记忆有案):
#   锁屏      —— 锁屏 = 零帧/零请求/黑 screencap,与"app 坏了"完全同相,
#                曾连烧三轮零数据才识破(2026-08-18)。
#   前台包名  —— 注入前必验前台,曾把手势注进 launcher(2026-08-10)。
#   adb reverse—— USB 重连后静默失效 = 矢量全灭假象(2026-08-13)。
#   MVT 服务  —— 本机没起 server 时整场看不到 MVT 症状(2026-08-13)。
#   APK 新鲜度—— stale APK 验了旧代码,strings 验 .so 才发现(2026-08-06)。

set -u

PKG="com.earthengine.minimalglobe"
MVT_PORT=8092     # MVT 瓦片服务(tools/mvt_demo),矢量底图硬依赖
IMG_PORT=8091     # 本地影像/i3dm 服务(编译期 flag 选源,可选依赖)

fail=0
pass() { printf 'PASS  %s\n' "$1"; }
warn() { printf 'WARN  %s\n' "$1"; }
err()  { printf 'FAIL  %s\n' "$1"; fail=1; }

# ---- 设备选择 ----
serial="${1:-${ANDROID_SERIAL:-}}"
devices=$(adb devices | awk 'NR>1 && $2=="device" {print $1}')
count=$(printf '%s\n' "$devices" | sed '/^$/d' | wc -l | tr -d ' ')
if [ "$count" = "0" ]; then
    err "无在线设备(adb devices 空)"
    exit 1
fi
if [ -z "$serial" ]; then
    if [ "$count" != "1" ]; then
        err "多设备在线($count 台)且未指定 serial —— 显式传参,别赌默认路由"
        exit 1
    fi
    serial="$devices"
fi
if ! printf '%s\n' "$devices" | grep -qx "$serial"; then
    err "设备 $serial 不在线(在线:$(echo $devices | tr '\n' ' '))"
    exit 1
fi
A="adb -s $serial"
pass "设备在线 $serial"

# ---- 锁屏 / 亮屏(⚠️⚠️ 锁屏与 app 坏完全同相)----
dreaming=$($A shell dumpsys window 2>/dev/null | grep -o "mDreamingLockscreen=[a-z]*" | head -1)
if printf '%s' "$dreaming" | grep -q "true"; then
    err "设备锁屏($dreaming)—— 零帧/零请求/黑截图全是它,先解锁再测"
else
    pass "未锁屏(${dreaming:-mDreamingLockscreen 未读到,自查})"
fi
awake=$($A shell dumpsys power 2>/dev/null | grep -o "mWakefulness=[A-Za-z]*" | head -1)
case "$awake" in
    *Awake*) pass "屏幕唤醒($awake)" ;;
    "")      warn "读不到 mWakefulness,手动确认亮屏" ;;
    *)       err  "屏幕未唤醒($awake)" ;;
esac

# ---- 前台包名(注入前必验)----
front=$($A shell dumpsys activity activities 2>/dev/null \
        | grep -o "topResumedActivity.*${PKG}[^ }]*" | head -1)
if [ -n "$front" ]; then
    pass "前台是 $PKG"
else
    err "前台不是 $PKG(注入/截图都会打在别的 app 上)"
fi

# ---- adb reverse(USB 重连后静默失效)----
rev=$($A reverse --list 2>/dev/null)
if printf '%s' "$rev" | grep -q "tcp:$MVT_PORT"; then
    pass "reverse tcp:$MVT_PORT 在(MVT)"
else
    err "reverse tcp:$MVT_PORT 缺 —— 矢量全灭假象的头号来源。修:adb -s $serial reverse tcp:$MVT_PORT tcp:$MVT_PORT"
fi
if printf '%s' "$rev" | grep -q "tcp:$IMG_PORT"; then
    pass "reverse tcp:$IMG_PORT 在(本地影像,编译期选源才用)"
else
    warn "reverse tcp:$IMG_PORT 缺(仅本地影像源需要;当前源看 EnvSnap 的日志字段)"
fi

# ---- 本机 MVT 服务 ----
if nc -z 127.0.0.1 $MVT_PORT >/dev/null 2>&1; then
    pass "本机 :$MVT_PORT 有服务在听"
else
    err "本机 :$MVT_PORT 没人听 —— 先起 tools/mvt_demo 的 server(没起过 = 整场看不到 MVT 症状)"
fi

# ---- APK 新鲜度(stale APK 之坑;best-effort,只 WARN)----
inst=$($A shell dumpsys package $PKG 2>/dev/null | grep -o "lastUpdateTime=.*" | head -1 | cut -d= -f2)
if [ -n "$inst" ]; then
    pass "装机时间 $inst(自查是否晚于最后一次构建;strings 验 .so 才是铁证)"
else
    warn "$PKG 未安装或读不到装机时间"
fi

[ "$fail" = "0" ] && printf -- '---\npreflight 全过,可测。\n' \
                  || printf -- '---\npreflight 有 FAIL,先修再测。\n'
exit $fail
