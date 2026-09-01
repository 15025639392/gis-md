# 继续任务提示词

请在新窗口中使用以下提示词继续当前任务：

---

## 提示词

我在 `/Users/yan/Desktop/work/gis-md` 项目的 `main` 分支上排查 **Android MinimalGlobe（PHK110 真机 `7e045e39`）交互卡顿**。已定位并修复三个 CPU 热点（均已在 origin/main）。

**⚠️ 关键方法论（本次最重要修正）**：性能测量**必须用 release 变体**（`./gradlew assembleRelease`，applicationId 带 `.codexverify` 后缀——这是项目 build.gradle 里「专供本地性能测量」的 -O2 构建）。**debug 是 -O0**，会把 glm 双精度数学膨胀 2.5-3×，产生假热点。此前所有"地形 build 1933ms / 命令重建 561ms / 单瓦 206ms"都是 debug 假象；release 下真实成本小 10-20×。**别再用 debug 包做性能评估。**

### 已完成并推送（commit 均在 origin/main）

**1. 热点① 贴地重钳**（`ae1b71d3`）
- `FeatureRenderLayer.{h,cpp}`: `reclampTileBucketLines` 按 `kReclampVertsPerFrame=2000` 顶点分片推进（`lineReclampProgressVert_`/`lineReclampVerts_`），整桶完成才上传 buffer；重钳循环改"当帧顶点数预算"。
- 真机 release: 重钳 8ms（无整帧冻结）。host 测试 `VertexBudgetSlicesLargeLineBucketAcrossFrames`。

**2. 热点② 标注烘焙**（`ae1b71d3`）
- `FeatureRenderLayer.cpp`: labelScan 加 `kLabelBakeBucketsPerFrame=16` 桶预算；烘焙幂等，未烘桶由 `hasPendingLabelWork` 供帧不饿死。
- 真机 release: labelScan 6ms。host 测试 `LabelBakeBudgetSlicesLargeRebakeAcrossFrames`。

**3. 热点④ 地形 fill-mask request**（`0e9eafd3`）
- `SceneRenderPipeline.{h,cpp}`: `prepareTerrainFillMasks` 循环前取一次 `store->styleRevision()`，**已就绪纹理的瓦跳过 request**（只对需更新的瓦调）。
- 真机 release: 稳态拖动 request 62ms→近 0；剩余 20-41ms 是新瓦 miss（必须加载）。
- 诊断埋点保留：`fillMaskBreakdown`（fillMask/req/up 拆分，>2ms 才打）。

**4. 热点③ 标签 placement**（**本次**）
- 根因:98.7% 屏外候选(驻留远瓦)collect 建结构体+dedup 白算、update 全量投影白算 → placement 4-6ms。
- 修=`LabelPlacement::boxFullyOffscreenScreen`(盒外接半径越出视口判屏外)在 **collect 与 update 双处共用**:collect 挡 97.6% 屏外候选(cand 2278→54),update 省投影(update 4.5→0.35ms);沿线标签(collisionParts 非空)保留原路径。
- 真机 release:placement 4-6ms→0-0.65ms,**4ms 哨兵 0 触发**;LabelDump placed=30 正常渲染。屏外标签 fade 清扫、重入淡入(用户拍板,对齐设计注释)。host 测试 27/27(新增 `BoxFullyOffscreenHelperCullsAndPreserves` 等),全量 212/212。

### 关键定位结论（供续接参考）
- **瓦片数不是浪费**：`upd=0.5ms`（瓦片选择）证伪选择瓶颈；45-103 瓦是 1240×2772 屏满足 SSE 的合理结果。
- **热点④ 根因**：`buildBreakdown` 的 `layers=22-88ms` 主要来自 `prepareTerrainFillMasks` 的 `AmapTerrainFillMaskStore::request`（锁/scheme/map 固定开销，62-150ms），**非**命令重建（8ms）、**非**瓦片选择（0.5ms）、**非**纹理上传（2ms）。
- **测试**：全量 native 212/212 绿（含新增分片/预算测试）；AI_INDEX 行号已同步。

### 遗留（下一步候选）
1. ~~热点③ 标签避让 placement~~ **已完成(本次)**。原方向"P4 时间片增量 + 视锥预剔除屏外候选"——预剔除即消尖刺,时间片因哨兵不再触发而暂不做(万级候选真现再议)。
2. **新瓦 miss 的 request**（20-41ms，拖动进未加载区域）：可看 `AmapTerrainFillMaskStore::request` 的 `state->fetch` 同步部分或 Landing 票开销，但属加载期，收益递减。
3. **release 全链路帧率**：真机 117 帧中 16 帧慢（13.7%），慢帧 max 88→50ms；是否继续压由需求决定。

### 验证环境
- 真机 PHK110 `7e045e39`；release 包 `com.earthengine.minimalglobe.codexverify`（启动：`adb shell am start -n com.earthengine.minimalglobe.codexverify/com.earthengine.minimalglobe.MainActivity`）。
- 注入拖动：`adb shell input swipe 300 1600 1050 500 2500`。
- 性能埋点：`adb shell setprop debug.ee.perflog 1`。
- native 测试：`cd scaffold && ./test_native.sh`。
- 关键文件：`FeatureRenderLayer.{h,cpp}`、`SceneRenderPipeline.{h,cpp}`、`AmapTerrainFillMaskStore.cpp`。
