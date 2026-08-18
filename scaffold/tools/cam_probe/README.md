# cam_probe — 手势锚点精度真机闭环

把手势 pin 正确性从"观感"变"机制自证":注入合成手势 → 引擎吐 `CAMPROBE`
→ 主机侧算逐帧 anchorErr / 本该vs实际位移 / 增益。

## 三段链路

1. **注入**(免 root 多点触控台,见 `examples/android/injector/`):
   `UiAutomation.injectInputEvent` 合成 2 指 MotionEvent 打到前台 MinimalGlobe。
   单指 pan/fling 直接用 `adb shell input swipe`。

2. **引擎吐数**(`FreeGlobeController::logCameraProbe`,tag=`CAMPROBE`):
   手势 START/每 MOVE/END 各一行,含 `finger` 像素 + `viewport` + `eyeAlt`
   (相机椭球高,m,近碰撞压测用)+ 当前锚点世界坐标 `anchor`(ECEF)+
   `vpm`(view·proj 矩阵,16 doubles 列主序)。
   纯诊断,零行为改动。**debug 变体才吐**(Oplus 吞非 debuggable 应用 logcat)。

3. **读出**(`camprobe.py`):用 `vpm` 把 `anchor` 投影到像素 = **实际落点**,
   对比注入的 `finger` = **本该落点** → anchorErr(px)。按手势聚合峰值/增益。

## 用法

```bash
# 单指 drag,看逐帧曲线
adb logcat -c
adb shell input swipe 620 1400 620 950 700
adb logcat -d -s CAMPROBE | python3 tools/cam_probe/camprobe.py --curve

# 双指 pinch/rotate/tilt 经注入台(见 injector 模块 README)
adb shell am instrument -w -e class com.earthengine.injector.GestureInjector#twoFinger \
  -e ax0 620 -e ay0 1100 -e bx0 620 -e by0 1600 \
  -e ax1 620 -e ay1 800  -e bx1 620 -e by1 1900 -e steps 24 -e dur 900 \
  com.earthengine.injector.test/androidx.test.runner.AndroidJUnitRunner
adb logcat -d -s CAMPROBE | python3 tools/cam_probe/camprobe.py   # 聚合峰值
```

聚合行示例:
```
[drag]  帧=43  anchorErr: 末=0.01px 峰=0.06px(@dragMove)  本该位移=445.5px 实际位移=445.5px 增益=1.000 最低eyeAlt=1200.0m
```

近碰撞压测实测:高俯仰贴底(eyeAlt≈350m)时 anchorErr 峰 ~0.4px(基线 0.06px),
= constrainEye 的径向抬升 fallback 发火(gain<0.2 时保锚退化)。亚像素、自限,非 bug。

## 判据

- **anchorErr**(px):被钉世界点当前投影离手指几像素。理想 0。pin 正确性核心判据。
- **增益**:实际位移/本该位移(仅 drag 等质心有位移的手势有意义;纯缩放/对称旋转质心不动 → nan)。
- ⚠️ **tilt 无几何 ground truth**:俯仰是"手指px→多少度 pitch"的灵敏度设计,不是刚性锚约束,anchorErr 对它不适用。

## 已知盲区

平地/中等俯仰下 anchorErr 恒 ≈0。真正压测泄漏要构造**近碰撞**(相机贴地+高俯仰,
clampNow 反扑 pin)或**正对地平线抓取**(conditioning→0)。工具已就位,是"拿它猎 bug"。
