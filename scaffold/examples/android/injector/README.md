# injector — 免 root 多点触控注入台

独立空壳 app 模块 + 自指 androidTest。`UiAutomation.injectInputEvent` 把合成
MotionEvent 打给**当前焦点窗口**(= 前台的 MinimalGlobe)。坐标为屏幕像素。

## 为什么独立模块

androidTest 的 `targetPackage` 恒 = 所在模块 applicationId(AGP 硬设,manifest
覆盖被吞)。若挂在 app 模块下,`AndroidJUnitRunner` 跑完会 **force-stop 目标 app**
(每次手势后 MinimalGlobe 被杀回桌面,伪装成 crash)。**自指独立空壳模块** →
跑完只 force-stop 这个空壳,MinimalGlobe 毫发无伤;注入是系统级,照样打到它。

## 两个入口

### `twoFinger` — 双指(pinch / rotate / tilt)
两指各自从 (ax0,ay0)/(bx0,by0) 线性插值到 (ax1,ay1)/(bx1,by1)。
pinch/rotate/tilt 由主机侧算好这 8 个坐标喂入,台本身不区分手势语义。
```bash
adb shell am instrument -w -e class com.earthengine.injector.GestureInjector#twoFinger \
  -e ax0 620 -e ay0 1100 -e bx0 620 -e by0 1600 \
  -e ax1 620 -e ay1 800  -e bx1 620 -e by1 1900 -e steps 24 -e dur 900 \
  com.earthengine.injector.test/androidx.test.runner.AndroidJUnitRunner
```

### `oneFinger` — 单指线性(pan / fling)
恒速(**无** `input swipe` 的末端 ease-out),用合成恒 dt 时间戳,故引擎侧算得的
**释放速度 = 总距/dur** —— 稳过近地惯性 100px/s 阈值(契约 1.4)。用于 C-V4 近地
惯性;`input swipe` 因 ease-out 触发不了近地惯性。
```bash
adb shell am instrument -w -e class com.earthengine.injector.GestureInjector#oneFinger \
  -e x0 620 -e y0 1600 -e x1 620 -e y1 1150 -e steps 18 -e dur 200 \
  com.earthengine.injector.test/androidx.test.runner.AndroidJUnitRunner
```
> 注:普通 pan(不需精确释放速度)用 `adb shell input swipe` 即可,更简单。

## 相关
- 读出/判据:`tools/cam_probe/README.md`(CAMPROBE 闭环)
- 环境坑(零售 Oplus 无 root / SELinux / 热降频):记忆 `gesture-realdevice-harness-2026-08-18`
