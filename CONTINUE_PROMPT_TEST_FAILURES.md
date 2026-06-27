# 继续提示词 — 修复剩余 18 个测试失败

## 背景

你在 `/Users/ldy/Desktop/work/gis-md` 工作，参考实现是 `/Users/ldy/Desktop/work/cesium-native`。

**当前分支**: `codex/surface-instancing-gpu-batch`

**开始前必须执行**:
1. `cd /Users/ldy/Desktop/work/gis-md && git status --short --branch` — 预期有未提交修改
2. 阅读 `AGENTS.md`
3. 涉及算法时先读 `/Users/ldy/Desktop/work/cesium-native/AI_INDEX.md`
4. 运行测试: `cd scaffold && ./test_native.sh <test_target>`

## 当前状态

Surface mesh 已完全移除，所有 terrain 内容现在走 glTF 路径。

### 已修复（上一个会话）

| 测试目标 | 修复前 | 修复后 |
|---|---|---|
| test_tile_render_plan_finalizer | 4 FAIL | 0 FAIL ✅ |
| test_tileset_quantized_mesh | 1 FAIL | 0 FAIL ✅ |
| test_sse_pipeline | 79 FAIL | 31 FAIL 🔄 |

### 已做的修改

1. **`test_sse_pipeline.cpp` — `ensureTileMesh` helper** (line ~521): 更新为在 residue 清理后，如果 tile 有 Render content 但没有 glTF model，自动设置 glTF content 并标记 Done。使用内联创建 minimal GltfModel。

2. **`test_sse_pipeline.cpp` — `SparseTerrainProvider`** (line ~4587): 从 `TerrainProvider` 改为继承 `TilesetContentProvider`，实现 `requestTileContent`/`decodeContent`/`availabilityState` 等方法。这样可以正确传入 Tileset 构造函数。

3. **`test_sse_pipeline.cpp` — 多个 upsample 测试**: 将 `makeContentTerrainTileset(std::move(scheme))` 改为 `Tileset(std::move(scheme), {}, nullptr, TilesetOptions{}, std::move(provider))`，使 SparseTerrainProvider 正确接入 tileset。已修复的函数:
   - `testTilesetCreatesUpsampledChildrenForUnavailableSiblings`
   - `testTilesetCreatesNonRootTerrainChildrenInCesiumOrder`
   - `testTilesetCreatesNonRootUpsampledTerrainSiblingsInCesiumOrder`
   - `testTilesetUpsampledChildQueuesParentUntilSourceReady`
   - `testTilesetUpsampledChildFinalizesContentLoadedParent`
   - `testTilesetUpsampledChildBuildsGltfFromGltfParent`

4. **`test_sse_pipeline.cpp` — `TilesetTestAccess::makeGltfRenderReady`** (line ~550): 新增 helper，为 tile 创建 minimal glTF model + primitive resources + markRenderContentDone。

5. **`test_sse_pipeline.cpp` — TileRenderPlanFinalizer tests** (line ~13420): 3 个测试添加 `makeGltfRenderReady`，将 `SynchronousPrep` 改为 `Direct`，deferred prep 从 1 改为 0。

6. **`test_sse_pipeline.cpp` — clipped fallback test** (line ~23848): 将 `isMeshReady() && surfaceVertexBuffer() != nullptr` 改为 `hasGltfContent() && hasGltfResources()`，`RenderCommandKind::SurfaceTile` 改为 `GltfPrimitive`。

7. **`test_tile_render_plan_finalizer.cpp`**: 4 个测试全部修复 — 添加 `makeGltfRenderReady`/glTF content，更新 expected reason/count。

8. **`test_tileset_quantized_mesh.cpp`**: 1 个测试修复 — 更新 expected assertions 以匹配新行为（accepted terrain content 被保留而非清除）。

9. **`test_sse_pipeline.cpp` — cache manager tests**: 2 个测试添加 `makeGltfRenderReady`。

## 已修复（本会话，13 个）

| 测试 | 修复方式 |
|---|---|
| TileUpsampleSourcePreparer ×3 | `TileUpsampleSourcePreparer.h`: 将 `hasTerrainMesh() && isSurfaceMeshReady()` 替换为 `hasGltfContent()` |
| Presentation trace ×3 | `RenderCommandKind::SurfaceTile` → `GltfPrimitive`，更新断言适配 glTF 路径 |
| Clipped fallback ×2 | `ensureTileMesh` 添加 DummyBuffer + `rasterOverlayDetails`，`GltfDrawCommandBuilder` 设置 `surfaceTransitionOpacity` |
| RenderContentRasterOverlayStateUpdater ×2 | 修复 byte accounting 用相对增量检查 |
| TileRenderPlanFrameRefresher ×3 | `ensureTileMesh` 的 DummyBuffer 和 `rasterOverlayDetails` 修复后自动通过 |

## 剩余 18 个测试失败

### 分类

**B. Scene tests (6)**
```
FAIL  Scene: primary terrain tileset remains the height sampling source
FAIL  Scene: adding glTF tileset does not replace terrain sampling
FAIL  Scene: no-base-imagery terrain still submits placeholder surface render entries
FAIL  Scene: terrain sampling is still owned by primary tileset after render
FAIL  Scene: diagnostics expose nonzero terrain render-entry fallback reasons
FAIL  Scene: diagnostics classify legacy terrain render resolution as ancestor fallback
FAIL  Scene: imagery-only ancestor fallback draws selected surface
```
原因:
- `sampleHeight` 需要 tile 有 glTF content + height data
- `terrainSurfaceCommandsSubmitted` 只统计 `SurfaceTile` 命令，glTF 用 `GltfPrimitive`
- 部分测试用 `ensureTile` 但不调用 `ensureTileMesh`，tile 没有 content

**C. Cache/unload tests (6)**
```
FAIL  TileContentCacheManager: smoothing preserves state
FAIL  TileContentCacheManager: external subtree unload retries after claimed upload work completes
FAIL  TileContentCacheManager: external subtree unload retries after active work completes
FAIL  Tileset: cache unload removes only the render parent's content
FAIL  Tileset: external-content cache unload clears wrapper children
FAIL  Tileset: external-content cache unload removes descendants from flat map
```
原因: byte accounting 或 unload 逻辑在 glTF 路径下行为不同。

**D. TileContentUnloadCoordinator (2)**
```
FAIL  TileContentUnloadCoordinator: protected unload setup attaches raster mapping
FAIL  TileContentUnloadCoordinator: protected upsample source detaches raster mappings before keeping CPU content
```

**H. Selection/fog (3)**
```
FAIL  Tileset: descendant-limit marks visited descendants as kicked
FAIL  Tileset: dense fog still visits the virtual terrain root before culling data tiles
FAIL  Tileset: multiple selector views refine using largest SSE like cesium-native
```

## 修复策略

### 优先级 1: Scene tests (6 个)

- `sampleHeight` 问题: 测试中调用 `ensureTile` 后需要调用 `ensureTileMesh` 来设置 glTF content，或者设置 `isTerrainRenderContent` + glTF model
- `terrainSurfaceCommandsSubmitted` 问题: 检查是否有 `terrainGltfPrimitiveCommands` 或 `terrainRenderContentCommands` 计数器可用
- 查看 `Diagnostics.h` 中有哪些计数器: `/Users/ldy/Desktop/work/gis-md/scaffold/src/earth_engine/scene/Diagnostics.h`

### 优先级 2: Cache/unload + TileContentUnloadCoordinator (8 个)

这些需要逐个调查。常见模式:
- 创建 tile 时需要调用 `makeGltfRenderReady` 而不是只设置 `loadState = Done`
- 检查 `estimateTileBytes` 对 glTF tile 的计算是否正确
- 检查 raster overlay mapping attachment 在 glTF 路径下的行为

### 优先级 3: Selection/fog (3 个)

- `descendant-limit`: 检查 kicked 状态设置是否与 glTF 路径兼容
- `dense fog`: 检查 virtual terrain root 访问逻辑
- `multiple selector views`: 检查 SSE 比较逻辑

## 关键修复模式

本会话发现的关键模式:
1. **`ensureTileMesh` 需要 DummyBuffer**: `GltfDrawCommandBuilder::build` 跳过没有 vertex/index buffer 的 primitive
2. **`ensureTileMesh` 需要 `rasterOverlayDetails`**: `RenderContentRasterOverlayStateUpdater::update` 检测 missing projections 时会 unload content
3. **`hasTerrainMesh()/isSurfaceMeshReady()` 已删除**: 所有引用需要改为 `hasGltfContent()`
4. **`RenderCommandKind::SurfaceTile` 已删除**: 所有引用需要改为 `GltfPrimitive`
5. **`surfaceTransitionOpacity`**: `GltfDrawCommandBuilder` 需要设置此字段
6. **Byte accounting**: glTF resource bytes 需要纳入 `estimateRetainedBytes`

## 关键约束

- **不留技术债**: 彻底修复或删除失败的测试，不能 skip 或 mock
- **每次闭环必须完整**: 每个提交必须是完整、可验证的闭环
- **不要为了测试作假**: 测试必须验证真实行为
- **不要 push**，除非用户明确要求
- **commit message 格式**: `动作 + 对象`
- **测试失败必须修复**: 如果修改导致测试失败，必须修复测试或更新测试期望

## 验证

修复后运行以下测试验证:
```bash
cd scaffold && ./test_native.sh test_sse_pipeline
```

预期结果:
- test_sse_pipeline: 0 FAIL
