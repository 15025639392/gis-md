# 继续提示词 — 修复剩余 31 个测试失败

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

## 剩余 31 个测试失败

### 分类

**A. Clipped fallback 命令生成 (2)**
```
FAIL  Tileset: clipped fallback emits one ancestor patch command for one missing child
FAIL  Tileset: clipped fallback emits separate commands for separate child patches
```
原因: `buildRenderCommands` 没有生成命令。可能因为 glTF path 的 `buildTileDrawCommand` 流程不同，或者 `ensureTileMesh` 创建的 minimal model 没有被正确处理。

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

**C. Cache/unload tests (5)**
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

**E. RenderContentRasterOverlayStateUpdater (2)**
```
FAIL  RenderContentRasterOverlayStateUpdater: visible mapping contributes retained bytes
FAIL  RenderContentRasterOverlayStateUpdater: invisible overlay releases raster tile references
```

**F. TileRenderPlanFrameRefresher (3)**
```
FAIL  TileRenderPlanFrameRefresher: attached mapped raster is no longer counted as loading
FAIL  TileRenderPlanFrameRefresher: frame progress returns to complete after mapped raster attaches
FAIL  TileRenderPlanFrameRefresher: progress fixture attaches mapped raster during draw command preparation
```
原因: glTF 命令构建器可能不触发 raster attachment 与旧 SurfaceTile 路径相同的方式。

**G. Presentation trace (3)**
```
FAIL  Presentation trace: draw command consumes the resolved render entry without reselecting LOD
FAIL  Presentation trace: fading render entry emits a translucent surface command
FAIL  Presentation trace: ADD selected entries emit parent and child commands
```
原因: 检查 `RenderCommandKind::SurfaceTile` 需要改为 `GltfPrimitive`。

**H. Selection/fog (3)**
```
FAIL  Tileset: descendant-limit marks visited descendants as kicked
FAIL  Tileset: dense fog still visits the virtual terrain root before culling data tiles
FAIL  Tileset: multiple selector views refine using largest SSE like cesium-native
```

**I. TileUpsampleSourcePreparer (3)**
```
FAIL  TileUpsampleSourcePreparer: permanent failed ancestor is not retried when an older source is ready
FAIL  TileUpsampleSourcePreparer: content-loaded ancestor is finalized before upsample request proceeds
FAIL  TileUpsampleSourcePreparer: unloading ancestor is usable only for existing protected work and not for new upsample requests
```
原因: `prepareSourceTile` 使用 `useHeightmapSurfacePath = true`，但 glTF tile 没有 `hasTerrainMesh() && isSurfaceMeshReady()`。

## 修复策略

### 优先级 1: TileUpsampleSourcePreparer (3 个)

在 `test_sse_pipeline.cpp` 中搜索 `TileUpsampleSourcePreparer` 测试函数，找到 `prepareSourceTile` 调用。将 `useHeightmapSurfacePath` 参数从 `true` 改为 `false`，或者将 `allowGltfTerrainSource` 从 `false` 改为 `true`。

查看 `/Users/ldy/Desktop/work/gis-md/scaffold/src/earth_engine/tiling/TileUpsampleSourcePreparer.h` 的 `findSourceTile` 函数理解参数含义。

### 优先级 2: Presentation trace (3 个)

搜索 `RenderCommandKind::SurfaceTile` 在 presentation trace 测试中的使用，改为 `RenderCommandKind::GltfPrimitive`，并检查 `surfaceClipEnabled`、`surfaceTransitionOpacity` 等属性是否在 GltfPrimitive 命令上可用（它们在 `RenderCommand` 结构体上，与 kind 无关）。

### 优先级 3: Scene tests (6 个)

- `sampleHeight` 问题: 测试中调用 `ensureTile` 后需要调用 `ensureTileMesh` 来设置 glTF content
- `terrainSurfaceCommandsSubmitted` 问题: 检查是否有 `terrainGltfPrimitiveCommands` 或 `terrainRenderContentCommands` 计数器可用
- 查看 `Diagnostics.h` 中有哪些计数器: `/Users/ldy/Desktop/work/gis-md/scaffold/src/earth_engine/scene/Diagnostics.h`

### 优先级 4: Clipped fallback 命令 (2 个)

调试 `buildRenderCommands` 为什么没有生成命令。可能需要在 `GltfDrawCommandBuilder::build` 中检查条件。查看 `/Users/ldy/Desktop/work/gis-md/scaffold/src/earth_engine/tiling/GltfDrawCommandBuilder.cpp`。

### 优先级 5: Cache/unload + raster overlay + frame refresher (12 个)

这些需要逐个调查。常见模式:
- 创建 tile 时需要调用 `makeGltfRenderReady` 而不是只设置 `loadState = Done`
- 检查 `estimateTileBytes` 对 glTF tile 的计算是否正确
- 检查 raster overlay mapping attachment 在 glTF 路径下的行为

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
