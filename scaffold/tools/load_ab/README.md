# 加载期 A/B 自动化测量台

用来判定「交互期地形上传改涓流」这类改动**有没有拿到收益、有没有付超额代价**。
判定归机器，不归肉眼 —— 人只在三条判据全过之后行使最后否决权。

背景与基线数据见
[`docs/issues/terrain-visual-maturity-gap-2026-08-02.md`](../../../docs/issues/terrain-visual-maturity-gap-2026-08-02.md) §2.1。

## 跑法

```bash
# 0) 必须 release。debug 是 -O0，帧时数字全是幻觉。
examples/android/build_apk.sh release

# 1) 采基线
tools/load_ab/collect.sh before 5

# 2) 改代码，重新构建 release，再采一档
examples/android/build_apk.sh release
tools/load_ab/collect.sh after 5

# 3) 判定
tools/load_ab/report.py tools/load_ab/out/before tools/load_ab/out/after
```

退出码 0 = 通过，1 = 不通过，可直接接 CI 或 `&&`。

## 三条判据

| # | 指标 | 方向 | 阈值 | 为什么是它 |
|---|---|---|---|---|
| ① | 交互期 `pendUp` 峰值 | ↓ | ≤10 | 积压是"停手后暂态"的燃料 |
| ② | 停手 → `pendUp` 归零 | ↓ | ≤0.20s | 暂态本身 |
| ③ | 交互期慢帧率（帧/秒） | 不劣化 | ≤1.20× 基线 | 放开交互期上传 = 主线程多做 finalize，代价闸 |

①② 是收益，③ 是代价。**三条同时满足才算过。**

### 已采基线（2026-08-03，HEAD=`7bc16a5c8` 之后，release，5/5 轮有效）

| 指标 | 中位数 | 全距 |
|---|---|---|
| ① 积压峰值 | **48** | 43–51 |
| ② 排空时长 | **0.53s** | 0.27–0.55 |
| ③ 慢帧率 | **0.39/s** | 0.37–1.14 |
| 交互期 `fin>0` 帧数 | **0** | 0–0 |

①② 跨轮离散度 <±10%，说明这个测量台本身可信。③ 的全距里有一个 1.14/s 的离群轮
（其余四轮 0.37–0.56）—— **这正是"必须取中位数、必须跑 ≥5 轮"的实证**：若只跑到
那一轮，会得出"基线本来就卡"的错误结论，后续 A/B 全盘失真。

另打一个不参与判定的机制信号：**交互期 `fin>0` 的帧数**。基线恒为 0
（`TilePendingLoadQueue.cpp:149` 的 Urgent-only 早退发生在 `tryFinalize`
之前）。改完若仍为 0，说明闸根本没放开，①② 的任何变化都是噪声。

## 方法学约束（别绕过）

- **输入必须逐字相同**。用 `adb shell input keyevent 25`（音量下 =
  `nativeDebugZoom(0.84)`）×14，不用触摸注入 —— 实测 `adb shell input swipe`
  引擎侧收不到（GESTDIAG 为 0）。
- **计数类指标优先**。真机单次 run 有 ±2× DVFS 调频噪声，均值/最大帧时会给出
  随机结论。③ 用"慢帧条数"而非"平均帧时"正是为此。慢帧阈值取 25ms 是因为
  `FrameLoop` 只在 ≥25ms 时落行 —— 没落盘的帧算不出来，不能凭空定 16.67ms。
- **每档 ≥5 轮取中位数 + 全距**，单轮不作数。`report.py` 在有效轮 <3 时会告警。
- **每轮冷缓存**：`adb uninstall` + `install`（`pm clear` 在本机被
  SecurityException 拒）。
- **污染检测**：脚本只发 keyevent，不产生 `GESTDIAG dragStart`。一旦出现 =
  真人碰了手机，该轮相机轨迹与别轮不同、工作量不可比，`collect.sh` 打
  `.contaminated` 标记，`report.py` 自动剔除。**这条不是形式主义** —— 第一轮
  实测就是因此作废的。

## 依赖的日志

| 行 | 出处 | 用途 |
|---|---|---|
| `LoadGate` | `examples/android/MinimalGlobe/GLESView.cpp` | ①②，交互期识别（`mode` 的 `I` 位） |
| `FrameLoop` | 同上 | ③ |

两条都是既有诊断，无需为测量额外插桩。
