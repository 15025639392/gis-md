# 地形实例化合批设计(方案 A)——per-tile 字段盘点 + 实例流布局(2026-07-23)

承接 `draw-batching-scoping-2026-07-23.md` §2 方案 A。本文 = 工程量第一段
「设计 + 盘点 per-tile 字段」的产出:字段全量清单、每字段的实例化去向、
资格闸、分期实施与验证点。

## 0. 盘点方法与代码事实源

逐行走查了四个层次的 per-tile 状态写入点:

1. **常驻命令构建** `GltfDrawCommandBuilder.cpp::rebuildCachedDrawCommands`
   (内容不变式:几何/材质/water mask/模板 swap/高度纹理)
2. **每帧盖章** `applyPerFrameCommandState`(frameId/morph/clip/overlay 绑定/
   blend 派生/pageStore bind)
3. **每帧 uniform compose** `SceneRenderCommandUniformUpdater.cpp`
   (MVP 双精度 compose、lightDir/eyePositionRTC 变换到瓦片 ENU 帧)
4. **shader 消费面** `kTerrainVertexGLSL`/`kTerrainFragmentGLSL`(Renderer.cpp
   906-1174)——实例化目标 draw 全部走 terrainShader(模板 swap 后),故只需
   盘 terrain shader 的 uniform 集,gltfShader 的 PBR 大块无关。

**盘点中的关键事实修正**(比 scoping 假设更有利):`TerrainPageStore::
updateVisiblePages` 的过滤条件只有 `terrainSurfaceSourceForDraw == RealTerrain`
(TerrainPageStore.cpp:245)——**所有**可见真实地形瓦片(z9-12 全部,非只
capped z12)都建间接纹理走共享 array。settled 态页全驻留后,mappedRaster
fallback 不被采样 → 影像绑定对批内瓦片完全共享化。

## 1. 分组键与批形态

- **批键 = 模板身份 `{schemeId, z, row(y), gridSize}`**,与
  `TerrainDisplacementTemplatePool::cacheKey` 同源(同键瓦片共享同一模板
  VBO/IBO,「同网格」是 glDrawElementsInstanced 的硬前提)。
- 每批一条 instanced RenderCommand:模板 VBO/IBO + 每帧 orphan 的 instance
  buffer + terrainShaderInstanced(新 program)。
- **批参考帧 = 批内首实例瓦片的 ENU→ECEF 帧(frame0)**。批级
  `u_modelViewProjection = viewProj · frame0` 沿用
  SceneRenderCommandUniformUpdater 的双精度 compose 分支(语义不变,矩阵换
  frame0)。per-instance 相对帧 `rel_i = inverse(frame0) · frame_i` 在 CPU
  双精度算好降 float 进实例流。lightDir/eyePositionRTC 变换到 frame0 帧
  (批级),顶点 `N_batch = mat3(rel_i)·a_normal`、`pos_batch = rel_i·morphPos`
  ——刚体变换下位移/裙墙/双分辨率 morph 数学全部不变。

## 2. per-tile 字段全量盘点

「去向」列:**B**=批级(uniform,批内常量)、**I**=实例流字段、**A**=搬
texture2DArray + 实例 layer id、**G**=资格闸(不满足则该瓦片留逐 draw)。

### 顶点侧

| # | 字段 | 现载体 | 变化节奏 | 去向 |
|---|------|--------|----------|------|
| 1 | ENU→ECEF 刚体帧 | `cmd.terrainDisplacementModelMatrix`(16 double) | 内容 | **I**:rel_i 3×vec4(48B,第 4 行恒 0001) |
| 2 | u_modelViewProjection | gltfUniforms,每帧 CPU compose | 每帧 | **B**:viewProj·frame0 |
| 3 | u_geomorphUpFactor.xyz | 模板路径恒 (0,0,1)(builder:407) | 常量 | **B**(shader 写死局部 +Z 亦可) |
| 4 | u_geomorphUpFactor.w = morphFactor | 每帧盖章 = `selectionFrameState.terrainMorphFactor` | 每帧 | **I**:float |
| 5 | u_heightDisplace.xy = minH·fade, range·fade | 常驻(builder:416;fade 只依赖 z → 批内常量,minH/range per-tile) | 内容 | **I**:vec2(预乘 fade 后写入) |
| 6 | u_heightDisplace.z(enabled)/w(gridSize=64) | 常驻 | 常量 | **B**(批内恒 1 / 恒 kTerrainDisplacementGridSize) |
| 7 | 高度纹理(65² RGBA8,RG 打包 16bit) | `cmd.textures[22]`,per-tile,pool 缓存 | 内容 | **A**:池改 texture2DArray(§5),实例 heightLayer |
| 8 | skirt 哨兵 / 双分辨率 morph 采样 | 模板顶点属性 a_heightDelta / a_texcoord01 | — | 不变(模板共享) |

### 片元侧

| # | 字段 | 现载体 | 变化节奏 | 去向 |
|---|------|--------|----------|------|
| 9 | u_lightDir / u_ambient / u_eyePositionRTC | 每帧 compose,**变换到瓦片 ENU 帧**(Updater:76-115) | 每帧 | **B**:变换到 frame0 帧 |
| 10 | u_baseColor / u_hasBaseColorTexture / baseColorTexture | 常驻材质 | 内容 | heightmap DEM 恒无纹理;**G**:有 baseColorTexture 不进批;u_baseColor 批级(批内应同 = 默认白;不同则 G) |
| 11 | u_renderOpacity | 每帧;地形恒 1(applyPerFrame:475) | 每帧 | **B**(恒 1) |
| 12 | u_alphaMode / u_alphaCutoff / blend 态 | 地形恒 Opaque 不 blend(applyPerFrame:566 注释) | — | **B**;blend 地形理论不存在,防御性 **G** |
| 13 | mappedRaster ×4(纹理 + tileUv + opacity + texCoordSet) | 每帧盖章,per-tile 纹理指针 | 每帧 | **G**(核心闸):pageStore indir 本帧存在且**全 cell 驻留(A=255)**、且唯一 overlay 为 BaseImagery → fallback 不会被采样,批命令不绑 mappedRaster、count=0;否则逐 draw |
| 14 | pageStore array(slot 20) | 共享 texture2DArray | — | **B**(已共享) |
| 15 | pageStore 间接纹理(slot 21,gridN² RGBA8,gridN per-tile 1..64) | per-tile Texture(`tileIndirs_`) | 每帧重建 | **A**:固定 64² 层 array(kMaxDetDepthLevels=6 → gridN≤64),texel 写左上 gridN² 区;实例 indirLayer + gridN |
| 16 | u_pageStoreParams(x=enabled, y=gridN) | 每帧盖章 | 每帧 | x → **B**(批内恒 1,由 #13 闸保证);y → **I**(gridN float) |
| 17 | clipUv / clipEnabled(祖先回退裁剪窗) | 每帧;同一祖先可多 render entry 不同 clip(TileRenderPlanFinalizer:100) | 每帧 | **I**:vec4 + enabled 打包(每 render entry 一实例,天然支持一瓦多实例) |
| 18 | water mask(纹理 + translationScale + state) | 常驻;demo DEM 无元数据 | 内容 | **G**:hasWaterMask 不进批 |
| 19 | cullFace / depthTest / depthWrite | 常驻;地形恒 opaque/cull | — | **B** |
| 20 | frameId / generation / stableKey / sortCenter | 命令生命周期字段 | 每帧 | 批命令自带(transient,每帧重建,不进常驻缓存) |

### 实例流布局(草案,96B/instance,16B 对齐)

```
vec4 relRow0        // rel_i 行 0(行主序上三行,第 4 行恒 0001)
vec4 relRow1
vec4 relRow2
vec4 dispMorph      // x=minH·fade  y=range·fade  z=morphFactor  w=gridN
vec4 clipUv         // 默认 (0,0,1,1)
vec4 layers         // x=heightLayer  y=indirLayer  z=clipEnabled  w=保留
```

掠视 137 实例 ≈ 13KB/帧 orphan 上传,可忽略。GLES:新
`VertexLayoutKind::Terrain32Instanced`(模板属性 0-3 逐顶点 + 实例属性 4-9
divisor=1,仿现有 `Gltf120Instanced` attrib 3-9 模式,RenderDeviceGLES.cpp:
1447-1466);Metal:vertex descriptor 加 PerInstance layout(仿 mm:601)。

## 3. 资格闸(逐 draw 回落 = 零回归面)

瓦片(render entry)进批的充要条件——任一不满足即走现有逐 draw 路径,
常驻命令缓存原样保留:

1. RealTerrain + 模板 swap 成功(`hasTerrainDisplacementFrame`,即
   reliefFade>0.001 且高度纹理/模板 acquire 成功);
2. 无 baseColorTexture、无 water mask、非 blend(demo DEM 天然满足);
3. pageStore ready 且该瓦片本帧 indir 存在且**全 cell 驻留**、唯一
   overlay = BaseImagery(→ mappedRaster fallback 不会被采样)。

天然留在逐 draw 的:粗瓦片(fade≤0.001,gltfShader baked VBO)、fill/
ellipsoid 代理、上采样 z13+(baked VBO 无自有高度图)、页 page-in 未满的
瓦片(瞬态,数帧后自愈进批)。SVT flag 关闭 → 全部回落逐 draw,逐字节现状。

「全 cell 驻留」由 determination 重建 indir 时顺手记录(per-tile bool,零
额外遍历)。预期 settled 掠视:z9-12 fine 主体全数进批,draw 135 →
批 ~10-20 + 残余逐 draw 个位数。

## 4. 帧流程改动面

- **批装配(新)**:命令收集处按资格分流——eligible entries 按批键分组,
  每组产一条 instanced 命令(transient,每帧重建,不进 per-tile 常驻缓存);
  ineligible 走现路径。实例序即绘制序,批内无透明排序需求(恒 opaque)。
- **SceneRenderCommandUniformUpdater**:批命令复用
  `hasTerrainDisplacementFrame` 分支,矩阵 = frame0。
- **submit**:GLES/Metal 各加一个实例化地形布局;draw 调用走既有
  `glDrawElementsInstanced`/`drawIndexedPrimitives instanceCount` 路径。
- **不碰**:selector/tiling/上传链/loadQueue(scoping 承诺)。

## 5. 两处存储 array 化(先行,独立可验)

1. **高度纹理**:`TerrainDisplacementTemplatePool::heightCache_` per-tile
   Texture → 共享 texture2DArray(65×65 RGBA8 × N 层),层分配复用
   `TerrainPageLayerPool`(blockLayers=1,LRU)。**逐 draw 路径同步迁移**
   (terrainShader 加 `u_heightLayer` uniform,sampler2D → sampler2DArray,
   `eeSampleTerrainHeight` texelFetch 加 layer 维)——单一存储形态,不留
   per-tile/array 双轨债。
2. **间接纹理**:`TerrainPageStore::tileIndirs_` per-tile Texture → 共享
   64×64 RGBA8 array,同上;逐 draw 路径加 `u_indirLayer`,片元
   `indirUv=(cell+0.5)/64.0`(原 `/gridN`)。

风险与既有模式对齐:层淘汰与在用命令一致性 = layer id 每帧盖章 + 帧内
快照(pageStore 已验证的模式);层容量按峰值可见瓦片(~185)定,淘汰仅
在超峰值时发生。

## 6. shader 改动(GLSL + MSL 双镜像)

- `terrainShaderInstanced` 新 program:vertex 加实例属性(§2 布局),
  高度采样/间接采样走 array + 实例 layer;fragment 删 mappedRaster 分支
  (资格闸保证不需要)。
- 逐 draw `terrainShader` 只做 array 化改造(§5),功能等价。
- MSL 镜像同步;uniform 增删遵守 GltfUniformBlock 三方契约
  (C++/MSL/描述表,见 memory `uniform-block-refactor-done`)。

## 7. 精度分析(rel_i float 化的抖动界)

批内瓦片对 frame0 的平移 = 同 {z,row} 可见列跨度。最坏 z9(瓦片 ~78km)
掠视可见 ~6-8 列 ≈ 500km,float 24bit 尾数 → 位置量化 ~30mm;对 12km 视距
角误差 ≈ 2.5e-6 rad ≈ 0.005px,不可见。z10-12 更小。兜底:若真机像素对比
出现 jitter,frame0 改选批内离相机最近实例(误差随距离/尺寸同缩)。

## 8. 分期实施与验证点(目标驱动)

| 步 | 内容 | 验证 |
|----|------|------|
| 1 | 高度纹理 array 化(逐 draw 路径) | host ctest 全绿(基线 3 除外)+ 真机冻结相机逐像素对比无 diff |
| 2 | 间接纹理 array 化(逐 draw 路径) | 同上 + SVT 页点亮行为不变(page-in 逐帧点亮) |
| 3 | 批装配 + instanced shader,GLES 真机点亮 | SUBMITDIAG draw 段 ≤1ms;命令数 142→~25;冻结相机像素对比;近景 settled 无回归 |
| 4 | Metal 镜像 | macOS/iOS host 渲染冒烟 + golden |
| 5 | 收口 | scoping §3 四条全过:settled 中位 ≤16.6ms、像素一致、ctest+golden 绿、激进 pan 无新尖刺 |

步 1/2 独立可合入(纯存储迁移,风险小);步 3 是主体;每步落地即真机验,
不攒大包。
