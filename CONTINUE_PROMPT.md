# gis-md Terrain-as-Tileset-Content 对齐 — 继续提示词

## 背景

你在 `/Users/ldy/Desktop/work/gis-md` 工作，参考实现是 `/Users/ldy/Desktop/work/cesium-native`。

**长期目标**: 系统性完成 gis-md native terrain + raster overlay 与 cesium-native 的架构和行为对齐。核心是把 gis-md 收敛到 cesium-native 的 terrain-as-tileset-content 地板上。

**当前分支**: `codex/surface-instancing-gpu-batch`

**开始前必须执行**:
1. `cd /Users/ldy/Desktop/work/gis-md && git status --short --branch` — 预期干净工作树
2. 阅读 `AGENTS.md`
3. 涉及算法时先读 `/Users/ldy/Desktop/work/cesium-native/AI_INDEX.md`
4. 运行测试: `cd scaffold && ./test_native.sh <test_target>`

## 已完成的工作（19 个提交）

### Domain policy 下沉
- `TileLoadDomainPolicy` 新增 `availabilityUpdatesForDomain` + `shouldCreateTerrainChildren`
- `TileAvailabilityUpdateCommitter` 简化为纯执行者
- `TileChildFrameMaterializer` 不再直接检查 `isTerrainAvailabilityUpsample()`

### Provider 接口统一
- 基类 `TilesetContentProvider`: `terrainAvailabilityState` → `availabilityState`, `applyTerrainAvailabilityUpdates` → `applyAvailabilityUpdates`
- `QuantizedMeshTerrainProvider` / `EllipsoidTerrainContentProvider`: 移除 wrapper，直接 override

### Legacy heightmap surface path 移除
- `TileContentAccess`: 移除 `forHeightmapTerrainSurfacePath`、`TerrainOwnership::HeightmapSurface`、`legacyHeightmapContent_`
- `Tileset.cpp`: 移除 legacy 路由
- `TileMeshPreparationManager`: 移除 `LegacyHeightmapSurface` mode
- `TileRefinementAvailabilityResolver`: 移除 `canRefineLegacyHeightmapSurfaceOrExternalContent`
- 删除 4 个文件: `TileLegacyHeightmapContentResolver.{h,cpp}`, `TileLegacyHeightmapSurfacePreparer.{h,cpp}`

### LegacyHeightmapTerrainCache 移除
- `TileContentLifecycleManager`: 移除 `legacyHeightmapTerrainCache_`
- `TileContentCacheManager` / `TileCacheOwnershipManager` / `TileContentRuntime` / `LoadedTerrainHeightSampler`: 移除 legacy cache 引用

### Legacy terrain provider 移除
- `TilesetTerrainProviders`: 简化为只接受 `TilesetContentProvider`

### 测试转换
- 70+ SSE pipeline 测试从 `makeLegacyTerrainTileset` → `makeContentTerrainTileset`
- 10+ budget 测试转换为 content provider path
- 2 sample height 测试移除（legacy cache 依赖）
- 多个 raster overlay 测试从 surface mesh → glTF

## 当前测试状态

| 测试目标 | 结果 |
|----------|------|
| `test_tile_pending_load_commit_coordinator` | **75 PASSED** |
| `test_tileset_request_missing_budget` | **20 PASSED** |
| `test_tileset_selection_refinement` | **30 PASSED** |
| `test_tileset_quantized_mesh` | **28 PASSED** |
| `test_tile_selection_root_policy` | **16 PASSED** |
| `test_tile_content_lifecycle_manager` | **3 PASSED** |
| `test_tileset_sample_height` | **4 PASSED** |
| `test_sse_pipeline` | **229 PASSED**, 3 FAIL |

## 剩余未完成任务

### 任务 1: 移除 `contentProviderTerrainQuadtreeTile` 标志和 surface mesh 渲染路径

**当前状态**: 标志在 6 个位置控制 terrain-specific 行为，移除会导致 71+ 测试失败。

**需要做的**:
1. 转换 67+ 个测试从 surface mesh 到 glTF（最大工作量）
2. 移除 `SurfaceTileDrawCommandBuilder.{h,cpp}`
3. 移除 `SurfaceMeshResourcePreparer.{h,cpp}`
4. 移除 `TileSurfaceMeshSourceResolver.h`
5. 移除 `TileSurfaceMeshEnsurer.h`
6. 移除 `TileSurfaceRenderContentCoordinator.h`
7. 移除 `contentProviderTerrainQuadtreeTile` 标志
8. 移除 `commitSurfaceRenderContent`、`refreshSurfaceDrawable` 等方法

**关键约束**:
- `contentProviderTerrainQuadtreeTile` 标志在 `TilesetTile.h` 中声明，在 `TileContentAccess.cpp` 中设置
- 标志在以下位置使用: `hasRasterOverlayHostContent()`, `renderableSnapshot()`, `hasSurfaceDrawable()`, `waitsForContentTerrainRasterDetails()`, `TileRenderCommandPreparer::build`, `TileRenderPlanFinalizer`
- 移除标志需要同时更新所有这些位置
- 测试转换模式: `setSurfaceMesh` → `prepareGltfContent` + `setTerrainRenderContent(true)` + `addGltfPrimitiveResource` + `markRenderContentReady`

**转换示例**:
```cpp
// 旧代码:
root->content.renderContent.setSurfaceMesh(std::make_unique<SurfaceTileMesh>());
root->content.renderContent.setMeshReady(true);
root->content.renderContent.setSurfaceGpuBuffers(
    std::make_unique<DummyBuffer>(32), nullptr);

// 新代码:
auto gltfModel = makeQuadTerrainGltfModel(rectangle);
gltfModel->rasterOverlayDetails = makeProviderDetails(scheme, rectangle);
root->content.renderContent.prepareGltfContent(
    std::move(gltfModel), Mat4::identity());
root->content.renderContent.setTerrainRenderContent(true);
GltfPrimitiveRenderResources resources;
resources.vertexBuffer = std::make_unique<DummyBuffer>(64);
resources.indexBuffer = std::make_unique<DummyBuffer>(12);
resources.indexCount = 6;
resources.vertexCount = 4;
root->content.renderContent.addGltfPrimitiveResource(std::move(resources));
root->content.renderContent.markRenderContentReady();
```

### 任务 2: 修复 3 个 SSE pipeline 测试失败

这 3 个测试在移除标志之前就已失败:
- `blocking base imagery draws surface geometry with the shared placeholder texture`
- `surface command carries quantized-mesh water mask state`
- `build command emits a surface command from the core ready raster`

**根因**: 这些测试专门测试 surface mesh 渲染路径。移除 `contentProviderTerrainQuadtreeTile` 标志后，surface mesh 路径不再被阻断，但测试仍然失败。

**注意**: 移除这 3 个测试函数会破坏 80 个其他测试（测试框架有隐式依赖）。需要同时处理依赖关系。

### 任务 3: 修复 segfault

Segfault 发生在 `testTilesetBlockingBaseImageryDrawsPlaceholderSurface` 清理阶段。`RasterMappedToTilesetTile` 的 null unique_ptr 访问。与任务 2 相关。

## 架构参考

### 数据流

```
QuantizedMeshTerrainProvider::requestTileContent()
  → QuantizedMeshContentLoader::loadTileContent() → GltfModel
  → TileContentLoadResult::renderTerrain(gltfModel)
  → TileLoadResult::fromContentResult()
  → TilePendingLoadCommitCoordinator::commitUpload()
    → TileLoadDomainPolicy::availabilityUpdatesForDomain()
    → TileContentUploadCommitter::prepareRenderContent()
    → ensureGltfResources() → GPU upload
    → TileContentUploadCommitter::finishRenderResourcePreparation()
  → TileRenderCommandPreparer::build()
    → GltfDrawCommandBuilder::build() → RenderCommand (GltfPrimitive)
```

### 关键文件

| 文件 | 用途 |
|------|------|
| `TileLoadDomainPolicy.h` | Domain-specific 决策: upload/terminal/metadata/availability/child-creation |
| `TileAvailabilityUpdateCommitter.h` | Availability update 执行 |
| `TilePendingLoadCommitCoordinator.h` | 内容提交协调 |
| `TileContentUploadCommitter.h` | glTF 内容上传 |
| `TileRenderCommandPreparer.h` | 渲染命令构建 |
| `TilesetTile.h` | 瓦片状态和属性 |
| `TileContentAccess.h` | 瓦片创建和子节点管理 |
| `TileContentLifecycleManager.h` | 内容生命周期 |
| `QuantizedMeshTerrainProvider.h` | 地形内容提供者 |

## 规则

- **不留技术债**: 彻底移除旧实现，不保留旧主路径、旧实现、兼容 shim 或死代码。如果移除会影响测试，必须同步转换测试，不能跳过。
- **每次闭环必须完整**: 每个提交必须是完整、可验证的闭环。不能留"后续再处理"的尾巴。
- **不要为了测试作假**: 测试必须验证真实行为，不能用 mock 或 skip 掩盖问题。
- **不要 push**，除非用户明确要求。
- **commit message 格式**: `动作 + 对象`（如 `Move availability update selection into domain policy`）。
- **遇到大规模重构时**: 可以分批提交，但每批必须是完整闭环。不能把半成品留在代码里。
- **测试失败必须修复**: 如果修改导致测试失败，必须修复测试或更新测试期望。不能忽略失败。
- **死代码必须删除**: 如果代码不再被生产路径调用，必须删除。不能留着"以防万一"。

## 动态 Agent 分配与回收策略

### 核心原则

根据任务实际复杂度、并行度和依赖关系动态调整 agent 数量，而不是固定使用 N 个 agent。目标是最小化 wall-clock 时间，同时避免浪费 token。

### 何时增加 Agent（分配）

**立即分配更多 agent 的信号**:

1. **可并行独立文件批量修改**: 当需要修改 10+ 个文件且每个文件的修改是独立的（如批量测试转换），按文件组分配 agent。每组 5-8 个文件为宜。
2. **多维度分析需求**: 当需要同时从多个角度分析问题（如 bug 根因、影响范围、修复方案），每个维度一个 agent。
3. **测试失败数量 > 5**: 批量测试失败时，按失败原因分组（相同根因的归为一组），每组一个 agent 并行修复。
4. **探索阶段**: 对未知代码区域的探索，使用 2-3 个 agent 从不同入口点并行搜索（如按文件名搜索、按符号搜索、按调用链搜索）。

**分配示例**:
```
场景: 需要将 67 个测试从 surface mesh 转换为 glTF
→ 分析: 按测试文件分组，每组 8-10 个测试函数
→ 分配: 8 个 agent 并行转换，每个 agent 负责一个文件的测试
→ 汇总: 等待所有 agent 完成，运行全量测试验证
```

### 何时减少 Agent（回收）

**立即回收 agent 的信号**:

1. **任务完成**: agent 完成其分配的工作后，立即回收，不要保留空闲 agent。
2. **依赖阻塞**: 当 agent 的工作依赖于另一个 agent 的输出时，暂停等待，不要让 agent 空转。
3. **结果收敛**: 当连续 2 轮探索没有发现新信息时，停止探索 agent。
4. **错误率过高**: 当某个 agent 连续 3 次修改导致测试失败，停止该 agent，由主 agent 接管。
5. **工作量不均衡**: 当某个 agent 完成速度是其他 agent 的 3 倍以上，将慢 agent 的剩余工作重新分配。

**回收策略**:
```
场景: 8 个 agent 并行转换测试
→ Agent 1-3 快速完成 → 回收，不等待
→ Agent 4-6 正常完成 → 回收
→ Agent 7 遇到困难 → 主 agent 接管其剩余工作
→ Agent 8 完成 → 回收
→ 最终: 运行测试验证所有转换
```

### 动态调度决策树

```
开始任务
├─ 任务是否可拆分为独立子任务？
│  ├─ 是 → 子任务数量？
│  │  ├─ 1-3 个 → 串行执行，不分配 agent
│  │  ├─ 4-10 个 → 分配 N 个 agent（N = min(子任务数, CPU核心数-2)）
│  │  └─ 10+ 个 → 分批处理，每批 8 个 agent
│  └─ 否 → 单 agent 执行
│
├─ 执行过程中
│  ├─ 某 agent 完成 → 立即回收，检查是否有新任务可分配
│  ├─ 某 agent 失败 3 次 → 回收，主 agent 接管
│  ├─ 所有 agent 空闲 → 进入下一阶段
│  └─ 某 agent 进度严重落后 → 重新分配其工作
│
└─ 阶段完成
   ├─ 运行测试验证
   ├─ 测试通过 → 进入下一阶段
   └─ 测试失败 → 分析失败原因，分配修复 agent
```

### Agent 通信协议

**分配任务时必须明确**:
1. 该 agent 负责的具体文件或函数列表
2. 预期输出格式（修改的文件列表、测试结果等）
3. 超时时间（默认 5 分钟无进展则回收）
4. 依赖关系（是否需要等待其他 agent 的结果）

**回收 agent 时必须收集**:
1. 完成状态（成功/失败/部分完成）
2. 修改的文件列表
3. 遇到的问题和决策
4. 未完成的工作（如有）

### 常见场景的 Agent 策略

| 场景 | 推荐策略 |
|------|----------|
| 批量测试转换 | 按文件分组，8 个 agent 并行 |
| Bug 调查 | 2-3 个 agent 从不同角度探索 |
| 架构重构 | 串行为主，关键路径并行 |
| 代码审查 | 2 个 agent 独立审查，取交集 |
| 性能优化 | 1 个 agent 分析，1 个 agent 实施 |
| 文档更新 | 单 agent 串行 |

### 反模式（避免）

1. **不要过度并行**: 不是所有任务都适合并行。有强依赖的任务串行执行。
2. **不要固定 agent 数量**: 根据实际进度动态调整，不要一开始就分配 8 个 agent 然后不管。
3. **不要忽略失败 agent**: 失败的 agent 必须被回收，其工作必须被重新分配或由主 agent 接管。
4. **不要重复工作**: 确保每个 agent 的工作范围不重叠，避免多个 agent 修改同一个文件。
5. **不要跳过验证**: 每个 agent 完成后必须验证其工作，不能假设"看起来对"就是对的。
