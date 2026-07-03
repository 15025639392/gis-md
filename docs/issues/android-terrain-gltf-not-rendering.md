# Android 地形/瓦片不渲染 + 低 FPS 问题

**日期**：2026-06-28
**状态**：根因 B（QM parser 对齐）**已修复**——瓦片能解析、变 render-ready、发出 10 个绘制命令（`draw` 0→10）；**但地形仍未在屏幕上显示**（截图为纯色）。发现更上游的几何 bug:部分瓦片的顶点被放到错误 ECEF 位置(见"更新 3")。之前"渲染验证 ✅"的说法有误(仅凭 draw 计数,截图证明是蓝屏)。
**依赖**：与 android-black-screen-texture-limit 问题同时存在

---

## 更新 3（2026-07-01 晚，纠正）：地形仍未显示 —— 更上游的几何 bug

截图验证发现 demo 仍是蓝屏(更新 2 的"渲染 ✅"是仅凭 draw 计数的误判)。逐层排查绘制侧全部正确(PSO 有效、isTerrain=1、stride 32、MVP 绑定在 buffer 1、reverse-Z GEQUAL 深度、`PipelineLayout::Terrain` stride-32 描述符);关剔除、强制片元红色、都无片元 → 顶点阶段没产出可见三角形。埋点 `buildTerrainVertices` 打印顶点 ECEF:**部分瓦片解码正确**(`pos0Ecef≈(-1612314,5315087,3125381)`=重庆,RTC `rel` 很小 ✓),**部分瓦片解码成垃圾**(`pos0Ecef=(0,6378137,0)`=Y 轴上的退化点,`localOrigin`≈135°E/45°N)→ 被放到几百公里外/地平线下 → 出屏。**根因在更上游的 QM 瓦片几何/包围盒**(tile-key→矩形→ECEF,或 QM u/v→经纬度解码)对部分瓦片算错位置,同时也会导致选瓦片错误。**不是** shader/绘制接线的问题(那部分已验证正确)。这是一个之前从未端到端跑通过的独立 bug,待修。

## 更新 2（2026-07-01 晚）：根因 B 实际修复 + 端到端渲染验证 ✅

在 macOS Metal demo 对可达地形服务器(`192.168.3.3:8090`,重庆范围)做像素级验证时,复现了本文症状(`entries=0` / 地形不渲染),并**定位+修复了真实根因**:

- **根因 = QM parser 对 16-bit 索引不做 4 字节对齐**。用真实瓦片(65×65 网格,`vertexCount=4225`,75136 字节)验证:顶点数据后 `offset=25442`(偶数但非 4 对齐)。该服务器(及旧 FABDEM 构建)对 16-bit 索引数据**也补 2 字节到 4 对齐**,但 parser 仅对 `idx32`(>65536 顶点)补齐。于是 `triangleCount` 在 25442 读到垃圾 **536870912** → 索引溢出 → `parseToDecodedTile` 返回 nullptr → 瓦片永不 render-ready → `entries=0` → 不渲染。加 2 字节 padding 后在 25444 读到 **8192**(=64×64×2,正确),整块解析到 75136 干净收尾。
- **修复**(`QuantizedMeshParser.cpp` 两处:`parseToDecodedTile` + `parseMetadataAvailabilityWithDiagnostics`):用**拟合检测**——若未补齐读到的 triCount 放不下剩余字节、而补 2 字节能放下,则判定为 4 对齐瓦片。**同时兼容 spec(2 对齐)与本服务器(4 对齐),不破坏任一方**。
- **验证**:native QM/terrain/sse 14/14 通过(无回归);demo 重编后跑同一服务器 —— `parseToDecodedTile fail` 从多次降为 **0**,`draw` 从 **0 → 10 tiles=10**,`entries=18`,120 FPS,重庆上空地形+高德影像正常渲染。

- (历史)本文原描述的"去掉 `idx32 &&` 守卫、所有情况都补齐"是错的方向 —— 那会破坏 spec 合规的 2 对齐瓦片。正解是拟合检测(见上)。
- **行号已漂移**：`TileRenderContentState.h` 现为 `hasGltfResources` @`:134`、`isGltfRenderReady` @`:137-138`、`isRenderContentReady` @`:140-141`（本文写的 `:131-139` 过时约 3 行）。`TileRenderPlanFinalizer.h:163`（`canBuildRenderEntryDirectly`）与 `TileRenderablePolicy.cpp:23-25`（Render 分支）仍准确。
- **尾部"问题链 / 可能原因 / 需要检查的代码路径"（约 138 行之后）是定位真实根因之前的推测**（"`gltfResourcesReady_` 从未 true"等），与"已修复"结论相互矛盾，属历史调查记录，不代表现状。
- **当前分支需注意**：同类症状（`gltf=0` / `entries=0` / 地形不渲染）可能在 `codex/surface-instancing-gpu-batch` 分支再现 —— 因为地形轻量顶点的**绘制侧未接通**（`GltfDrawCommandBuilder` 仍发 stride-120、`Renderer::terrainShader()` 未定义），详见 `android-performance-analysis.md` 的核对更新。

---

## 问题描述

修复 shader 纹理采样器超限问题后，屏幕不再黑屏，但：
1. 看不到地形和卫星影像瓦片
2. FPS 很低（~35 FPS，目标 60 FPS）

### 症状 1：瓦片不渲染

日志显示：
```
submit: 3 commands, surface=1 gltf=0 vector=0 env=2
```

- `surface=1` — 只有 1 个地表命令（globe fallback）
- `gltf=0` — 没有 glTF primitive 命令（地形瓦片）
- `entries=0` — 地形瓦片未生成渲染条目
- `selectedTiles=122` — 瓦片被选中但未渲染

### 症状 2：低 FPS

```
Engine.render.total = 27-30ms → ~33-37 FPS（目标 60 FPS）
```

性能分解：
- `update=23-25ms` — 主要瓶颈
  - `terrain=23ms` — 地形更新
  - `prefetch=17ms` — 预取（即使 visited=0 仍消耗大量 CPU）
  - `selector=3ms` — 瓦片选择器
  - `request=3.7ms` — 请求处理
- `render=3.6-4.8ms` — 渲染命令构建
- `submit=0.11ms` — GL 提交

**关键**：即使 `visited=0, notReady=0`，`prefetch` 仍每帧消耗 17ms，说明管线在空转。

---

## 根本原因分析

### 渲染条目生成流程

```
TileRenderPlanFinalizer::appendRenderEntry()
  → canBuildRenderEntryDirectly(tile)
    → tile.content.renderContent.hasGltfContent() ?
      → true: return isGltfRenderReady()
      → false: return hasRenderableSurfaceForPlan(tile)

isGltfRenderReady() = gltfModel != nullptr && hasGltfResources()
hasGltfResources() = gltfResourcesReady_ && !gltfPrimitiveResources.empty()
```

### 问题链

```
QuantizedMesh 地形加载
  → 转换为 glTF model（gltfModel != nullptr）
  → GltfRenderResourcePreparer::prepare() 被调用
  → 需要创建 GPU 资源（vertex buffer, index buffer, texture）
  → 如果 model 为空或资源创建失败 → gltfResourcesReady_ 保持 false
  → isGltfRenderReady() 返回 false
  → entries=0，地形不渲染
```

### 关键代码位置

1. **TileRenderPlanFinalizer.h** (line 163-168):
   ```cpp
   static bool canBuildRenderEntryDirectly(const TilesetTile& tile) {
       if (tile.content.renderContent.hasGltfContent()) {
           return tile.content.renderContent.isGltfRenderReady();
       }
       return hasRenderableSurfaceForPlan(tile);
   }
   ```

2. **TileRenderContentState.h** (line 134-136):
   ```cpp
   bool isGltfRenderReady() const {
       return gltfModel != nullptr && hasGltfResources();
   }
   ```

3. **GltfRenderResourcePreparer.cpp** (line 118-122):
   ```cpp
   void GltfRenderResourcePreparer::prepare(TilesetTile& tile, ...) {
       GltfModel* model = tile.content.renderContent.gltfContent();
       if (!model) return;  // 如果 model 为空，直接返回
       ...
   }
   ```

---

## 可能的原因

1. **glTF model 未正确创建** — QuantizedMesh 到 glTF 的转换可能失败
2. **GPU 资源创建失败** — 顶点/索引缓冲区或纹理创建可能失败
3. **surface mesh 移除后管线断裂** — 之前的 surface mesh 路径被移除，但 glTF 路径未完全接替

---

## 根本原因（已修复！）

### 实际根因：两个问题

**问题 A：地形服务器不可达**
- 配置中使用 `192.168.0.3:8090`，但设备无法连接该地址
- 需要更新为实际可达的 IP（如 `192.168.1.6:8090`）

**问题 B：QuantizedMesh 解析器不处理 4 字节对齐填充**
- FABDEM 地形构建脚本在三角形计数前添加 2 字节填充（用于 4 字节对齐）
- `QuantizedMeshParser.cpp` 只在 `idx32`（顶点数 > 65536）时处理对齐
- 实际需要在所有情况下都处理对齐

### 修复

**文件**：`scaffold/src/earth_engine/terrain/QuantizedMeshParser.cpp`

```cpp
// 修复前：
bool idx32 = (vc > 65536);
if (idx32 && (offset % 4) != 0) offset += 2;

// 修复后：
bool idx32 = (vc > 65536);
if ((offset % 4) != 0) offset += 2;
```

### 问题链

### 问题链

```
1. 地形 tile 加载 QuantizedMesh → 转换为 glTF
2. prepareGltfContent() 设置 gltfModel，但 gltfResourcesReady_ = false
3. markRenderContentLoaded() 设置 loadState = ContentLoaded
4. GltfRenderResourcePreparer::prepare() 应该被调用创建 GPU 资源
   → 但它可能未被调用，或者失败了
5. gltfResourcesReady_ 保持 false
6. isGltfRenderReady() 返回 false
7. isRenderContentReady() 返回 false
8. isCompleteRenderable() 返回 false
9. tile 不可渲染，entries=0
```

### 关键代码

**TileRenderablePolicy.cpp (line 23-25)**:
```cpp
case TileContentKind::Render:
    return snapshot.requiredRasterOverlaysReady &&
           snapshot.meshReady;  // ← 对 glTF 地形始终为 false
```

**TileRenderContentState.h (line 137-139)**:
```cpp
bool isRenderContentReady() const {
    return gltfModel ? isGltfRenderReady() : surface_.meshReady;
}
```

**TileRenderContentState.h (line 134-136)**:
```cpp
bool isGltfRenderReady() const {
    return gltfModel != nullptr && hasGltfResources();
}
```

**TileRenderContentState.h (line 131-133)**:
```cpp
bool hasGltfResources() const {
    return gltfResourcesReady_ && !gltfPrimitiveResources.empty();
}
```

**问题**：`gltfResourcesReady_` 从未被设为 true，因为 `GltfRenderResourcePreparer::prepare()` 未被调用或失败。

### 可能原因

1. **GPU 资源准备未被触发** — `GltfRenderResourcePreparer::prepare()` 未被调用
2. **GPU 资源准备失败** — `prepare()` 被调用但失败（model 为空或资源创建失败）
3. **loadState 卡在 ContentLoaded** — 未转换到 Done 状态

### 需要检查的代码路径

1. `TilePendingLoadCommitCoordinator::commitUpload()` — 是否调用 `ensureGltfResources()`
2. `GltfRenderResourcePreparer::prepare()` — 是否被调用，model 是否为空
3. `TileContentUploadPolicy::prepareGltfRenderContent()` — 是否正确设置 gltfModel
4. loadState 转换：ContentLoaded → Done 的路径

---

## 相关文件

- `scaffold/src/earth_engine/tiling/TileRenderPlanFinalizer.h`
- `scaffold/src/earth_engine/tiling/TileRenderContentState.h`
- `scaffold/src/earth_engine/tiling/GltfRenderResourcePreparer.cpp`
- `scaffold/src/earth_engine/tiling/TilesetTile.h`
- `scaffold/src/earth_engine/tiling/TileContentUploadPolicy.cpp`

---

## 临时解决方案

恢复 `blocksCompleteRenderable = true` 和高德影像配置，等待根本原因修复。
