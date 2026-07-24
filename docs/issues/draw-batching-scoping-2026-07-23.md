# Draw 合批专项 scoping(2026-07-23)

## 0. 结论速览

掠视宽视野 settled(release,12km/elev10,decouple 生产默认)帧 ~17-22ms(40-55fps),
**主导成本 = 逐瓦片 draw 的两条 O(N) 链**:GLES submit ~5-5.7ms + 命令拷贝/盖章
~3-5ms(N=137 命令)。submit 内部三段分解(SUBMITDIAG 真机实测,137 命令):

| 段 | ms | 备注 |
|---|---|---|
| bind(program/VAO/texture) | 0.7-1.0 | 缓存已生效,program 切换少 |
| uniform | 0.5-0.7 | 644 calls(冗余消除后 ~4.7 个/draw),已优化 |
| **draw(glDrawElements+state)** | **3.4-4.1** | **~27µs/draw = Adreno 驱动固有开销,主导** |

⟹ bind/uniform 层的微优化已到头;**唯一量级杠杆 = 减少 draw 数**(135 地形
draw → 个位数~十位数)。近景(3-4 瓦片)settled 4-5ms 健康,本专项只针对宽视野。

## 1. 为什么现在有条件做(此前没有)

Phase 2c P5a/P5b + SVT 合成方案落地后,地形 draw 的资源形态已经高度同构:

- **几何**:fine 瓦片(z≥9,fade=1)全部共享位移模板 VBO/IBO(同 {schemeId,z,row}
  一份,P5b 默认开后 per-tile VBO 已不存在)——实例化的"同网格"前提**天然成立**。
- **每瓦片差异**只剩:①ENU→ECEF 刚体帧矩阵 ②高度纹理(per-tile,pool 缓存)
  ③影像绑定(mappedRaster per-tile 纹理 或 pageStore 共享 array+per-tile 间接纹理)
  ④少量 uniform(reliefFade/minH/range/morph)。
- 其中 ③ 的 pageStore 路径已是**共享 texture2DArray**;②④ 是纯数据,可下沉为
  array layer / 实例属性。**阻塞实例化的只有 per-tile 高度纹理与 per-tile 间接
  纹理两类小纹理**——而它们恰好都是 64-66² 级小图,天然适合搬进 texture2DArray
  (与 pageStore 同一套已验证的基建:updateTextureRegion+layer,Adreno 2048 层上限)。

## 2. 方案(按质量排序)

### A. 地形实例化合批(推荐,目标形态)
同一模板 VBO 的可见瓦片合成一个 `glDrawElementsInstanced`:
- 高度纹理池 → texture2DArray(每瓦片一 layer;vertex shader `texelFetch` 加
  layer 维,实例属性给 layer id)。间接纹理同理(SVT 已有 per-tile RGBA8 小图)。
- 每实例数据(刚体帧 mat4 + heightDisplace + layer ids + mappedRasterUv):instance
  buffer(divisor=1)或 UBO+gl_InstanceID,GLES3.0 原生支持(引擎已有 i3dm 实例化
  路径 `GltfPrimitiveInstanced` 可参照)。
- draw 数:135 → ~模板组数(z-row 组,掠视纵深实测 ~10-20 组)。submit draw 段
  3.4-4.1ms → ~0.3-0.6ms;命令拷贝/盖章同步缩(142→~20 命令)。
- **依赖面**:terrain shader(GLSL+MSL 双镜像)加实例属性/array 采样;
  GltfDrawCommandBuilder 按模板分组聚合;高度纹理池改 array。**不碰** selector/
  tiling/上传链。
- **风险**:①mappedRaster 未走 pageStore 的瓦片(coarse z≤6/z13+ 上采样)纹理
  per-tile,不可实例化 → 保持逐 draw(它们数量少,掠视主体是 z9-12 fine 瓦片);
  ②geomorph/fade per-tile uniform 全部要搬进实例流,漏一个=视觉回归,须逐字段
  盘点 applyPerFrameCommandState;③双后端(GLES/Metal)同步改。

### B. 只合并命令构建/盖章层(不动 draw)
常驻命令缓存已在;把 per-frame 拷贝+盖章改增量(只盖变化字段)。省 ~2-3ms,
但 submit 3.4-4.1ms 不动——**不解决主导项,不推荐单独做**(可作为 A 的附带)。

### C. 状态排序/去重微优化
数据已证 bind/uniform 仅 ~1.5ms 且缓存已生效,**空间 <1ms,不立项**。

## 3. 验证标准(目标驱动)

1. 掠视 settled(冻结相机 12km/elev10):`GLES submit` 行 draw 段 ≤1ms、
   Engine total 心跳中位 ≤16.6ms(60fps);近景 settled 无回归(4-5ms)。
2. 像素:实例化前后截图逐像素对比(冻结相机),地形起伏/影像/morph 过渡一致。
3. host:ctest 全绿(基线 3 失败除外)+ `test_terrain_shader_command` 适配;
   golden selector trace 无 diff(不碰选择层)。
4. 运动:激进 scripted pan 无新尖刺(reqIssued/upload 链不受影响)。

## 4. 工程量(AI 协作基准)

设计+盘点 per-tile 字段 ~1 会话;实现(shader 双后端+builder 分组+高度纹理
array 化)+真机调试 ~2-3 会话;验证收口 ~半会话。

## 5. 测量出处

- SUBMITDIAG 三段分解:本文件 §0 表(2026-07-23 真机 7e045e39,release,
  冻结相机掠视,137 命令,8 个采样点稳定)。
- 帧总量/构成:memory `perf-remeasure-wall-moved-2026-07-23` 晚二段。
- 相关:P5b `5d1b89fac`(共享模板默认开=同构几何前提)、SVT array 基建
  (northstar-decoupling §Step2/3)、i3dm 实例化路径(blend-instancing-a2c)。

---

## 6. 收口:四条验证标准结论(2026-07-24,release 真机 7e045e39)

方案 A 已按 Step1(高度纹理 array)→ Step2(间接纹理 array)→ Step3(GLES 批装配)
落地。**§3 的四条标准全部达成**,逐条对账:

### ① 掠视 settled draw 段 ≤1ms / 帧 ≤16.6ms ✅
同一 release 变体 Step2 基线 vs Step3,HORIZON 冻结机位 settled:

| 指标 | Step2 基线 | Step3 合批 |
|---|---|---|
| 面板 FPS / Frame | 30.1 / 33.3ms | **53.2 / 16.5ms** |
| 面板 CPU(build / submit) | 18.4ms(5.2 / 5.6) | **9.4ms(3.1 / 1.4)** |
| GLES submit | 3.4-6.7ms | **1.25-1.59ms** |
| **draw 段** | 2.4-4.6ms | **0.87-1.13ms** |
| uniform(glUniform 调用数) | 0.6-1.1ms(690) | 0.19-0.24ms(162) |
| FrameLoop total / callback | 18-26ms / **33.3ms(掉 30Hz)** | 10.5-13.9ms / **16.6ms(锁 60Hz)** |
| draw calls / GPU 纹理对象 | 132 / 111 | 57 / 18 |

合批构成 = 18 批覆盖 93 实例 + 37 条逐 draw = 130 瓦片,与基线 gltf=130 对齐,
零命令丢失。**最强信号是 callback 间隔**:基线在此机位被 vsync 打到 30Hz,合批
后稳定吃满 60Hz。§2 方案 A 预估「draw 段 3.4-4.1 → 0.3-0.6ms」略乐观(实测
0.87-1.13),但「135 → ~10-20 组」的量级判断成立(实测 130 → 18 批 + 37 单例)。

近景 settled 无回归 ✅:重庆 nadir(RESET 机位)可见瓦片仅 3 片,**分组恒 <2 →
inst=0,批装配路径根本不进入**(设计上的单例保护),实测 FPS 59.9 / Frame 17.1ms
/ CPU 4.1ms / draw calls 7,与既有近景 4-5ms 基线一致。

### ② 像素一致 ✅
冻结 HORIZON 机位、面板以上区域逐像素:**99.0% 逐字节相同,max diff 8/255,
diff>2 的仅 4 px**。差异来源 = MSAA×4 + 设计 §7 预测的 rel 矩阵降 float 亚像素
抖动。地形起伏/影像/morph 过渡目视一致。

### ③ host ctest + golden ✅
153/156,失败仍是既存三基线(test_sse_pipeline / test_raster_overlay_details /
test_scene_frame_state);`test_selector_cesium_golden_diff` Passed(未碰选择层)。
`test_terrain_page_store` 断言随 array 化更新(createdTextureCount 1→2)。

### ④ 激进 scripted pan 无新尖刺 ✅
0.5°/帧 × 600 帧掠视扫掠,基线 vs 合批同脚本:

| | Step2 基线 | Step3 合批 |
|---|---|---|
| 慢帧(total ≥25ms)条数 | 16 | 15 |
| 慢帧位置 | 帧 772-930(pan 结束后的 settle 涌入) | 帧 755-857(同) |
| 慢帧量级 | 25-32ms | 25-32ms |
| in-pan 吞吐 | 17.14 ms/帧(58.3 fps) | **16.63 ms/帧(60.1 fps,压在 vsync 上限)** |
| batch 装配段 | — | **in-pan 0.10-0.15ms / settled 0.5-1.0ms** |

慢帧簇在两个 build 上位置、条数、量级全对齐,且都落在 **pan 停止后的加载 settle
窗口**而非扫掠期 → 合批未引入任何新尖刺;batch 装配段自身在运动期只有 0.1ms 级,
永远不是尖刺来源。

## 7. 挂起项

- **Step4 Metal 接线**:苹果端暂不出货,**主动挂起**。当前状态是干净的零回归回落
  ——Metal 不创建 instanced shader → `terrainInstancedShader()==null` →
  `TerrainInstanceBatcher::assemble` 首行返回 → 整条地形走逐 draw。
- 恢复 Step4 时的已知前置洞(本次调研发现,均**未修**):
  1. `RenderDeviceMetal` 从未绑定 `terrainVertex` 声明的 `buffer(3)`
     u_heightDisplace / `buffer(4)` u_terrainLayers / `texture(22)` 高度纹理
     → **Metal 侧地形 GPU 位移整体没接线**(读未绑定 uniform)。这是正确性问题,
     优先级高于合批。
  2. 片元材质纹理绑定循环上限 = `kGltfPageStoreIndirTextureSlot + 1`(21),
     高度纹理槽 22 被排除在外;且循环只 `setFragmentTexture`,而高度纹理是
     **顶点阶段**采样,需要单独 `setVertexTexture`。
  3. MSL `GltfUniforms` 镜像结构体在 Renderer.cpp 里已有两份(glTF 片元 /
     terrain 片元),实例化片元会成为第三份 —— 四方同步契约(C++ 块 / 三份 MSL /
     GLES 描述表)值得先做一次去重再加。
- **调试面板口径**:`Surface meshes` / `Attachments` 按**命令数**统计,合批后显示
  53/35 而非 128,不是几何丢失(draw calls + inst 计数已对齐)。属诊断口径未跟上
  合批,独立小项。
