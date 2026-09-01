# 高德面缺失 + 贴地重钳卡顿修复 — 2026-09-01

> 冻结记录(写完即归档)。本次会话修复了官方 Amap 场景的两类问题:
> ① 面缺失(地形掩码绑定被投影限制);② 运动/加载时贴地重钳的渲染线程卡顿尖峰。
> 相关判据:`docs/northstar/vector.md`(V2/V3/V30)、`docs/northstar/amap-vector.md`(A-V*)。
> 提交:`706fb156..32bf440c`(面缺失 + B2 + A + worker 冷填充)。

## 问题一:普通面(surface fill)缺失

### 症状
PHK110 真机:道路/POI/标注/建筑渲染正常,但普通面(水/绿地/地块)完全缺失。

### 根因(三层,源码核实)
1. **面依赖地形掩码合成**:`bakeOfficialSurfaceFill` ON 时几何 VectorFill 被抑制
   (`FeatureRenderLayer.cpp:1204-1210` 空 if 体),面全靠 terrain shader 的
   `u_terrainFillMask` 合成,无几何兜底。
2. **掩码绑定被投影限制**:掩码在 ancestor 顶替时只在 `height-remap` 成功路径绑定
   (`GltfDrawCommandBuilder.cpp:763`),`TileSurfaceClip::supportsTerrainHeightRemap`
   只接受 **Geographic** 投影。
3. **demo 用 WebMercator 地形**:NASA Terrain-RGB 是 WebMercator → remap 永不执行
   → 掩码永不绑 → 面缺失。此限制是正确约束(WebMercator clip UV 的 v 轴纬度非线性,
   直接复用会采错祖先 DEM 纬度区间,见 `TileSurfaceClip.h:35-38`)。

### 修复
1. **demo 接真实地形**(`MinimalGlobeDemoConfig`):NASA heightmap(kind=Heightmap),
   掩码 remap 路径有 DEM 可采样。
2. **掩码命令缓存失效修复**(`TileRenderContentState`):掩码纹理是 per-frame 绑定
   (GltfDrawCommandBuilder 的 per-frame 状态),不进缓存命令读取集;新增
   `markRetainedBytesChanged()` 只更新字节记账,不 bump `drawCommandReadSetRevision_`
   → 掩码上传不再强制命令重建。
3. **WebMercator remap 支持**(`TileSurfaceClip` + `GltfDrawCommandBuilder` +
   `GltfUniformBlock` + `Renderer` GLSL/MSL):新增 `forDescendantBoundsTileLocal`
   (地理仿射 clip UV),`u_heightClipUV` uniform 供高度采样用,影像仍用 overlay 投影
   `u.clipUv`。`supportsTerrainHeightRemap` 改为检查真实 DEM,接受 WebMercator。

### 验证
真机 PHK110:ancestor-clip 瓦 `plainClip=45→0`、`remap=0→45`,掩码绑定恢复;
面正常渲染。native 全量通过。

## 问题二:贴地重钳(reclamp)渲染线程卡顿

### 症状
真机 PHK110 运动/加载时帧时间尖峰 679-3688ms;`Scene.render.buildBreakdown` 的
`vector`(矢量命令构建)暴涨,`FeatureLayerPerf amap-vector reclamp` 单桶 100-900ms。

### 根因
`reclampTileBucketLines` 对**每个线顶点**全量重采样高度 + 重建顶点缓冲。路网瓦顶点
极多(最大 3.8 万),每次地形换代:
- 逐顶点 `cartographicToCartesian`(三角函数 ECEF 变换)= 168ms(3.8万顶点);
- 每顶点 3 次采样(pos/prev/next),且 prev/next 引用相邻顶点 → 3× 重复采样;
- 新瓦片首次(冷填充)算 k/normal 三角函数 = 229ms,在渲染线程阻塞。

### 修复(三阶段,每阶段独立提交)
1. **B2 自持化**(`721af2dd`):`DecodedHeightmap` unique→shared_ptr<const>,
   `HeightSource` 自持(shared_ptr heightmap + 值 bounds),对齐已有
   `shared_ptr<const TerrainEdgeLutTable>` 先例。worker 可安全持有采样数据。
2. **A 降成本**(`28cfeed3`):缓存每唯一 (lon,lat) 的 (k, normal),reclamp 用
   `k + normal*height`(一次乘加);本帧采样去重(每唯一位置一次,非 3×)。
3. **worker 冷填充**(`32bf440c`):worker(tessellation)算 k/normal 存
   `FeatureTileMesh.lineClampSurfaceCache`,commit 存桶,渲染线程 reclamp 无冷填充。

### 验证
真机 PHK110:reclamp 229ms(n=1)→ ~9ms;运动帧大部分 `build=17-29ms`(原 679-3688ms)。
native 全量通过。

## 关键取舍与遗留

- **WebMercator remap**:为掩码绑定支持 WebMercator,分离了"高度采样 clip UV(地理仿射)"
  与"影像 clip UV(overlay 投影)",改动跨 GLSL/MSL 双份 shader + uniform ABI。
- **冷填充 worker 化**:三角函数从渲染线程移到 tessellation worker,不阻塞渲染线程;
  数据结构贯穿 tessellation→mesh→commit→桶。
- **遗留**:偶发帧尖峰(f560 291ms / f711 116ms)是**命令构建**(`vector`,3 层
  buildRenderCommands 总和),非 reclamp —— 独立问题,未处理。
- **上游复核**:cesium-native/openglobus 参考源码缺失(路径不存在),对齐结论来自
  web 调研(cesium `GroundPolylinePrimitive` 烘焙不更新、MapLibre GPU 位移+纹理拖拽)
  + 项目既有 `.ref` 记录,置信度:cesium/MapLibre 高、OpenGlobus 中。
