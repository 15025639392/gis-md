# 继续提示词 — 修复剩余 12 个测试失败

## 背景

你在 `/Users/ldy/Desktop/work/gis-md` 工作，参考实现是 `/Users/ldy/Desktop/work/cesium-native`。

**当前分支**: `codex/surface-instancing-gpu-batch`

**开始前必须执行**:
1. `cd /Users/ldy/Desktop/work/gis-md && git status --short --branch`
2. 阅读 `AGENTS.md`
3. 运行测试: `cd scaffold && ./test_native.sh test_sse_pipeline`

## 当前状态

Surface mesh 已完全移除，所有 terrain 内容现在走 glTF 路径。

| 测试目标 | 修复前 | 当前 |
|---|---|---|
| test_tile_render_plan_finalizer | 4 FAIL | 0 FAIL ✅ |
| test_tileset_quantized_mesh | 1 FAIL | 0 FAIL ✅ |
| test_sse_pipeline | 79 FAIL | 12 FAIL 🔄 |

## 剩余 12 个测试失败

```
FAIL  Scene: diagnostics classify glTF terrain render resolution as direct render
FAIL  TileContentCacheManager: smoothing preserves state
FAIL  TileContentCacheManager: external subtree unload retries after claimed upload work completes
FAIL  TileContentCacheManager: external subtree unload retries after active work completes
FAIL  TileContentUnloadCoordinator: protected unload setup attaches raster mapping
FAIL  TileContentUnloadCoordinator: protected upsample source detaches raster mappings before keeping CPU content
FAIL  Tileset: cache unload removes only the render parent's content
FAIL  Tileset: dense fog still visits the virtual terrain root before culling data tiles
FAIL  Tileset: descendant-limit marks visited descendants as kicked
FAIL  Tileset: external-content cache unload clears wrapper children
FAIL  Tileset: external-content cache unload removes descendants from flat map
FAIL  Tileset: multiple selector views refine using largest SSE like cesium-native
```

## 已完成的修复（关键模式）

本分支已完成的修复，新会话无需重复：

1. **`TileUpsampleSourcePreparer.h`**: `findSourceTile` 高度图祖先遍历用 `hasGltfContent()` 替代已删除的 `hasTerrainMesh() && isSurfaceMeshReady()`

2. **`GltfDrawCommandBuilder.cpp`**: 设置 `cmd.surfaceTransitionOpacity = context.transitionOpacity`

3. **`ensureTileMesh` helper** (test_sse_pipeline.cpp ~line 537): 需要 DummyBuffer 资源 + `rasterOverlayDetails.setGeographicRectangle(tile.bounds)`
   - `GltfDrawCommandBuilder::build` 跳过没有 vertex/index buffer 的 primitive
   - `RenderContentRasterOverlayStateUpdater::update` 检测 missing projections 时会 unload content

4. **`SceneRenderPipeline.cpp`**: `isTerrainSurfaceCommand` 检查 `GltfPrimitive + terrainRenderContent`（不是 `SurfaceTile`）

5. **`SceneRenderDiagnostics.cpp`**: 命令计数逻辑中 `GltfPrimitive` 同时计入 imagery 和 renderGltfPrimitives 路径

6. **所有 `RenderCommandKind::SurfaceTile` 引用**: 已替换为 `RenderCommandKind::GltfPrimitive`

## 修复策略

### Scene diagnostic (1 个)

```
FAIL  Scene: diagnostics classify glTF terrain render resolution as direct render
```

测试在 line ~22930。已设置 `makeGltfRenderReady(*root)` 使 tile 有 glTF content。问题是 `terrainRenderEntriesAncestorFallback` 等诊断计数器值不符合预期。

**调试方法**: 在测试中添加临时 print 输出 `scene.diagnostics()` 的各个字段，看实际值是什么。然后更新 check 断言。

### TileContentCacheManager (3 个)

```
FAIL  TileContentCacheManager: smoothing preserves state
FAIL  TileContentCacheManager: external subtree unload retries after claimed upload work completes
FAIL  TileContentCacheManager: external subtree unload retries after active work completes
```

这些测试检查缓存字节管理和卸载逻辑。

**调试方法**:
1. 找到测试函数（搜索测试名称）
2. 检查 `estimateTileBytes` 对 glTF tile 的返回值
3. 检查 `makeGltfRenderReady` 创建的 tile 是否正确计入字节
4. 可能需要在 `makeGltfRenderReady` 中添加 DummyBuffer 资源（当前只添加空的 `GltfPrimitiveRenderResources{}`）

### TileContentUnloadCoordinator (2 个)

```
FAIL  TileContentUnloadCoordinator: protected unload setup attaches raster mapping
FAIL  TileContentUnloadCoordinator: protected upsample source detaches raster mappings before keeping CPU content
```

这些测试检查卸载时的 raster overlay mapping 生命周期。

**调试方法**:
1. 搜索测试函数名
2. 检查 tile 是否有正确的 glTF content + raster overlay mapping
3. 可能需要调用 `makeGltfRenderReady` 或 `ensureTileMesh` 设置 tile

### Tileset cache/unload (3 个)

```
FAIL  Tileset: cache unload removes only the render parent's content
FAIL  Tileset: external-content cache unload clears wrapper children
FAIL  Tileset: external-content cache unload removes descendants from flat map
```

这些测试检查内容卸载逻辑。

**调试方法**:
1. 搜索测试函数名
2. 检查 tile 的 content state 是否正确（`loadState`, `contentKind`, `hasGltfContent()`）
3. 可能需要更新测试中的 tile 设置方式

### Tileset selection/fog (3 个)

```
FAIL  Tileset: descendant-limit marks visited descendants as kicked
FAIL  Tileset: dense fog still visits the virtual terrain root before culling data tiles
FAIL  Tileset: multiple selector views refine using largest SSE like cesium-native
```

这些测试检查瓦片选择和雾裁剪逻辑。

**调试方法**:
1. 搜索测试函数名
2. 检查 `setMeshReady(false)` 是否对 glTF tile 有效（`setMeshReady` 在 glTF 模式下可能行为不同）
3. 检查 `selectionKickedCount` 等状态变量的设置逻辑

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

同时确保其他测试目标不受影响:
```bash
cd scaffold && ./test_native.sh test_tile_render_plan_finalizer
cd scaffold && ./test_native.sh test_tileset_quantized_mesh
```
