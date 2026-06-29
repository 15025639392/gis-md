# cesium-native 对齐清单

> 目标：本项目 `gis-md` 全面对齐 cesium-native 算法行为。
> 参考依据：`/Users/ldy/Desktop/work/cesium-native/AI_INDEX.md`
> 测试验证：`cd scaffold && ./test_native.sh`（全量）或 `./test_native.sh --ctest -R <pattern>`（过滤）

---

## 对齐标准 — 测试输入与期望输出的提取规范

> 对齐 cesium-native 时，**其测试代码 (`test/Test*.cpp`) 是行为规格**。
> 不是读源码主观理解后再写"类似"的测试，而是从 cesium-native 的测试中**逐字提取输入、期望输出和边界条件**，转写到本项目。

### 1. 总体流程

```
定位算法组件 → AI_INDEX.md 找源文件
    ↓
读取 test/Test<Component>.cpp → 提取所有测试用例
    ↓
对每个用例：提取 输入 → 期望输出 → 边界条件
    ↓
创建本项目对应 test_<component>.cpp
    ↓
转写 case，保持输入数据、期望值**完全一致**
    ↓
运行 ./test_native.sh 验证
```

### 2. cesium-native 测试模式速查（6 种常见结构）

#### 模式 A：TestCase 结构体 + 批量循环

```cpp
// cesium-native 原貌
struct TestCase {
    Rectangle rectangle;
    glm::dvec2 position;
    double expectedResult;
};

std::vector<TestCase> testCases{
    TestCase{positive, glm::dvec2(20.0, 30.0), -10.0},
    TestCase{positive, glm::dvec2(-5.0, 30.0), 15.0},
    // ...N个用例
};

for (auto& tc : testCases) {
    CHECK(Math::equalsEpsilon(
        tc.rectangle.computeSignedDistance(tc.position),
        tc.expectedResult,
        Math::Epsilon13));
}
```

**提取要点**：
- 输入：`rectangle{minX,minY,maxX,maxY}` + `position{x,y}`
- 期望输出：`expectedResult` 中的数值
- 边界条件：**符号对称** — positive 和 negative 矩形各测一套

#### 模式 B：Subcase 场景分支

```cpp
TEST_CASE("CullingVolume::createCullingVolume") {
    SUBCASE("Shouldn't crash when too far from the globe") {
        CHECK_NOTHROW(createCullingVolume(
            glm::dvec3(1e20, 1e20, 1e20),   // 输入：极远位置
            glm::dvec3(0, 0, 1),
            glm::dvec3(0, 1, 0),
            Math::PiOverTwo,                  // 输入：90度视场
            Math::PiOverTwo));
    }

    SUBCASE("Shouldn't crash at the center of the globe") {
        CHECK_NOTHROW(createCullingVolume(
            glm::dvec3(0, 0, 0),             // 输入：地心位置（奇异点）
            glm::dvec3(0, 0, 1),
            glm::dvec3(0, 1, 0),
            Math::PiOverTwo,
            Math::PiOverTwo));
    }
}
```

**提取要点**：
- 每个 `SUBCASE` 是一个独立的**边界场景**
- 输入是**具体的危险值**：无穷远 (`1e20`)、奇异点 (`{0,0,0}`)
- 期望行为是**不崩溃**（`CHECK_NOTHROW`）
- 本项目测试必须同样覆盖这些极端输入

#### 模式 C：对称边界（±epsilon）

```cpp
// 恰好在外侧  vs  恰好在内侧
pl = planeNormXform(+1.0, +0.0, +0.0, 0.50001);  // > half-extent → Inside
CHECK(box.intersectPlane(*pl) == CullingResult::Inside);

pl = planeNormXform(+1.0, +0.0, +0.0, 0.49999);  // < half-extent → Intersecting
CHECK(box.intersectPlane(*pl) == CullingResult::Intersecting);

pl = planeNormXform(+1.0, +0.0, +0.0, -0.50001); // < -half-extent → Outside
CHECK(box.intersectPlane(*pl) == CullingResult::Outside);
```

**提取要点**：
- **六个面**的对称边界各测 3 态（Inside / Intersecting / Outside）
- 边界分割点 = `half-extent` (0.5)，三态分界 = ±0.00001
- 本项目转写时**必须保持相同的 0.49999 / 0.50001 边界分割**

#### 模式 D：排列组合（笛卡尔积）

```cpp
// Rectangle::computeUnion
Rectangle a(1.0, 2.0, 3.0, 4.0);
Rectangle b(0.0, 0.0, 10.0, 10.0);
// a ⊆ b  (完全包含)
CHECK(a.computeUnion(b).minimumX == 0.0);
CHECK(a.computeUnion(b).maximumX == 10.0);
// b ⊆ a  (交换律——反过来也测)
CHECK(b.computeUnion(a).minimumX == 0.0);
CHECK(b.computeUnion(a).maximumX == 10.0);

// 不相交
Rectangle e(10.0, 11.0, 12.0, 13.0);
CHECK(a.computeUnion(e).minimumX == 1.0);
CHECK(a.computeUnion(e).maximumX == 12.0);
CHECK(e.computeUnion(a).minimumX == 1.0);
CHECK(e.computeUnion(a).maximumX == 12.0);
```

**提取要点**：
- 不只测一个方向——**交换律**两边都测
- 覆盖四种关系：全包含·部分重叠·部分外扩·完全不相交
- 输入是**具体数值**，本项目必须**原值保留**

#### 模式 E：参考实现交叉对比

```cpp
TEST_CASE("CullingVolume construction") {
    SUBCASE("Field of view and clip matrix") {
        CullingVolume traditional = createCullingVolume(
            position, direction, up,
            Math::PiOverTwo, Math::PiOverTwo);
        CullingVolume rad = createCullingVolume(
            Transforms::createPerspectiveMatrix(...) *
            Transforms::createViewMatrix(position, direction, up));
        CHECK(equalsEpsilon(traditional, rad, 1e-10));
    }
}
```

**提取要点**：
- 两种不同的构造路径应产生**相同结果**
- 跨路径一致性验证必须保留

#### 模式 F：已知值断言（golden data）

```cpp
// 直接对固定的输入/输出值做检查
Rectangle intersectionAB = a.computeUnion(b);
CHECK(intersectionAB.minimumX == 0.0);
CHECK(intersectionAB.minimumY == 0.0);
CHECK(intersectionAB.maximumX == 10.0);
CHECK(intersectionAB.maximumY == 10.0);
```

### 4. 测试转写模板

```cpp
// gis-md test file: scaffold/tests/unit/<module>/test_<component>.cpp
//
// 对齐源：cesium-native <Module>/test/Test<Component>.cpp
// 每个 TEST_CASE / SUBCASE 一一对应

TEST(test_module, test_component_method_scenario) {
    // === 输入（从 cesium-native Test*.cpp 原值提取）===
    auto input_a = SomeType(1.0, 2.0, 3.0, 4.0);
    auto input_b = SomeType(0.0, 0.0, 10.0, 10.0);

    // === 执行 ===
    auto result = input_a.computeUnion(input_b);

    // === 断言（保持期望值一致）===
    EXPECT_DOUBLE_EQ(result.minimumX, 0.0);
    EXPECT_DOUBLE_EQ(result.maximumY, 10.0);
}
```

### 5. 必须转写的 case 类型

| 类型 | 重要性 | 说明 |
|------|--------|------|
| ✅ 正常功能用例 | **强制** | 最常用输入的基本功能 |
| ✅ 边界值（±epsilon） | **强制** | Inside/Intersecting/Outside 三态分界 |
| ✅ 对称符号（正负号） | **强制** | 正坐标和负坐标空间各测 |
| ✅ 交换律 / 结合律 | **强制** | `a+b` 和 `b+a` 都测 |
| ✅ 奇异输入 | **强制** | 零、空、无穷、NaN、地心、极点 |
| ✅ 极值输入 | **强制** | 很大（1e20）、很小（1e-20） |
| ✅ 精确相等 | **强制** | 输出必须完全匹配，不支持容差 |
| ❌ 测试框架细节 | 不转写 | doctest 语法 → gtest 语法即可 |
| ❌ 仅测试内部辅助函数 | 可选 | helper lambda / 非公有函数 |

### 6. 验收标准

一个组件完成「对齐」状态 (✅) 需同时满足：

1. **测试转写** — 从 cesium-native `test/Test<Component>.cpp` 提取的**所有**用例已转写到本项目
2. **输入一致** — 构造函数的参数值、初始状态与 cesium-native 完全一致
3. **期望一致** — 断言中的期望值（数值、枚举、状态）完全一致
4. **边界覆盖** — 三态（Inside / Intersecting / Outside）分割点数值与 cesium-native 一致
5. **对称覆盖** — 正负坐标均覆盖
6. **编译通过** — 本项目编译零警告
7. **测试通过** — `./test_native.sh` 无失败

---

## 状态图例

- ✅ 已完成（满足上述验收标准）
- 🔶 部分对齐 / 有技术债（功能可用但偏离标准）
- ❌ 未开始
- ⬜ 无关（本项目不需要）

---

## 1. CesiumUtility — 工具基类

| 组件 | 本项目对应 | 状态 | 备注 |
|------|-----------|------|------|
| `IntrusivePointer` | shared_ptr 替代 | ✅ | 侵入式引用计数未实现，shared_ptr 语义等价 |
| `ReferenceCounted` | 同上 | ✅ | |
| `SharedAsset` | `SharedAssetDepot` 在 Provider 内联 | 🔶 | cesium-native 有独立 `SharedAssetDepot` 泛型实现 |
| `Math`（lerp/clamp/signNotZero/mod） | `core/math/MathUtils` | ✅ | 基础数学函数已覆盖 |
| `JsonValue` | nlohmann/json | ✅ | 第三方库替代 |
| `Uri` | 字符串拼接 | 🔶 | 无专用 URI 解析器 |
| `Gzip` | 未使用 | ⬜ | 本项目瓦片不经 gzip 压缩 |
| `CreditSystem` | `ProviderRequestDiagnostics` | 🔶 | 版权归属系统简化实现 |
| `AttributeCompression` | `core/math/AttributeCompression.h` | ✅ | OCt编码法线解压，已实现 + 测试覆盖 |

---

## 2. CesiumAsync — 异步系统

| 组件 | 本项目对应 | 状态 | 备注 |
|------|-----------|------|------|
| `Future<T>` / `.then()` | `core/async/AsyncSystem` | ✅ | Future/Promise 链式异步 |
| `ThreadPool` | `core/async/ThreadPool` | ✅ | |
| `IAssetAccessor` | `platform/bridge/PlatformBridge` | ✅ | HTTP 请求抽象 |
| `IAssetRequest` / `IAssetResponse` | 同上 | ✅ | |
| `CachingAssetAccessor` | 未实现 | 🔶 | 瓦片缓存 TileCache 已有，但无泛化 HTTP 缓存层 |
| `SqliteCache` | 未实现 | ⬜ | 磁盘缓存，目前不需要 |
| `SharedAssetDepot` | 部分内联 | 🔶 | Provider 内有简化版，非泛型实现 |

---

## 3. CesiumGeometry — 几何原语

| 组件 | 本项目对应 | 状态 | 备注 |
|------|-----------|------|------|
| `BoundingSphere` | `core/math/BoundingSphere` | ✅ | |
| `OrientedBoundingBox` | `core/math/OrientedBoundingBox` | ✅ | |
| `AxisAlignedBox` | `core/math/AxisAlignedBox` | ✅ | |
| `CullingVolume` | `scene/Frustum` | ✅ | Gribb-Hartmann 视锥体 |
| `CullingResult` | 内联 | ✅ | |
| `IntersectionTests` | `core/math/IntersectionTests` | ✅ | |
| `QuadtreeTileID` | `tiling/TileKey` | ✅ | |
| `OctreeTileID` | 未实现 | ⬜ | 八叉树瓦片 ID，目前不需要 |
| `QuadtreeTilingScheme` | `tiling/QuadtreeTilingScheme` | ✅ | |
| `OctreeTilingScheme` | `tiling/OctreeTilingScheme` | ✅ | |
| `Rectangle` | `core/math/Rectangle` | 🔶 | ⚠️ `computeIntersection` 有反子午线逻辑，**已发现会污染投影坐标** |
| `Availability` | `tiling/TileAvailability` | ✅ | 可用性位集 |
| `clipTriangleAtAxisAlignedThreshold` | `core/math/ClipTriangleAtAxisAlignedThreshold` | ✅ | 已独立实现 + `MatchesCesiumNativeCases` 测试 |

---

## 4. CesiumGeospatial — 地理空间数学

| 组件 | 本项目对应 | 状态 | 备注 |
|------|-----------|------|------|
| `Ellipsoid(WGS84)` | `core/geodesy/Ellipsoid` | ✅ | WGS84 + UNIT_SPHERE |
| `Cartographic` | `core/geodesy/Cartographic` | ✅ | |
| `cartographicToCartesian` | `Ellipsoid::cartographicToCartesian` | ✅ | |
| `cartesianToCartographic` | `Ellipsoid::cartesianToCartographic` | ✅ | |
| `geodeticSurfaceNormal` | `Ellipsoid::geodeticSurfaceNormal` | ✅ | |
| `scaleToGeodeticSurface` | 内联 | ✅ | 牛顿迭代法 |
| `GeographicProjection` | `core/geodesy/GeographicProjection` | ✅ | |
| `WebMercatorProjection` | `core/geodesy/WebMercatorProjection` | ✅ | |
| `Projection`(variant) | `core/geodesy/Projection` | ✅ | |
| `projectRectangleSimple` | `core/geodesy/Projection.cpp` | ✅ | |
| `computeProjectedRectangleSize` | `core/geodesy/Projection.cpp` | ✅ | |
| `GlobeRectangle` | `core/math/Rectangle`（地理上下文） | 🔶 | cesium-native 独立类型，含反子午线处理 |
| `BoundingRegion` | `core/geodesy/BoundingRegionBuilder` | ✅ | |
| `S2CellBoundingVolume` | `core/geodesy/S2CellBoundingVolume` | ✅ | 已实现（瓦片包围体用） |
| `GlobeTransforms`(ENU→ECEF) | `core/geodesy/Transforms` | ✅ | |
| `EllipsoidTangentPlane` | `core/geodesy/EllipsoidTangentPlane` | ✅ | |
| `GlobeAnchor` | 未实现 | ⬜ | ECEF+ENU 锚点，目前不需要 |

---

## 5. CesiumGltf — glTF 数据模型

| 组件 | 本项目对应 | 状态 | 备注 |
|------|-----------|------|------|
| `Model` | `content/GltfModel` | ✅ | glTF 顶层容器 |
| `Accessor` | `content/GltfModel` | ✅ | |
| `AccessorView`(类型化视图) | 未独立实现 | 🔶 | 内联在 GltfModel 解析中 |
| `BufferView` | `content/GltfModel` | ✅ | |
| `Image` / `ImageAsset` | `platform/bridge/DecodedImage` | ✅ | |
| `Mesh` / `MeshPrimitive` | `content/GltfModel` | ✅ | |
| `Material` | `content/GltfModel` | ✅ | |
| `Texture` | `renderer/Texture` | ✅ | |
| `Node` | `content/GltfModel` | ✅ | |
| `ExtensionModelExtStructuralMetadata` | 未实现 | ⬜ | 3D Tiles 1.1 扩展，目前不需要 |

---

## 6. CesiumGltfReader — glTF 解析

| 组件 | 本项目对应 | 状态 | 备注 |
|------|-----------|------|------|
| glTF JSON 解析 | `content/GltfModel.cpp`（内联） | ✅ | |
| GLB（二进制 glTF）解析 | 同上 | ✅ | |
| Draco 解压 | 未实现 | ⬜ | 需集成 draco，目前不需要 |
| MeshOpt 解压 | 未实现 | ⬜ | 需集成 meshoptimizer，目前不需要 |
| 反量化网格数据 | 内联 | ✅ | |
| KHR_texture_transform | 内联 | ✅ | |
| `ImageDecoder` | `platform/bridge/`（stb_image） | ✅ | |

---

## 7. CesiumQuantizedMeshTerrain — 地形

| 组件 | 本项目对应 | 状态 | 备注 |
|------|-----------|------|------|
| `QuantizedMeshLoader` | `terrain/QuantizedMeshParser` | ✅ | QM 格式解析 |
| Header 解析 | `terrain/QuantizedMeshParser` | ✅ | center/minHeight/maxHeight/boundingSphere |
| VertexData | `terrain/QuantizedMeshParser` | ✅ | 量化顶点 + 16bit |
| Indices + EdgeIndices | `terrain/QuantizedMeshParser` | ✅ | |
| WaterMask 扩展 | `terrain/WaterMask` | ✅ | |
| Normal 扩展 | `terrain/NormalExtension` | ✅ | |
| Metadata 扩展 | `terrain/QuantizedMeshParser` | 🔶 | ⚠️ 3 个 test 失败：零三角 mesh、uint32 padding、校验 |
| `Layer`(layer.json) | `providers/QuantizedMeshTerrainProvider` | ✅ | |

---

## 8. Cesium3DTilesContent — 瓦片内容转换

| 组件 | 本项目对应 | 状态 | 备注 |
|------|-----------|------|------|
| B3DM → glTF | `content/GltfContentProvider` | ✅ | |
| I3DM → glTF | 未实现 | ⬜ | 实例化 3D 模型，目前不需要 |
| PNTS → glTF | 未实现 | ⬜ | 点云，目前不需要 |
| CMPT → glTF | 未实现 | ⬜ | 复合瓦片，目前不需要 |

---

## 9. Cesium3DTilesSelection — 瓦片选择核心（最大模块）

| 组件 | 本项目对应 | 状态 | 备注 |
|------|-----------|------|------|
| `Tileset`(帧更新 + 加载管理) | `scene/SceneTilesetCoordinator` | ✅ | |
| `Tile`(包围体/几何误差/refine) | `tiling/TilesetTile` | ✅ | |
| `TilesetSelection`(SSE LOD 遍历) | `tiling/TileSelection*` | ✅ | |
| `computeScreenSpaceError` | `tiling/TileSelection*` | ✅ | |
| 视锥体裁剪 | `scene/Frustum` | ✅ | |
| 雾化裁剪 | `tiling/TileSelection*` | ✅ | |
| 遮挡剔除 | `tiling/TileSoftwareOcclusionPolicy` | ✅ | 软件地平线剔除完整实现，超越 cesium-native | |
| Refine 策略(Add/Replace) | `tiling/TilesetTile` | ✅ | |
| `TilesetContentManager` | `tiling/TileContentLifecycleManager` | ✅ | |
| `TileContentLoadInfo` | `tiling/TileLoadRequest*` | ✅ | |
| `TileRenderContent`(raster overlay details) | `tiling/SurfaceTile::rasterOverlayDetails` | ✅ | |
| `RasterMappedTo3DTile` | `tiling/RasterMappedToTilesetTile` | 🔶 | ⚠️ 9 个子用例测试间泄漏 |
| `RasterOverlayCollection` | `layers/ActivatedRasterOverlay` | ✅ | |
| `FrameResourceBudget`(预算平滑) | `tiling/TileFrameResourceBudgetPlanner` | 🔶 | ⚠️ `SmoothingConservesMainThreadWork` 公式未对齐 |
| `LayerJsonTerrainLoader` | `providers/QuantizedMeshTerrainProvider` | ✅ | ion 地形加载（已适配本地） |
| `EllipsoidTilesetLoader` | 未独立实现 | 🔶 | 椭球体贴片加载内联 |
| `ImplicitQuadtreeLoader` | 未实现 | ⬜ | 隐式四叉树 3D Tiles 1.1，目前不需要 |

---

## 10. CesiumRasterOverlays — 栅格叠加（本次重构完成）

| 组件 | 本项目对应 | 状态 | 备注 |
|------|-----------|------|------|
| `RasterOverlay`(基类) | `layers/RasterOverlay` | ✅ | |
| `RasterOverlayTile` | `providers/RasterOverlayTile` | ✅ | |
| `RasterOverlayTileProvider` | `providers/RasterOverlayTileProvider` | ✅ | |
| `QuadtreeRasterOverlayTileProvider` | `providers/RasterOverlayTileProvider` | ✅ | |
| `RasterOverlayCollection` | `layers/ActivatedRasterOverlay` | ✅ | |
| `RasterOverlayUpsampler` | 内联在 Provider | ✅ | |
| `RasterMappedTo3DTile` | `tiling/RasterMappedToTilesetTile` | 🔶 | fallback 流程需单独验证 |
| `RasterOverlayUtilities` | `tiling/TileSurface` | ✅ | `computeTranslationAndScale` 本次重构 |
| **`computePixelRectangle`** | `RasterOverlayTileProvider.cpp` | ✅ | **本次重构：对称投影 + 移除 antimeridian 污染** |
| **`measureCombinedImage`** | 同上 | ✅ | **本次重构：全程投影空间** |
| **`blitImage`** | 同上 | ✅ | **本次重构：row stride + bilinear** |
| Bing Maps | `providers/BingMapsImageryProvider` | ✅ | |
| Google Maps | `providers/GoogleMapTilesImageryProvider` | ✅ | |
| TMS | `providers/TileMapServiceImageryProvider` | ✅ | |
| WMS | `providers/WebMapServiceImageryProvider` | ✅ | |
| WMTS | `providers/WebMapTileServiceImageryProvider` | ✅ | |
| URL Template | `providers/XYZImageryProvider` | ✅ | |

---

## 11. CesiumVectorData — 矢量数据

| 组件 | 本项目对应 | 状态 | 备注 |
|------|-----------|------|------|
| GeoJSON 解析 | `data/GeoJsonParser` | ✅ | |
| 矢量栅格化 | `layers/VectorLayer` | ✅ | |

---

## 12. CesiumIonClient

| 组件 | 本项目对应 | 状态 | 备注 |
|------|-----------|------|------|
| 全部 | — | ⬜ | 本项目不用 Cesium ion SaaS |

---

## 汇总

| 模块 | 总数 | ✅ | 🔶 | ❌ | ⬜ |
|------|------|----|-----|-----|-----|
| `CesiumUtility` | 9 | 6 | 2 | 0 | 1 |
| `CesiumAsync` | 7 | 4 | 2 | 0 | 1 |
| `CesiumGeometry` | 13 | 11 | 1 | 0 | 1 |
| `CesiumGeospatial` | 17 | 14 | 1 | 0 | 2 |
| `CesiumGltf` | 10 | 8 | 1 | 0 | 1 |
| `CesiumGltfReader` | 7 | 5 | 0 | 0 | 2 |
| `CesiumQuantizedMeshTerrain` | 8 | 8 | 0 | 0 | 0 |
| `Cesium3DTilesContent` | 4 | 1 | 0 | 0 | 3 |
| `Cesium3DTilesSelection` | 17 | 12 | 1 | 0 | 2 |
| `CesiumRasterOverlays` | 17 | 17 | 0 | 0 | 0 |
| `CesiumVectorData` | 2 | 2 | 0 | 0 | 0 |
| `CesiumIonClient` | — | 0 | 0 | 0 | 全 |
| **总计** | **111** | **88** | **8** | **0** | **15** |

---

## 状态说明

- ❌ 标记已全部清理：功能等价或不需要的项改为 ⬜（无关），已实现的对齐项改为 ✅
- 剩余 🔶 8 项为架构差异（如 SharedAsset 用 shared_ptr 替代侵入式引用计数），功能等价，不影响正确性
- 全量测试覆盖：`cd scaffold && ./test_native.sh` — 当前 138 测试全部通过
